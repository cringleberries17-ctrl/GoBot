#include "gobot/auto_chat.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"
#include "gobot/routine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>

#include <windows.h>
#include <tlhelp32.h>

namespace gobot {

namespace {

// ---- what is said, in Vietnamese ------------------------------------------

const char* const kNoWindow   = "Chưa thấy cửa sổ game.";
const char* const kNotFront   = "Game không ở trên cùng — không gõ để khỏi gõ nhầm vào cửa sổ khác.";
const char* const kKeeper     = "Đang tự đăng nhập lại — chat sau.";
const char* const kTrash      = "Đang dọn rác — chat sau.";
const char* const kUserActive = "Bạn đang dùng chuột hoặc bàn phím — chat sau.";
const char* const kNoHud      = "Nhân vật chưa ở trong game (đang tải, đã chết hoặc mất kết nối) — "
                                "chat sau.";
const char* const kLowHp      = "Máu thấp — chờ 2 giây rồi chat.";
const char* const kBarOpen    = "Ô chat đang mở — có thể bạn đang gõ, để sau 15 giây.";
const char* const kNoBar      = "Chưa thấy ô chat ở góc trái dưới màn hình game. Đừng để cửa sổ nào che "
                                "khung chat.";
std::string not_opened(int enters) {
    return "Không mở được ô chat (đã bấm Enter " + std::to_string(enters) + " lần) — bỏ qua câu này.";
}
const char* const kNotClosed  = "Không đóng được ô chat sau khi gửi — Tự chat đã dừng.";
const char* const kNotKnown   = "Bấm Enter xong thì ô chat không giống hình ô chat đang mở hay đang đóng — "
                                "Tự chat đã dừng. Ảnh màn hình lúc đó ở thư mục chat_debug.";
const char* const kLostOpening = "Không thấy ô chat sau khi bấm Enter — câu chưa gửi.";
const char* const kNotCleared = "Ô chat còn chữ không xoá được — câu chưa gửi.";
const char* const kShutTyping = "Ô chat tự đóng lại khi đang gõ — câu chưa gửi.";
const char* const kLostTyping = "Không thấy ô chat khi đang gõ — câu chưa gửi.";
const char* const kMaybeLeft  = "Ô chat có thể còn chữ chưa gửi.";
const char* const kNoShot     = "Chưa chụp được màn hình game.";
const char* const kStopped    = "Đã dừng giữa chừng.";
const char* const kNothing    = "Câu không còn chữ nào gõ được.";

std::string dialog_wait(const std::string& which) {
    return "Đang có hộp thoại trong game (" + which + ") — chờ hộp thoại đóng.";
}

// ---- the chat bar, as the commercial client finds it -----------------------
//
// From its IsChatModeActive (go_client.exe, at 0x140100CB0), and its numbers.

/// The corner it searches: the bottom-left 401 by 145 pixels of the screen.
constexpr int kCornerW = 401, kCornerH = 145;
/// Its first patch of the open picture, the divider between the bar's two
/// boxes: 4 by 18 at (80, 5).
constexpr int kDividerX = 80, kStripY = 5, kDividerW = 4, kStripH = 18;
/// Its second, the bar's left edge, 3 by 18 at (3, 5) of the picture: 77
/// pixels left of the first, each channel within 15 of the picture.
constexpr int kEdgeX = 3, kEdgeW = 3, kEdgeDx = kDividerX - kEdgeX;
constexpr int kStripSlack = 15;
/// The field, from the first patch: 308 by 15, starting 4 right and 2 up --
/// the inside of the picture's right box, exactly.
constexpr int kFieldDx = 4, kFieldDy = -2, kFieldW = 308, kFieldH = 15;
/// The open field's grey. The commercial client calls the line open when one
/// pixel of the field is exactly this; a hundred, each channel within 3, says
/// the same without hanging on one pixel. A shut field has none at all.
constexpr int kOpenGrey = 49, kGreySlack = 3, kOpenAtLeast = 100;
/// A pixel this far from that grey, in any channel, is something drawn on the
/// field: the caret, or a letter.
constexpr int kInkFrom = 40;
/// Where the closed picture sits, from the first patch: measured on every
/// frame saved from the live client, the field at (85, 698) with the divider
/// at (80, 700).
constexpr int kShutDx = 5, kShutDy = -2;

// ---- the sender's timing ------------------------------------------------------

/// Keys typed between looks for a dialog that came up meanwhile. The bar
/// itself is looked at before every key; the dialogs take a quarter of a
/// second to look for, which every key cannot afford.
constexpr int kKeysPerLook = 12;
/// The commercial client's gap between keys, which the look before each key
/// is taken in.
constexpr int kKeyGap = 33;
// The Enters pressed to open the line are ChatSettings::max_enters: two as
// shipped, not the commercial client's three -- a game slow enough to take
// them all late ends up with the line as it found it, shut, after an even
// number.
/// Long enough for the game to draw the keys just pressed before the field is
/// looked at again.
constexpr int kDrawnMs = 150;
/// How long after an Enter that may have opened the line a look showing it
/// shut is believed. The game takes a key a frame or more after it is pressed
/// and draws what it did a frame after that, so a look taken sooner can show
/// the line as it was before the Enter: shut, a moment before it opens.
constexpr int kSettleMs = 750;
/// Backspaces enough for whatever `px` pixels of the field can hold: the
/// narrowest letters the game draws, i and l, are a pixel wide with one
/// between them, so no more than one character for every two pixels.
int rub_for(int px) { return std::max(0, px) / 2 + 2; }
/// The most Backspaces pressed at once: more than the longest line has.
constexpr int kMostRubbed = ChatSettings::kLongest + 20;

int deviation(const Frame& f, int x, int y, int grey) {
    const std::uint8_t* p = &f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4];
    return std::max({ std::abs(p[0] - grey), std::abs(p[1] - grey), std::abs(p[2] - grey) });
}

/// The strip of `t` at (tx, ty), w by h, is in `f` at (fx, fy), each channel of
/// every pixel within kStripSlack.
bool strip_at(const Frame& f, int fx, int fy, const Frame& t, int tx, int ty, int w, int h) {
    if (fx < 0 || fy < 0 || fx + w > f.width || fy + h > f.height) return false;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* a = &f.pixels[(static_cast<std::size_t>(fy + y) * f.width + fx) * 4];
        const std::uint8_t* b = &t.pixels[(static_cast<std::size_t>(ty + y) * t.width + tx) * 4];
        for (int i = 0; i < w * 4; i += 4)
            for (int c = 0; c < 3; ++c)
                if (std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])) > kStripSlack)
                    return false;
    }
    return true;
}

ScreenRect clamp_rect(const Frame& f, ScreenRect r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.x + r.w), y1 = std::min(f.height, r.y + r.h);
    return ScreenRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

// ---- text -----------------------------------------------------------------

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

/// Windows' own Unicode normalisation; the text as it was when it will not.
std::wstring normalise(const std::wstring& w, NORM_FORM form) {
    if (w.empty()) return w;
    int size = NormalizeString(form, w.c_str(), static_cast<int>(w.size()), nullptr, 0);
    for (int tries = 0; tries < 4 && size > 0; ++tries) {
        std::wstring out(static_cast<std::size_t>(size), L'\0');
        const int got = NormalizeString(form, w.c_str(), static_cast<int>(w.size()), out.data(), size);
        if (got > 0) {
            out.resize(static_cast<std::size_t>(got));
            return out;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
        size = -got;    // a better guess at the length, negated
    }
    return w;
}

bool is_blank(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x00A0 || c == 0x3000 ||
           (c >= 0x2000 && c <= 0x200A);
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---- the file ---------------------------------------------------------------

/// What a chat.ini starts as when the tab has to make one. The shipped file
/// says more; this is enough to be read back.
const char* const kNewFile =
    "# Tự chat: what to say in the game's chat, and how often. Written by the\n"
    "# simple screen's Tự chat tab; the shipped configs/chat.ini explains every key.\n"
    "\n"
    "[Chat]\n"
    "enabled=false\n"
    "interval_s=120\n"
    "order=in_turn\n"
    "\n"
    "[Lines]\n";

bool make_if_missing(const std::string& path) {
    if (path_exists(path)) return true;
    std::ofstream out(utf8_to_wide(path).c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << kNewFile;
    return static_cast<bool>(out);
}

/// A text file as its lines, and how it was written: chat.ini goes back the
/// way it was found, with its BOM, its line ends and its encoding.
struct TextLines {
    std::vector<std::string> lines;     ///< UTF-8, whatever the file is in
    bool bom = false;
    bool crlf = false;
    /// UTF-16 with its BOM -- what Notepad calls "Unicode" -- read as UTF-8
    /// and written back as it was found: a save that added UTF-8 lines to it
    /// would leave a file that reads as neither.
    bool utf16 = false;
    bool big_endian = false;            ///< of UTF-16, the less usual order
};

/// UTF-16 bytes, after their BOM, as a wide string in this machine's order.
std::wstring utf16_units(const std::string& bytes, bool big_endian) {
    std::wstring w(bytes.size() / 2, L'\0');
    for (std::size_t i = 0; i < w.size(); ++i) {
        const auto a = static_cast<unsigned char>(bytes[2 * i]);
        const auto b = static_cast<unsigned char>(bytes[2 * i + 1]);
        w[i] = static_cast<wchar_t>(big_endian ? (a << 8) | b : (b << 8) | a);
    }
    return w;
}

bool read_lines(const std::string& path, TextLines& out) {
    std::ifstream in(utf8_to_wide(path).c_str(), std::ios::binary);
    if (!in) return false;
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(all[i]); };
    if (all.size() >= 2 && ((byte(0) == 0xFF && byte(1) == 0xFE) || (byte(0) == 0xFE && byte(1) == 0xFF))) {
        out.utf16 = true;
        out.bom = true;
        out.big_endian = byte(0) == 0xFE;
        all = narrow(utf16_units(all.substr(2), out.big_endian));
    } else if (all.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        out.bom = true;
        all.erase(0, 3);
    }
    const auto first_end = all.find('\n');
    out.crlf = first_end != std::string::npos && first_end > 0 && all[first_end - 1] == '\r';
    for (std::size_t at = 0; at < all.size();) {
        const std::size_t end = all.find('\n', at);
        std::string line = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return true;
}

/// Written beside the file and moved over it, so that a reader -- a running
/// Tự chat reads the file every second -- never finds half of one. A reader
/// that has it open at that moment makes the move fail, so it is tried again.
bool write_lines(const std::string& path, const TextLines& t) {
    const std::string fresh = path + ".tmp";
    {
        std::ofstream o(utf8_to_wide(fresh).c_str(), std::ios::binary | std::ios::trunc);
        if (!o) return false;
        const char* const end = t.crlf ? "\r\n" : "\n";
        std::string text;
        for (const std::string& line : t.lines) text += line + end;
        if (t.utf16) {
            std::string bytes = t.big_endian ? std::string("\xFE\xFF") : std::string("\xFF\xFE");
            for (const wchar_t c : widen(text)) {
                const char hi = static_cast<char>((c >> 8) & 0xFF), lo = static_cast<char>(c & 0xFF);
                bytes += t.big_endian ? hi : lo;
                bytes += t.big_endian ? lo : hi;
            }
            o << bytes;
        } else {
            if (t.bom) o << "\xEF\xBB\xBF";
            o << text;
        }
        o.flush();
        if (!o) {
            o.close();
            remove_file(fresh);
            return false;
        }
    }
    for (int tries = 0; tries < 10; ++tries) {
        if (MoveFileExW(utf8_to_wide(fresh).c_str(), utf8_to_wide(path).c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        Sleep(20);
    }
    remove_file(fresh);
    return false;
}

bool is_section(const std::string& line, std::string* name = nullptr) {
    const std::string t = trim(line);
    if (t.empty() || t.front() != '[') return false;
    const auto close = t.find(']');
    if (close == std::string::npos) return false;
    if (name) *name = lower(trim(t.substr(1, close - 1)));
    return true;
}

/// A whole number as written: in base ten whatever its leading zeros -- the
/// shared IniFile reads "090" as octal, which makes it nothing -- or in hex
/// after 0x. `fallback` when there is none, or it is too big to be one.
int read_int(const IniFile& ini, const std::string& section, const char* key, int fallback) {
    const std::string t = trim(ini.get(section, key));
    if (t.empty()) return fallback;
    const bool hex = t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X');
    try {
        return std::stoi(t, nullptr, hex ? 16 : 10);
    } catch (...) {
        return fallback;
    }
}

/// A number with a fraction, a decimal comma taken for a point: "0,95" is
/// how a Vietnamese keyboard writes it.
float read_float(const IniFile& ini, const std::string& section, const char* key, float fallback) {
    std::string t = trim(ini.get(section, key));
    if (t.empty()) return fallback;
    std::replace(t.begin(), t.end(), ',', '.');
    try {
        return std::stof(t);
    } catch (...) {
        return fallback;
    }
}

/// True the first time `what` is asked about in this run: a running Tự chat
/// reads its file every second, and a warning about it is said once.
bool first_time(const std::string& what) {
    static std::mutex guard;
    static std::set<std::string> said;
    std::lock_guard<std::mutex> lock(guard);
    return said.insert(what).second;
}

/// `line` is "lineN=..." for some N.
bool is_line_key(const std::string& line) {
    const std::string t = trim(line);
    const auto eq = t.find('=');
    if (eq == std::string::npos) return false;
    const std::string key = lower(trim(t.substr(0, eq)));
    if (key.size() < 5 || key.compare(0, 4, "line") != 0) return false;
    return key.find_first_not_of("0123456789", 4) == std::string::npos;
}

/// `line` is "lineN_only_alone=..." for some N: a line's "Chỉ khi chưa có tổ
/// đội" tick.
bool is_tick_key(const std::string& line) {
    const std::string t = trim(line);
    const auto eq = t.find('=');
    if (eq == std::string::npos) return false;
    const std::string key = lower(trim(t.substr(0, eq)));
    const std::string tail = "_only_alone";
    if (key.size() < 5 + tail.size() || key.compare(0, 4, "line") != 0 ||
        key.compare(key.size() - tail.size(), tail.size(), tail) != 0)
        return false;
    const std::string number = key.substr(4, key.size() - 4 - tail.size());
    return !number.empty() && number.find_first_not_of("0123456789") == std::string::npos;
}

// ---- the debug folder -------------------------------------------------------

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

/// The oldest PNGs in `dir` deleted until `keep` are left. Named by when they
/// were taken, they sort oldest first.
void prune_pngs(const std::string& dir, std::size_t keep) {
    std::vector<std::string> found = files_in(dir, "*.png");
    std::sort(found.begin(), found.end());
    for (std::size_t i = 0; i + keep < found.size(); ++i)
        remove_file(dir + "\\" + found[i]);
}

} // namespace

// ------------------------------------------------------------------ method

const char* chat_method_name(ChatMethod m) {
    switch (m) {
        case ChatMethod::Vk:      return "vk";
        case ChatMethod::Unicode: return "unicode";
        case ChatMethod::WmChar:  return "wmchar";
        case ChatMethod::ImeChar: return "imechar";
    }
    return "vk";
}

bool chat_method_from(const std::string& text, ChatMethod& out) {
    const std::string t = lower(trim(text));
    for (ChatMethod m : { ChatMethod::Vk, ChatMethod::Unicode, ChatMethod::WmChar,
                          ChatMethod::ImeChar }) {
        if (t == chat_method_name(m)) {
            out = m;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- settings

int ChatSettings::clamp_interval(int seconds) {
    return std::clamp(seconds, kMinInterval, kMaxInterval);
}

bool ChatSettings::load(const std::string& path) {
    // Read here rather than by IniFile::load, which wants [Chat] and its keys
    // in exactly this case and is blinded by a BOM in front of the first
    // line -- Notepad's "UTF-8 with BOM" -- and a hand-edited file has both.
    // Into an IniFile under the names asked for below, for its conversions.
    TextLines text;
    if (!read_lines(path, text)) return false;
    IniFile ini;
    std::string section;
    bool have_chat = false;
    for (const std::string& raw : text.lines) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            std::string name;
            if (!is_section(line, &name)) continue;
            section = name == "chat" ? "Chat" : name == "lines" ? "Lines" : name;
            have_chat = have_chat || section == "Chat";
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        if (key.empty()) continue;
        ini.set(section, key, trim(line.substr(eq + 1)));
    }
    if (!have_chat) return false;

    ChatSettings s;
    const std::string C = "Chat";
    s.enabled          = ini.get_bool(C, "enabled", s.enabled);
    s.interval_s       = clamp_interval(read_int(ini, C, "interval_s", s.interval_s));
    s.random_order     = lower(trim(ini.get(C, "order", "in_turn"))) == "random";
    s.jitter_pct       = std::clamp(read_int(ini, C, "jitter_pct", s.jitter_pct), 0, kMaxJitter);
    s.first_delay_s    = std::clamp(read_int(ini, C, "first_delay_s", s.first_delay_s), 0, 600);
    s.max_length       = std::clamp(read_int(ini, C, "max_length", s.max_length), 1, kLongest);
    s.strip_diacritics = ini.get_bool(C, "strip_diacritics", s.strip_diacritics);
    // Anything but a method still offered is typed GoC's way. Paste was one,
    // and is not: a paste the game got round to late would post whatever the
    // player had copied.
    const std::string asked = lower(trim(ini.get(C, "method", "vk")));
    ChatMethod how = ChatMethod::Vk;
    if (asked.empty() || chat_method_from(asked, how)) {
        s.method = how;
    } else {
        s.method = ChatMethod::Vk;
        s.method_note = asked == "paste"
                            ? "Cách gõ paste (dán qua clipboard) không còn nữa, nên câu được gõ bằng vk."
                            : "Cách gõ \"" + asked + "\" trong chat.ini không có, nên câu được gõ bằng vk.";
        if (first_time(path + "\n" + asked)) {
            if (asked == "paste")
                GB_WARN("Chat: method=paste in %s is no longer offered -- a paste the game took late "
                        "could post what was on the clipboard -- so lines are typed with vk",
                        path.c_str());
            else
                GB_WARN("Chat: method '%s' in %s is not one of vk, unicode, wmchar, imechar; typing "
                        "with vk", asked.c_str(), path.c_str());
        }
    }
    s.clear_backspaces = std::clamp(read_int(ini, C, "clear_backspaces", s.clear_backspaces), 0, 20);
    const std::string open = trim(ini.get(C, "open_picture"));
    if (!open.empty()) s.open_picture = open;
    const std::string shut = trim(ini.get(C, "closed_picture"));
    if (!shut.empty()) s.closed_picture = shut;
    s.closed_score     = std::clamp(read_float(ini, C, "closed_score", s.closed_score), 0.5f, 1.0f);
    s.empty_ink        = std::clamp(read_int(ini, C, "empty_ink", s.empty_ink), 1, 40);
    s.max_failures     = std::clamp(read_int(ini, C, "max_failures", s.max_failures), 1, 20);
    const std::string test = trim(ini.get(C, "test_line"));
    if (!test.empty()) s.test_line = test;
    // How the keys go. Both holds 0, each key keeps its own; otherwise every
    // key from the one to the other, each 10 to kMaxHold ms, the second no
    // less than the first -- one alone set is a fixed hold.
    s.hold_min_ms = std::clamp(read_int(ini, C, "hold_min_ms", s.hold_min_ms), 0, kMaxHold);
    s.hold_max_ms = std::clamp(read_int(ini, C, "hold_max_ms", s.hold_max_ms), 0, kMaxHold);
    if (s.hold_min_ms > 0 || s.hold_max_ms > 0) {
        if (s.hold_min_ms == 0) s.hold_min_ms = s.hold_max_ms;
        s.hold_min_ms = std::clamp(s.hold_min_ms, 10, kMaxHold);
        s.hold_max_ms = std::clamp(std::max(s.hold_max_ms, s.hold_min_ms), 10, kMaxHold);
    }
    s.enter_gap_ms    = std::clamp(read_int(ini, C, "enter_gap_ms", s.enter_gap_ms), 250, 3000);
    s.quiet_before_ms = std::clamp(read_int(ini, C, "quiet_before_ms", s.quiet_before_ms), 0, 5000);
    s.type_wait_ms    = std::clamp(read_int(ini, C, "type_wait_ms", s.type_wait_ms), 0, 3000);
    s.max_enters      = std::clamp(read_int(ini, C, "max_enters", s.max_enters), 1, kMaxEnters);
    s.type_tries      = std::clamp(read_int(ini, C, "type_tries", s.type_tries), 1, kMaxTypeTries);
    s.inject = lower(trim(ini.get(C, "inject", "vk"))) == "scancode" ? InputManager::KeyInject::Scancode
                                                                      : InputManager::KeyInject::Vk;

    for (int i = 1; i <= kMaxLines; ++i) {
        const std::string key = "line" + std::to_string(i);
        const std::string line = trim(ini.get("Lines", key));
        if (line.empty()) continue;
        s.lines.push_back(line);
        // Its tick beside it, under the same number: a tick never belongs to
        // the line that happens to land in its place.
        s.only_alone.push_back(ini.get_bool("Lines", key + "_only_alone", false));
    }
    *this = std::move(s);
    return true;
}

bool ChatSettings::save_key(const std::string& path, const std::string& key,
                            const std::string& value) {
    if (!make_if_missing(path)) return false;
    TextLines text;
    if (!read_lines(path, text)) return false;

    // As IniFile::set_in_place does it -- the key replaced where it is, or
    // added at the end of [Chat], or in a [Chat] of its own -- but written in
    // one move, as the lines are.
    const std::string want = lower(trim(key));
    const std::string entry = trim(key) + "=" + value;
    bool in_chat = false, found = false;
    std::size_t last = 0;       // the last line belonging to [Chat]
    for (std::size_t i = 0; i < text.lines.size(); ++i) {
        const std::string line = trim(text.lines[i]);
        std::string name;
        if (!line.empty() && line.front() == '[') {
            if (is_section(line, &name)) {
                in_chat = name == "chat";
                if (in_chat) { found = true; last = i; }
            }
            continue;
        }
        if (!in_chat) continue;
        last = i;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || lower(trim(line.substr(0, eq))) != want) continue;
        text.lines[i] = entry;
        return write_lines(path, text);
    }
    if (found) {
        // Back over blank lines, so the key joins the section rather than
        // floating in the gap before the next one.
        std::size_t at = last;
        while (at > 0 && trim(text.lines[at]).empty()) --at;
        text.lines.insert(text.lines.begin() + static_cast<std::ptrdiff_t>(at) + 1, entry);
    } else {
        if (!text.lines.empty() && !trim(text.lines.back()).empty()) text.lines.push_back({});
        text.lines.push_back("[Chat]");
        text.lines.push_back(entry);
    }
    return write_lines(path, text);
}

bool ChatSettings::save_lines(const std::string& path, const std::vector<std::string>& lines) {
    return save_lines(path, lines, {});
}

bool ChatSettings::save_lines(const std::string& path, const std::vector<std::string>& lines,
                              const std::vector<bool>& only_alone) {
    if (!make_if_missing(path)) return false;

    TextLines file;
    if (!read_lines(path, file)) return false;
    const std::vector<std::string>& text = file.lines;

    // The old lines out, wherever they are in [Lines]; comments stay.
    std::vector<std::string> out;
    std::size_t header = static_cast<std::size_t>(-1);
    bool in_lines = false;
    for (const std::string& line : text) {
        std::string name;
        if (is_section(line, &name)) {
            in_lines = name == "lines";
            if (in_lines && header == static_cast<std::size_t>(-1)) header = out.size();
            out.push_back(line);
            continue;
        }
        // The old ticks go with the old lines: they are written again below,
        // each beside the line it is now for.
        if (in_lines && (is_line_key(line) || is_tick_key(line))) continue;
        out.push_back(line);
    }
    std::vector<std::string> keys;
    int written = 0;
    for (std::size_t i = 0; i < lines.size() && written < kMaxLines; ++i) {
        // One line each: a line break inside one would start a key of its own.
        std::string v = lines[i];
        std::replace(v.begin(), v.end(), '\n', ' ');
        std::replace(v.begin(), v.end(), '\r', ' ');
        v = trim(v);
        if (v.empty()) continue;
        const std::string key = "line" + std::to_string(++written);
        keys.push_back(key + "=" + v);
        // Only a tick that is there is written: a file without one reads as
        // no tick, which is how the shipped file has every line.
        if (i < only_alone.size() && only_alone[i]) keys.push_back(key + "_only_alone=true");
    }
    if (header == static_cast<std::size_t>(-1)) {
        if (!out.empty() && !trim(out.back()).empty()) out.push_back({});
        out.push_back("[Lines]");
        out.insert(out.end(), keys.begin(), keys.end());
    } else {
        // After the comments that open the section, so they stay its heading.
        std::size_t at = header + 1;
        while (at < out.size() && !is_section(out[at])) {
            const std::string t = trim(out[at]);
            if (t.empty() || t[0] == '#' || t[0] == ';') { ++at; continue; }
            break;
        }
        while (at > header + 1 && trim(out[at - 1]).empty()) --at;
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), keys.begin(), keys.end());
    }

    file.lines = std::move(out);
    return write_lines(path, file);
}

std::vector<std::string> ChatSettings::split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= text.size() && out.size() < static_cast<std::size_t>(kMaxLines)) {
        const std::size_t end = text.find('\n', at);
        const std::string line =
            trim(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (!line.empty()) out.push_back(line);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return out;
}

std::string ChatSettings::join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty()) out += '\n';
        out += line;
    }
    return out;
}

std::vector<bool> ChatSettings::carry_ticks(const std::vector<std::string>& before,
                                            const std::vector<bool>& before_ticks,
                                            const std::vector<std::string>& lines) {
    std::vector<bool> out(lines.size(), false);
    std::vector<bool> taken(before.size(), false);
    const auto tick_of = [&](std::size_t j) { return j < before_ticks.size() && before_ticks[j]; };
    // Each line's own place first, so that a list with the same words twice
    // keeps each tick where it was; then the first of those words not taken.
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (i < before.size() && before[i] == lines[i]) {
            out[i] = tick_of(i);
            taken[i] = true;
        }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i < before.size() && before[i] == lines[i]) continue;
        for (std::size_t j = 0; j < before.size(); ++j)
            if (!taken[j] && before[j] == lines[i]) {
                out[i] = tick_of(j);
                taken[j] = true;
                break;
            }
    }
    return out;
}

AloneTurn alone_turn(const std::vector<bool>& only_alone, const std::vector<bool>& typeable, int index,
                     bool alone) {
    const auto ticked = [&](std::size_t i) { return i < only_alone.size() && only_alone[i]; };
    if (index < 0 || !ticked(static_cast<std::size_t>(index)) || alone) return AloneTurn::Send;
    // In a party, or not known: passed over for the next line -- unless no
    // line could go instead, when passing over would only come back round to
    // this one a second later, the HUD read again each time.
    for (std::size_t i = 0; i < typeable.size(); ++i)
        if (typeable[i] && !ticked(i)) return AloneTurn::Skip;
    return AloneTurn::Wait;
}

AloneLook alone_look(bool trusted, bool focused) {
    // The game behind another window: chat_gate holds every line until it is
    // in front again, and the ticked one keeps its turn like the rest. In
    // front, the turn is never kept for want of a look at the party -- a
    // window kept on top over the game's left side would otherwise hold back
    // every line after it for as long as it stays there.
    if (!focused) return AloneLook::KeepTurn;
    return trusted ? AloneLook::Read : AloneLook::Unread;
}

// -------------------------------------------------------------------- text

ChatText prepare_chat(const std::string& line, ChatMethod method, bool strip, int max_length) {
    ChatText out;
    const bool keys = method == ChatMethod::Vk;

    // Composed first: a mark typed or pasted as a character of its own is
    // the same letter as the one with the mark built in.
    std::wstring w = normalise(widen(line), NormalizationC);

    if (keys || strip) {
        // Taken apart, the marks dropped, put together again. đ is a letter
        // of its own rather than d with a mark, so it is done by hand.
        const std::wstring parts = normalise(w, NormalizationD);
        std::wstring bare;
        bare.reserve(parts.size());
        for (const wchar_t c : parts) {
            if (c >= 0x0300 && c <= 0x036F) { out.stripped = true; continue; }
            if (c == 0x0111) { bare += L'd'; out.stripped = true; continue; }
            if (c == 0x0110) { bare += L'D'; out.stripped = true; continue; }
            bare += c;
        }
        w = normalise(bare, NormalizationC);
    }

    std::wstring kept;
    kept.reserve(w.size());
    for (const wchar_t c : w) {
        if (is_blank(c)) { kept += L' '; continue; }
        bool ok;
        if (keys) {
            // What has a key of its own, as the commercial client types it.
            ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        } else {
            // Anything printable that is one UTF-16 unit: no control codes,
            // nothing outside the basic plane, no invisible marks.
            ok = !(c < 0x20 || (c >= 0x7F && c <= 0x9F) || (c >= 0xD800 && c <= 0xDFFF) ||
                   c == 0xFFFD || c == 0xFEFF || (c >= 0x200B && c <= 0x200F));
        }
        if (!ok) { out.dropped = true; continue; }
        kept += c;
    }

    // One space between words, none at the ends.
    std::wstring tidy;
    tidy.reserve(kept.size());
    for (const wchar_t c : kept) {
        if (c == L' ' && (tidy.empty() || tidy.back() == L' ')) continue;
        tidy += c;
    }
    while (!tidy.empty() && tidy.back() == L' ') tidy.pop_back();

    const int longest = std::clamp(max_length, 1, ChatSettings::kLongest);
    if (static_cast<int>(tidy.size()) > longest) {
        tidy.resize(static_cast<std::size_t>(longest));
        out.cut = true;
        while (!tidy.empty() && tidy.back() == L' ') tidy.pop_back();
    }

    out.typed = narrow(tidy);
    out.chars = static_cast<int>(tidy.size());
    return out;
}

std::vector<ChatKey> chat_keys(const std::string& ascii, bool caps_lock) {
    std::vector<ChatKey> keys;
    keys.reserve(ascii.size());
    for (const char c : ascii) {
        if (c == ' ') {
            keys.push_back({ VK_SPACE, false });
        } else if (c >= '0' && c <= '9') {
            keys.push_back({ static_cast<std::uint32_t>(c), false });
        } else if (c >= 'a' && c <= 'z') {
            // A small letter with Caps Lock on needs Shift to come out small.
            keys.push_back({ static_cast<std::uint32_t>(c - 'a' + 'A'), caps_lock });
        } else if (c >= 'A' && c <= 'Z') {
            keys.push_back({ static_cast<std::uint32_t>(c), !caps_lock });
        }
    }
    return keys;
}

// ---------------------------------------------------------------- schedule

ChatSchedule::ChatSchedule(std::uint32_t seed) : rng_(seed) {}

void ChatSchedule::start(std::uint64_t now, const ChatSettings& s) {
    count_      = static_cast<int>(s.lines.size());
    random_     = s.random_order;
    interval_s_ = ChatSettings::clamp_interval(s.interval_s);
    jitter_pct_ = std::clamp(s.jitter_pct, 0, ChatSettings::kMaxJitter);
    sent_       = 0;
    failures_   = 0;
    turn_       = 0;
    if (random_ && count_ > 1) turn_ = std::uniform_int_distribution<int>(0, count_ - 1)(rng_);
    due_        = now + static_cast<std::uint64_t>(std::max(0, s.first_delay_s)) * 1000ull;
    last_end_   = now;
}

void ChatSchedule::update(const ChatSettings& s) {
    const int interval = ChatSettings::clamp_interval(s.interval_s);
    const bool shorter = interval < interval_s_;
    count_      = static_cast<int>(s.lines.size());
    random_     = s.random_order;
    interval_s_ = interval;
    jitter_pct_ = std::clamp(s.jitter_pct, 0, ChatSettings::kMaxJitter);
    if (count_ > 0 && turn_ >= count_) turn_ = 0;
    // A line waiting out the old, longer interval does not go on waiting it.
    if (shorter && sent_ > 0)
        due_ = std::min(due_, last_end_ + static_cast<std::uint64_t>(interval) * 1000ull);
}

int ChatSchedule::after(int index) {
    if (count_ <= 1) return 0;
    if (!random_) return (index + 1) % count_;
    // Any line but this one.
    int pick = std::uniform_int_distribution<int>(0, count_ - 2)(rng_);
    if (pick >= index) ++pick;
    return pick;
}

std::uint64_t ChatSchedule::draw_gap_ms() {
    const double j = jitter_pct_ / 100.0;
    const double stray = j > 0.0 ? std::uniform_real_distribution<double>(-j, j)(rng_) : 0.0;
    const double gap = std::max(ChatSettings::kMinInterval * 1000.0,
                                interval_s_ * 1000.0 * (1.0 + stray));
    return static_cast<std::uint64_t>(gap);
}

void ChatSchedule::sent(std::uint64_t now) {
    ++sent_;
    failures_ = 0;
    if (count_ > 0) turn_ = after(turn_);
    due_ = now + draw_gap_ms();
    last_end_ = now;
}

void ChatSchedule::failed(std::uint64_t now, std::uint64_t backoff_ms) {
    ++failures_;
    due_ = now + backoff_ms;
    last_end_ = now;
}

void ChatSchedule::postpone(std::uint64_t now, std::uint64_t ms) {
    due_ = now + ms;
}

void ChatSchedule::skip(std::uint64_t now) {
    if (count_ > 0) turn_ = after(turn_);
    due_ = now + 1000;
}

// --------------------------------------------------------------------- bar

const char* chat_bar_name(ChatBarState s) {
    switch (s) {
        case ChatBarState::Closed:  return "đang đóng";
        case ChatBarState::Open:    return "đang mở";
        case ChatBarState::Unknown: break;
    }
    return "không thấy";
}

bool can_look_for_chat(const TemplateStore& store, const ChatSettings& s) {
    const Frame* open = store.get(s.open_picture);
    return open != nullptr && open->valid() && open->width >= kDividerX + kFieldDx + kFieldW &&
           open->height >= kStripY + kStripH;
}

ChatBar look_chat_bar(const Frame& f, const TemplateStore& store, const ChatSettings& s) {
    ChatBar best;
    if (!can_look_for_chat(store, s)) {
        best.trouble = "Không có hình ô chat (" + s.open_picture + "), nên không biết ô chat đang mở "
                       "hay đóng.";
        return best;
    }
    if (!f.valid()) {
        best.trouble = kNoShot;
        return best;
    }
    const Frame& open = *store.get(s.open_picture);
    const Frame* shut = store.get(s.closed_picture);

    const int top = std::max(0, f.height - kCornerH);
    const int right = std::min(f.width, kCornerW);
    for (int y = top; y + kStripH <= f.height; ++y) {
        for (int x = kEdgeDx; x + kDividerW <= right; ++x) {
            if (!strip_at(f, x, y, open, kDividerX, kStripY, kDividerW, kStripH)) continue;
            if (!strip_at(f, x - kEdgeDx, y, open, kEdgeX, kStripY, kEdgeW, kStripH)) continue;

            ChatBar bar;
            bar.found = true;
            bar.x = x;
            bar.y = y;
            bar.field = clamp_rect(f, ScreenRect{ x + kFieldDx, y + kFieldDy, kFieldW, kFieldH });
            for (int py = bar.field.y; py < bar.field.y + bar.field.h; ++py)
                for (int px = bar.field.x; px < bar.field.x + bar.field.w; ++px) {
                    const int d = deviation(f, px, py, kOpenGrey);
                    if (d <= kGreySlack) ++bar.dark;
                    else if (d > kInkFrom) bar.ink = std::max(bar.ink, px - bar.field.x + 1);
                }
            if (shut != nullptr && shut->valid()) {
                const ScreenRect around = clamp_rect(
                    f, ScreenRect{ x + kShutDx - 4, y + kShutDy - 4, shut->width + 8, shut->height + 8 });
                const auto m = TemplateMatcher::find(f, around, *shut, 0.0f);
                bar.closed_score = m.found ? m.score : 0.0f;
            }
            if (bar.dark >= kOpenAtLeast)
                bar.state = ChatBarState::Open;
            else if (bar.closed_score >= s.closed_score)
                bar.state = ChatBarState::Closed;

            // The likeliest of more than one: a state over none, the first
            // found otherwise. There is one, in every frame seen so far.
            const auto rank = [](const ChatBar& b) {
                return b.state == ChatBarState::Open ? 3 : b.state == ChatBarState::Closed ? 2 :
                       b.found ? 1 : 0;
            };
            if (rank(bar) > rank(best)) best = bar;
        }
    }
    return best;
}

// ----------------------------------------------------------------- dialogs

void ChatDialogs::load(const std::string& trash_ini, const std::string& party_ini,
                       const std::string& routines_ini) {
    CleanSettings clean;
    if (clean.load(trash_ini)) bag = clean.layout;
    IniFile party;
    if (party.load(party_ini)) invite.load(party);
    const auto routines = Routine::load(routines_ini);
    const Routine* revive = Routine::find(routines, "revive");
    if (revive != nullptr && revive->valid()) {
        death = revive->steps.front().find;
        death_score = revive->steps.front().score;
    }
}

std::string ChatDialogs::up(const Frame& f, const TemplateStore& store) const {
    if (!f.valid()) return "không chụp được màn hình";
    // The four looks only read the frame and the pictures, and none needs
    // another's answer: side by side, the answer comes in the time of the
    // slowest of them rather than of all four, and the frame the next Enter
    // goes on is that much younger. Each about a tenth of a second.
    auto buttons = std::async(std::launch::async, [&] { return bag::find_dialogs(f, store); });
    auto invited = std::async(std::launch::async, [&] {
        PartyInvite asking;
        asking.set_settings(invite);
        return asking.look(f, store, nullptr).up;
    });
    auto dead = std::async(std::launch::async, [&] {
        const Frame* picture = store.get(death);
        return picture != nullptr &&
               TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *picture, death_score).found;
    });
    const BagView v = bag::find(f, store, bag);
    const std::vector<bag::Dialog> dialogs = buttons.get();
    const bool asking = invited.get();
    const bool died = dead.get();

    // The most particular first. The invitation's picture is only the
    // [Xác Nhận][Hủy Bỏ] pair every question wears -- the revive question's,
    // a shop's -- so it names an invitation only where no other look found a
    // dialog with those buttons; it keeps Enter back all the same.
    for (const bag::Dialog& d : dialogs)
        if (d.ours()) return "thùng rác";
    if (died) return "hồi sinh";
    if (!dialogs.empty()) return "hộp thoại có nút Xác Nhận";
    if (asking) return "lời mời tổ đội";
    if (v.found && !v.bin_empty) return "thùng rác đang giữ đồ";
    return {};
}

bool ChatDialogs::can_check(const TemplateStore& store) {
    return bag::can_check(store, TrashList::Action::Delete);
}

// -------------------------------------------------------------------- gate

ChatGate chat_gate(const ChatGateInput& in) {
    const auto wait = [](std::uint64_t ms, std::string why) {
        ChatGate g;
        g.wait_ms = ms;
        g.why = std::move(why);
        return g;
    };
    if (!in.window) return wait(5000, kNoWindow);
    if (in.keeper) return wait(10000, kKeeper);
    if (in.trash) return wait(10000, kTrash);
    if (!in.held_by.empty())
        return wait(10000, "Bot đang tạm dừng (" + in.held_by + ") — chat sau.");
    if (!in.focused) return wait(5000, kNotFront);
    if (in.user_active) return wait(5000, kUserActive);
    if (in.hp_pct >= 0.0f && in.hp_pct < static_cast<float>(in.hp_floor)) return wait(2000, kLowHp);
    if (in.looked) {
        if (!in.dialog.empty()) return wait(5000, dialog_wait(in.dialog));
        if (!in.hud) return wait(10000, kNoHud);
        if (in.bar == ChatBarState::Open) return wait(15000, kBarOpen);
        if (in.bar != ChatBarState::Closed) return wait(10000, kNoBar);
    }
    ChatGate g;
    g.send = true;
    return g;
}

std::string chat_quiet_pause(int ms, const std::function<std::string()>& stop,
                             const std::function<void(int)>& sleep) {
    for (int slept = 0;; slept += kQuietStepMs) {
        if (stop) {
            std::string why = stop();
            if (!why.empty()) return why;
        }
        if (slept >= ms) return {};
        const int step = std::min(kQuietStepMs, ms - slept);
        if (sleep) sleep(step);
        else std::this_thread::sleep_for(std::chrono::milliseconds(step));
    }
}

// ------------------------------------------------------------------ sender

namespace {

// What a look at a line left in doubt says, in Vietnamese, for the tab.
const char* const kLeftUnseen  = "Không thấy ô chat ở góc trái dưới màn hình game, nên chưa biết ô chat đã "
                                 "đóng chưa. Đừng để cửa sổ nào che khung chat.";
const char* const kLeftNeither = "Ô chat không giống hình ô chat đang mở hay đang đóng, nên chưa biết ô chat "
                                 "đã đóng chưa.";
const char* const kLeftAway    = "Ô chat đang mở mà game không ở trên cùng, nên chưa đóng được.";

const char* trace_state(ChatBarState s) {
    switch (s) {
        case ChatBarState::Open:    return "open";
        case ChatBarState::Closed:  return "shut";
        case ChatBarState::Unknown: break;
    }
    return "unseen";
}

std::string trace_secs(int ms) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "+%.2f s", static_cast<double>(std::max(0, ms)) / 1000.0);
    return buf;
}

std::string trace_range(int lo, int hi) {
    return lo == hi ? std::to_string(lo) : std::to_string(lo) + "-" + std::to_string(hi);
}

} // namespace

std::string chat_trace_line(const ChatTrace& t) {
    std::string s = "Chat trace:";
    std::string sep = " ";
    for (std::size_t i = 0; i < t.enters.size(); ++i) {
        const ChatTrace::Enter& e = t.enters[i];
        s += sep + "Enter " + std::to_string(i + 1) + " held " + std::to_string(e.hold_ms) + " ms:";
        if (e.looks.empty()) s += " no look";
        for (std::size_t k = 0; k < e.looks.size(); ++k)
            s += std::string(k ? ", " : " ") + trace_secs(e.looks[k].first) + " " + trace_state(e.looks[k].second);
        sep = "; ";
    }
    if (t.cleared_ink >= 0) {
        s += sep + "cleared";
        if (t.backspace_max > 0) s += " (Backspace held " + trace_range(t.backspace_min, t.backspace_max) + " ms)";
        s += ", ink " + std::to_string(t.cleared_ink);
        sep = "; ";
    }
    if (t.letters > 0) {
        s += sep + std::to_string(t.letters) + " keys typed in " + std::to_string(t.tries) +
             (t.tries == 1 ? " try" : " tries");
        if (t.letter_max > 0)
            s += ", held " + trace_range(t.letter_min, t.letter_max) + " ms (" +
                 (t.inject == InputManager::KeyInject::Scancode ? "scancode" : "vk") + ")";
        if (!t.inks.empty()) {
            s += ", ink before each next key:";
            for (const int ink : t.inks) s += " " + std::to_string(ink);
        }
        sep = "; ";
    }
    if (!t.after.empty()) {
        s += sep + "after the last key:";
        for (std::size_t k = 0; k < t.after.size(); ++k)
            s += std::string(k ? ", " : " ") + trace_secs(t.after[k].first) + " ink " + std::to_string(t.after[k].second);
        sep = "; ";
    }
    if (t.send_hold > 0) {
        s += sep + "Enter to send held " + std::to_string(t.send_hold) + " ms";
        sep = "; ";
    }
    s += sep + (t.outcome.empty() ? std::string("not sent") : t.outcome);
    if (!t.machine.empty()) s += "; " + t.machine;
    return s;
}

std::string chat_machine_note(void* game_window) {
    std::string s = "typing programs running: ";
    const std::vector<std::string> typers = vietnamese_typers_running();
    if (typers.empty()) s += "none";
    for (std::size_t i = 0; i < typers.size(); ++i) s += (i ? ", " : "") + typers[i];
    const std::uint16_t layout = window_keyboard_layout(game_window);
    char hex[48];
    if (layout != 0) std::snprintf(hex, sizeof hex, "; the game's keyboard layout %04X", layout);
    else std::snprintf(hex, sizeof hex, "; the game's keyboard layout not known");
    return s + hex;
}

ChatSender::ChatSender(InputManager& input, const TemplateStore& store, Grab grab)
    : input_(input), store_(store), grab_(std::move(grab)) {}

bool ChatSender::key(std::uint32_t vk, bool shift, int hold_ms) {
    if (hands_.key) return hands_.key(vk, shift, hold_ms);
    return input_.type_key(vk, shift, hold_ms, settings_.inject);
}

int ChatSender::hold(int lo, int hi) {
    // chat.ini's holds, when set, for every key alike.
    if (settings_.hold_min_ms > 0 && settings_.hold_max_ms > 0) return jitter(settings_.hold_min_ms, settings_.hold_max_ms);
    return jitter(lo, hi);
}

bool ChatSender::enter(bool opening, int hold_ms) {
    if (!key(VK_RETURN, false, hold_ms)) return false;
    entered_ = true;
    opening_enter_ = opening;
    enter_ms_ = clock_ms();
    return true;
}

bool ChatSender::text(const std::wstring& t, ChatMethod how) {
    if (hands_.text) return hands_.text(t, how);
    if (t.empty()) return false;
    switch (how) {
        case ChatMethod::Unicode: return input_.type_unit(t[0]);
        case ChatMethod::WmChar:  return input_.post_char(t[0], false);
        case ChatMethod::ImeChar: return input_.post_char(t[0], true);
        case ChatMethod::Vk:      break;
    }
    return false;
}

bool ChatSender::focused() {
    return hands_.focused ? hands_.focused() : input_.window_focused();
}

bool ChatSender::caps_lock() {
    return hands_.caps_lock ? hands_.caps_lock() : InputManager::caps_lock_on();
}

std::uint64_t ChatSender::clock_ms() {
    return hands_.now ? hands_.now() : now_ms();
}

void ChatSender::pause(int ms) {
    if (ms <= 0) return;
    if (hands_.sleep) hands_.sleep(ms);
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool ChatSender::wait(int ms) {
    for (int slept = 0; slept < ms; slept += 20) {
        if (cancelled_) return false;
        // HP under the potion's mark, or another hold: the line ends now,
        // not when the wait does -- the Enter watch, the type wait and the
        // looks after typing run to seconds as chat.ini can set them (R8R-2).
        if (guard_) {
            std::string why = guard_();
            if (!why.empty()) {
                halted_ = std::move(why);
                return false;
            }
        }
        pause(std::min(20, ms - slept));
    }
    return !cancelled_;
}

int ChatSender::jitter(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, std::max(lo, hi))(rng_);
}

void ChatSender::keep(const Frame& f, const char* what, const ChatBar* bar) {
    if (debug_dir_.empty() || !f.valid()) return;
    make_folder(debug_dir_);
    prune_pngs(debug_dir_, 29);
    static int n = 0;
    const std::string path = debug_dir_ + "\\" + stamp() + "_" + std::to_string(n++ % 100) + "_" +
                             what + ".png";
    if (!save_png(f, path)) return;
    if (bar == nullptr) {
        GB_INFO("Chat: kept the screen as %s", path.c_str());
        return;
    }
    // With the numbers the bar was judged by, which the picture alone does
    // not say.
    GB_INFO("Chat: kept the screen as %s -- the bar %s, found=%d at %d,%d, dark %d (open from %d), "
            "like the shut field %.3f (shut from %.2f), ink %d",
            path.c_str(), chat_bar_name(bar->state), bar->found ? 1 : 0, bar->x, bar->y, bar->dark,
            kOpenAtLeast, static_cast<double>(bar->closed_score),
            static_cast<double>(settings_.closed_score), bar->ink);
}

ChatBar ChatSender::look() {
    Frame f;
    if (!grab_ || !grab_(f) || !f.valid()) {
        ChatBar none;
        none.trouble = kNoShot;
        return none;
    }
    return look_chat_bar(f, store_, settings_);
}

bool ChatSender::look_both(ChatBar& bar, std::string& dialog, Frame* kept) {
    Frame f;
    if (!grab_ || !grab_(f) || !f.valid()) {
        bar = ChatBar{};
        bar.trouble = kNoShot;
        dialog.clear();
        return false;
    }
    bar = look_chat_bar(f, store_, settings_);
    dialog = dialog_ ? dialog_(f) : std::string();
    if (kept) *kept = std::move(f);
    return true;
}

bool ChatSender::look_bar(ChatBar& bar, Frame* into) {
    Frame local;
    Frame& f = into != nullptr ? *into : local;
    if (!grab_ || !grab_(f) || !f.valid()) {
        bar = ChatBar{};
        bar.trouble = kNoShot;
        return false;
    }
    bar = look_chat_bar(f, store_, settings_);
    return true;
}

bool ChatSender::believe_shut(ChatBar& bar) {
    if (bar.state != ChatBarState::Closed) return false;
    // A frame on at least, so that one odd frame does not decide it. After an
    // Enter that may have opened the line, not before the game has had time
    // to show what it did with it: a look sooner shows the line as it was.
    int gap = kKeyGap;
    if (entered_ && opening_enter_) {
        const std::uint64_t now = clock_ms(), due = enter_ms_ + kSettleMs;
        if (due > now) gap = std::max(gap, static_cast<int>(due - now));
    }
    pause(gap);
    ChatBar again;
    look_bar(again);
    bar = again;
    return again.state == ChatBarState::Closed;
}

bool ChatSender::rub_out(int n) {
    for (int i = 0; i < n; ++i) {
        if (!key(VK_BACK, false, hold(25, 25))) return false;
        pause(15);
    }
    return true;
}

bool ChatSender::shows_empty(const ChatBar& bar) const {
    // Nothing drawn at all is an empty line with its caret blinked off: the
    // game's letters are outlined in black, which shows on the field's grey.
    if (bar.ink == 0) return true;
    // An empty line's caret, measured when it was cleared. Only with it shown
    // (more than nothing): measured blinked off, it says nothing of where the
    // caret goes.
    return empty_ink_ > 0 && bar.ink <= empty_ink_ + 2;
}

void ChatSender::tidy(Result& r, int rub) {
    ChatBar bar;
    std::string dialog;
    const bool looked = look_both(bar, dialog);
    if (looked) r.last = bar;
    if (looked && bar.state == ChatBarState::Closed) {
        const bool shut = believe_shut(bar);
        r.last = bar;
        if (shut) {
            r.left_open = false;
            return;
        }
    }
    // Rubbed out only into a line a look has just shown open: with it shut,
    // or out of sight, Backspace goes to the game itself. A line not seen
    // open is left to the looks below, and to whoever watches it after.
    if (rub > 0 && bar.state == ChatBarState::Open) {
        if (!rub_out(std::min(rub, kMostRubbed))) {
            r.left_open = true;
            if (r.trouble.empty()) r.trouble = kMaybeLeft;
            return;
        }
        pause(kDrawnMs);
    }

    // Closed with Enter, which sends nothing from an empty line -- and sends
    // whatever is in one that is not. So only on a fresh look that shows the
    // line open, no dialog, and nothing in it: nothing drawn past the caret
    // measured when it was cleared, or else -- that not measured -- no more
    // than ChatSettings::empty_ink, and then only after Backspaces enough
    // for whatever could hide in that much. Pressed after the look, those
    // reach the game before the Enter does, however late the frame was.
    // Anything wider is rubbed out, a few rounds at most, and never sent.
    const int caret = settings_.empty_ink;
    bool armed = false;     // a caret's width of Backspaces pressed since a look showed no more
    int rounds = 0, enters = 0;
    int rubbed_at = -1;     // what the field showed before the last round rubbed out
    for (int i = 0; i < 8; ++i) {
        if (!look_both(bar, dialog)) { pause(250); continue; }
        r.last = bar;
        if (bar.state == ChatBarState::Closed) {
            const bool shut = believe_shut(bar);
            r.last = bar;
            if (shut) {
                r.left_open = false;
                return;
            }
            continue;       // whatever the second look showed, looked at again
        }
        if (!dialog.empty()) {
            r.dialog = dialog;
            break;
        }
        if (bar.state != ChatBarState::Open) { pause(250); continue; }

        if (shows_empty(bar) || (armed && bar.ink <= caret)) {
            // Twice at most, and each time waited on: an Enter the game is
            // slow to take, pressed again, opens the line it shut.
            if (enters >= 2 || !focused() || !enter(false, hold(50, 50))) break;
            ++enters;
            armed = false;
            for (int k = 0; k < 3; ++k) {
                pause(250);
                ChatBar after;
                if (look_bar(after) && after.state == ChatBarState::Closed && believe_shut(after)) {
                    r.last = after;
                    r.left_open = false;
                    return;
                }
            }
            continue;
        }
        if (rounds++ >= 3) break;
        if (bar.ink <= caret) {
            if (!rub_out(rub_for(caret))) break;
            armed = true;
        } else {
            // A round that took nothing away says what shows is not text
            // Backspace removes: left open, rather than rubbed at for ever.
            if (rubbed_at >= 0 && bar.ink >= rubbed_at - 2) break;
            rubbed_at = bar.ink;
            if (!rub_out(std::min(rub_for(bar.ink), kMostRubbed))) break;
            armed = false;
        }
        pause(kDrawnMs);
    }
    r.left_open = true;
}

bool ChatSender::close_left_open(int rub_out_count) {
    // Only a line seen open is rubbed out: with the line shut, Backspace goes
    // to the game itself.
    ChatBar bar;
    std::string dialog;
    if (!look_both(bar, dialog)) return false;
    if (bar.state == ChatBarState::Closed) return believe_shut(bar);
    if (bar.state != ChatBarState::Open || !dialog.empty()) return false;
    Result r;
    tidy(r, rub_out_count);
    return !r.left_open;
}

ChatSender::LeftOpen ChatSender::check_left_open(std::string* why) {
    const auto because = [why](LeftOpen what, std::string text) {
        if (why) *why = std::move(text);
        return what;
    };
    ChatBar bar;
    std::string dialog;
    if (!look_both(bar, dialog)) return because(LeftOpen::Unseen, kLeftUnseen);
    if (bar.state == ChatBarState::Closed && believe_shut(bar)) return LeftOpen::Shut;
    if (bar.state != ChatBarState::Open)
        return because(LeftOpen::Unseen, bar.found ? kLeftNeither : kLeftUnseen);
    if (!dialog.empty()) return because(LeftOpen::Busy, dialog_wait(dialog));
    if (!focused()) return because(LeftOpen::Busy, kLeftAway);
    return LeftOpen::Closable;
}

ChatSender::Result ChatSender::send(const ChatText& line) {
    Result r = send_line(line);
    // What the trace says it came to, when it does not say already.
    r.trace.inject = settings_.inject;
    if (r.trace.outcome.empty())
        r.trace.outcome = r.sent         ? "sent"
                        : r.not_now      ? "not started"
                        : r.send_pressed ? (r.stop ? "not seen sent (the game may send it late), Tu chat stops"
                                                   : "not seen sent (the game may send it late)")
                        : r.stop         ? "not sent, Tu chat stops"
                        : r.aborted      ? "put off"
                        : r.left_open    ? "not sent, the line may be open"
                                         : "not sent";
    return r;
}

ChatSender::Result ChatSender::send_line(const ChatText& line) {
    Result r;
    empty_ink_ = -1;
    entered_ = false;
    halted_.clear();
    const std::uint64_t began = clock_ms();
    const ChatMethod how = settings_.method;
    const int clear = settings_.clear_backspaces;
    if (line.empty()) {
        r.trouble = kNothing;
        return r;
    }

    // Worked out before anything is pressed.
    std::vector<ChatKey> keys;
    std::wstring units;
    if (how == ChatMethod::Vk) keys = chat_keys(line.typed, caps_lock());
    else units = widen(line.typed);
    if ((how == ChatMethod::Vk && keys.empty()) || (how != ChatMethod::Vk && units.empty())) {
        r.trouble = kNothing;
        return r;
    }

    Frame shot;
    const auto not_now = [&](std::uint64_t ms, std::string why) {
        r.not_now = true;
        r.wait_ms = ms;
        r.trouble = std::move(why);
        return r;
    };
    // Stopped part way for something that is not the line's own trouble:
    // tried again later, not counted as a failure.
    const auto put_off = [&](std::uint64_t ms) {
        r.aborted = true;
        r.wait_ms = std::max(r.wait_ms, ms);
    };
    const auto stop_why = [&]() -> std::string {
        if (cancelled_) return kStopped;
        return guard_ ? guard_() : std::string();
    };
    // The game lost the front: the keys go nowhere from then on. Waited for
    // a while, and rubbed out and closed if it comes back.
    const auto lost_front = [&](int rub) {
        if (r.trouble.empty()) r.trouble = kNotFront;
        put_off(5000);
        for (int i = 0; i < 100 && !focused() && !cancelled_; ++i) pause(100);
        if (focused()) {
            tidy(r, rub);
        } else {
            r.left_open = true;
            r.trouble = kMaybeLeft;
        }
        return r;
    };
    // Given up part way with the line open, or perhaps open: rubbed out and
    // closed, never sent -- or, not seen open, left to whoever watches it.
    const auto give_up = [&](std::string why, int rub) {
        r.trouble = std::move(why);
        if (!focused()) return lost_front(rub);
        tidy(r, rub);
        return r;
    };
    // The same, for a reason that is no fault of the line's.
    const auto abort_line = [&](std::string why, int rub, std::uint64_t ms) {
        put_off(ms);
        return give_up(std::move(why), rub);
    };
    // A wait ended early (wait): a stop, or the guard's reason -- HP under
    // the potion's mark, another hold -- which ends the line as it does
    // between keys: rubbed out, closed, put off.
    const auto halt = [&](int rub) {
        if (cancelled_ || halted_.empty()) return abort_line(kStopped, rub, 0);
        return abort_line(halted_, rub, 5000);
    };
    // Up to `looks` looks at the bar alone, a quarter of a second apart,
    // until one shows it open; what they saw, the last of them in r.last. The
    // dialogs do not matter yet: nothing but Backspace goes into the line
    // before a look that takes them in as well. False when a stop came
    // meanwhile.
    // Each look after an Enter pressed to open the line goes in the trace,
    // with how long after the Enter it was.
    const auto traced = [&](const ChatBar& now) {
        if (r.trace.enters.empty()) return;
        r.trace.enters.back().looks.emplace_back(static_cast<int>(clock_ms() - enter_ms_), now.state);
    };
    const auto watch = [&](int looks, bool& saw_open, bool& saw_shut, bool& saw_neither) {
        for (int i = 0; i < looks && !saw_open; ++i) {
            if (!wait(250)) return false;
            ChatBar now;
            look_bar(now, &shot);
            r.last = now;
            traced(now);
            if (now.state == ChatBarState::Open) saw_open = true;
            else if (now.state == ChatBarState::Closed) saw_shut = true;
            else if (now.found) saw_neither = true;
        }
        return true;
    };
    // An Enter pressed to open the line: its hold, as chat.ini says or its
    // own, and its place in the trace.
    const auto open_enter = [&]() {
        const int held = hold(60, 130);
        if (!enter(true, held)) return false;
        r.trace.enters.push_back(ChatTrace::Enter{ held, {} });
        return true;
    };

    // ---- open it ------------------------------------------------------------
    // Enter, each time on a look just taken that shows the line shut and no
    // dialog. Pressed again only when the line was seen still shut after it,
    // so the presses never get out of step with the line -- and max_enters
    // at most, as shipped two: a game slow enough to take both late is left
    // as it was found, shut, and the line is reported as maybe open, for
    // whoever watches it after. Each is watched enter_gap_ms for the line to
    // open, a look every 250 ms.
    const int max_enters = std::clamp(settings_.max_enters, 1, ChatSettings::kMaxEnters);
    const int enter_looks = std::max(1, (settings_.enter_gap_ms + 125) / 250);
    bool opened = false;
    for (int press = 1; press <= max_enters && !opened; ++press) {
        if (cancelled_) {
            // Before the first Enter nothing has been pressed and the line is
            // as it was, shut. After it, whether the game took it is looked
            // for before anything else.
            if (press > 1) return abort_line(kStopped, 0, 0);
            r.trouble = kStopped;
            put_off(0);
            return r;
        }
        ChatBar bar;
        std::string dialog;
        const bool looked = look_both(bar, dialog, &shot);
        r.last = bar;
        if (press > 1) traced(bar);
        if (press > 1 && bar.state == ChatBarState::Open) {
            opened = true;      // it did open, just later than it was looked for
            break;
        }
        if (press == 1) {
            // Nothing pressed yet: anything in the way is a "not now".
            if (!looked) return not_now(5000, kNoShot);
            if (!dialog.empty()) return not_now(5000, dialog_wait(dialog));
            if (bar.state == ChatBarState::Open) return not_now(15000, kBarOpen);
            if (bar.state != ChatBarState::Closed)
                return not_now(10000, bar.trouble.empty() ? std::string(kNoBar) : bar.trouble);
            if (ready_) {
                const std::string why = ready_();
                if (!why.empty()) return not_now(5000, why);
            }
            // Asked again right before the Enter: the look above takes a
            // while, and a stop that came meanwhile is not to be followed by
            // an Enter that nothing is left to see to.
            const std::string why = stop_why();
            if (!why.empty()) {
                if (!cancelled_) return not_now(5000, why);
                r.trouble = kStopped;
                put_off(0);
                return r;
            }
            if (!focused() || !open_enter()) return not_now(5000, kNotFront);
        } else {
            // One Enter in already, the line seen shut after it -- which the
            // game may yet take, late.
            const std::string why = stop_why();
            if (!why.empty()) return abort_line(why, 0, cancelled_ ? 0 : 5000);
            r.left_open = true;
            if (!looked || !dialog.empty() || bar.state != ChatBarState::Closed || !focused()) {
                r.trouble = !looked ? std::string(kNoShot)
                          : !dialog.empty() ? dialog_wait(dialog)
                          : bar.state != ChatBarState::Closed ? std::string(kNoBar)
                                                               : std::string(kNotFront);
                if (!dialog.empty()) r.dialog = dialog;
                put_off(bar.state != ChatBarState::Closed && dialog.empty() ? 10000 : 5000);
                return r;
            }
            if (!open_enter()) {
                r.trouble = kNotFront;
                put_off(5000);
                return r;
            }
        }

        // Looked at a few times: a game that is slow to draw the line open is
        // not pressed again, which would shut it.
        bool saw_open = false, saw_shut = false, saw_neither = false;
        if (!watch(enter_looks, saw_open, saw_shut, saw_neither)) return halt(clear);
        if (!saw_open && !saw_shut && !watch(2, saw_open, saw_shut, saw_neither)) return halt(clear);
        if (saw_open) {
            opened = true;
            break;
        }
        if (!saw_shut) {
            // Neither picture on any look after the Enter. One look more, the
            // dialogs with it, says which way that was.
            ChatBar now;
            std::string d;
            const bool seen = look_both(now, d, &shot);
            r.last = now;
            traced(now);
            if (seen && now.state == ChatBarState::Open) {
                opened = true;      // late, but open
                break;
            }
            if (!(seen && now.state == ChatBarState::Closed)) {
                keep(shot, "open_unknown", &now);
                r.left_open = true;
                if (!saw_neither && !now.found) {
                    // The bar not to be seen at all -- a window over the
                    // corner, a loading screen. Nothing more is pressed into
                    // a game that cannot be seen: the line may be open, and
                    // is left to whoever watches it after.
                    r.trouble = kLostOpening;
                    put_off(10000);
                    return r;
                }
                // The bar in its place and its field like neither picture:
                // most likely the line did open, and the live client draws
                // an open line some other way. Nothing has been typed, so one
                // Enter more shuts it again, sending nothing -- on a look that
                // still shows the bar so and no dialog. Every line would go
                // the same way, so Tự chat stops.
                r.stop = true;
                r.trouble = kNotKnown;
                if (seen && now.found && now.state == ChatBarState::Unknown && d.empty() &&
                    stop_why().empty() && focused() && open_enter()) {
                    bool open_again = false, shut_again = false, neither_again = false;
                    watch(3, open_again, shut_again, neither_again);
                    ChatBar last = r.last;
                    if (!open_again && believe_shut(last)) r.left_open = false;
                    r.last = last;
                }
                return r;
            }
            // Shut after all: as if it had been seen so.
        }
        if (press < max_enters) say("ô chat chưa mở, bấm Enter lần nữa");
    }
    if (!opened) {
        keep(shot, "not_opened", &r.last);
        r.trouble = not_opened(static_cast<int>(r.trace.enters.size()));
        r.trace.outcome = "not_opened";
        r.left_open = true;     // either Enter may yet be taken, late
        return r;
    }
    r.left_open = false;
    r.opened = true;
    {
        char seen[200];
        std::snprintf(seen, sizeof seen,
                      "ô chat đã mở (%d điểm màu ô chat đang mở, giống ô chat đóng %.3f, vết chữ %d)",
                      r.last.dark, static_cast<double>(r.last.closed_score), r.last.ink);
        say(seen);
    }

    // ---- clear it -----------------------------------------------------------
    // Whatever was left in it, as the commercial client clears it: six
    // Backspaces. Then whatever the field still shows is rubbed at -- after
    // the look that showed it, so that the keys reach the game before
    // anything typed later, however old that look's frame -- until it shows
    // no more than an empty line's caret, and then a caret's width more: one
    // or two characters left over look no wider than the caret. Rounds go on
    // while each takes something away, up to kMostRubbed in all. If the field
    // still shows more than a caret, nothing is typed: what is left of a line
    // the player shut without sending would go out in front of this one.
    if (!wait(200)) return halt(clear);
    for (int i = 0; i < clear; ++i) {
        const int held = hold(40, 70);
        r.trace.backspace_min = i == 0 ? held : std::min(r.trace.backspace_min, held);
        r.trace.backspace_max = std::max(r.trace.backspace_max, held);
        if (!key(VK_BACK, false, held)) return lost_front(clear);
        if (!wait(30)) return halt(clear);
    }
    if (!wait(50)) return halt(clear);

    ChatBar before;
    std::string dialog;
    look_both(before, dialog, &shot);
    r.last = before;
    int rubbed = 0;             // Backspaces past the commercial client's
    int shown_before = -1;      // what the field showed before the last round
    bool armed = false;         // a caret's width rubbed since a look showed no more
    while (dialog.empty() && before.state == ChatBarState::Open && before.ink > 0 &&
           !(armed && before.ink <= settings_.empty_ink)) {
        int more;
        if (before.ink <= settings_.empty_ink) {
            more = rub_for(settings_.empty_ink);
            armed = true;
        } else {
            // A field drawn on to its end may hold more than it shows; short
            // of that, a round that took nothing away says that what shows
            // is not something Backspace removes.
            const bool full = before.ink >= before.field.w - 4;
            if ((shown_before >= 0 && before.ink >= shown_before - 2 && !full) ||
                rubbed >= kMostRubbed) {
                keep(shot, "not_cleared", &before);
                say("ô chat vẫn hiện tới " + std::to_string(before.ink) + " điểm sau khi xoá " +
                    std::to_string(clear + rubbed) + " lần: nếu ô chat đang trống thì empty_ink=" +
                    std::to_string(settings_.empty_ink) + " trong chat.ini cần lớn hơn");
                return give_up(kNotCleared, 0);
            }
            more = std::min(rub_for(before.ink), kMostRubbed - rubbed);
            armed = false;
            say("ô chat còn chữ (vết chữ " + std::to_string(before.ink) + " điểm), xoá thêm " +
                std::to_string(more) + " lần");
        }
        shown_before = before.ink;
        if (!rub_out(more)) return lost_front(more + clear);
        rubbed += more;
        if (!wait(kDrawnMs)) return halt(rubbed + clear);
        look_both(before, dialog, &shot);
        r.last = before;
    }
    if (!dialog.empty()) {
        r.dialog = dialog;
        r.left_open = true;
        r.trouble = dialog_wait(dialog);
        put_off(5000);
        return r;
    }
    if (before.state != ChatBarState::Open) {
        keep(shot, "lost_before_typing", &before);
        if (before.state == ChatBarState::Closed && believe_shut(before)) {
            r.last = before;
            r.trouble = "Ô chat tự đóng lại trước khi gõ.";
            return r;
        }
        r.last = before;
        r.left_open = true;
        r.trouble = "Không thấy ô chat trước khi gõ.";
        put_off(10000);
        return r;
    }
    // The empty line's look, which what is closed with Enter later is judged
    // by: no wider than a caret, or the rounds above would not have ended.
    empty_ink_ = before.ink <= settings_.empty_ink ? before.ink : -1;
    r.empty_ink = empty_ink_;
    r.trace.cleared_ink = before.ink;
    say("ô chat đã xoá (vết chữ " + std::to_string(before.ink) + " điểm)");
    // chat.ini's type_wait_ms between the line cleared and its first letter:
    // none as shipped.
    if (settings_.type_wait_ms > 0 && !wait(settings_.type_wait_ms)) return halt(clear);

    // ---- type it ------------------------------------------------------------
    // A key at a time, as the commercial client types: 33 ms apart, each held
    // for 50 ms (or as chat.ini's holds say). Asked between keys whether to
    // go on. The gap before each key is spent looking at the bar, and the key
    // goes only if it still shows the line open: a line shut under it -- a
    // click in the game, a map change -- makes every key after that a skill
    // key or a potion. Every dozen keys the look takes in the dialogs as well.
    //
    // Then the field has to show the text: past where the caret was, about as
    // far as that many characters reach -- two pixels for each that is not a
    // space, five at least: the narrowest letters the game draws, i and l, are
    // a pixel wide with one between them, and a caret blinking on is four.
    // Looked at kDrawnMs after the last key and, nothing there yet, twice
    // more a quarter of a second apart: a game that draws late is not taken
    // for one that took nothing. Nothing at all, the line still open and as
    // empty as it was, and chat.ini's type_tries allowing: typed once more.
    // Never into a line that took any of it, which would hold it twice.
    const std::size_t count = how == ChatMethod::Vk ? keys.size() : units.size();
    int marks = 0;
    for (const char c : line.typed)
        if (c != ' ' && (static_cast<unsigned char>(c) & 0xC0) != 0x80) ++marks;
    const int need = std::min(60, std::max(5, 2 * marks));
    const int tries = std::clamp(settings_.type_tries, 1, ChatSettings::kMaxTypeTries);
    ChatBar shown;
    for (int attempt = 1;; ++attempt) {
        r.trace.tries = attempt;
        for (std::size_t i = 0; i < count; ++i) {
            const std::string why = stop_why();
            if (!why.empty()) return abort_line(why, r.typed + 2, 5000);
            const std::uint64_t started = clock_ms();
            ChatBar mid;
            std::string d;
            const bool looked = i > 0 && i % kKeysPerLook == 0 ? look_both(mid, d, &shot)
                                                               : look_bar(mid, &shot);
            r.last = mid;
            if (i > 0) r.trace.inks.push_back(mid.ink);
            if (!d.empty()) {
                // What was typed rubbed out at once -- an Enter the player
                // presses for the dialog could send it -- but only into a line
                // still seen open; and a line seen shut believed so only on a
                // second look.
                const bool shut = mid.state == ChatBarState::Closed && believe_shut(mid);
                r.last = mid;
                if (mid.state == ChatBarState::Open) rub_out(r.typed + clear);
                r.dialog = d;
                r.left_open = !shut;
                r.trouble = dialog_wait(d) + " Câu chưa gửi.";
                put_off(5000);
                return r;
            }
            if (looked && mid.state == ChatBarState::Closed) {
                // Looked at once more, a frame later, before it is believed --
                // a line wrongly taken for shut would be left open with its
                // half a line in it -- and with no key pressed meanwhile.
                pause(kKeyGap);
                ChatBar again;
                if (look_bar(again, &shot)) mid = again;
                r.last = mid;
            }
            if (looked && mid.state == ChatBarState::Closed) {
                // Not one key more, a Backspace least of all: with the line
                // shut it is the game's.
                keep(shot, "shut_while_typing", &mid);
                r.trouble = kShutTyping;
                return r;
            }
            if (!looked || mid.state != ChatBarState::Open) {
                // Out of sight: rubbed out only once a look shows it open again.
                keep(shot, "lost_while_typing", &mid);
                return abort_line(kLostTyping, r.typed + 2, 10000);
            }
            const auto spent = static_cast<int>(clock_ms() - started);
            if (!wait(std::max(0, kKeyGap - spent))) return halt(r.typed + 2);
            const int held = hold(50, 50);
            const bool went = how == ChatMethod::Vk ? key(keys[i].vk, keys[i].shift, held)
                                                    : text(std::wstring(1, units[i]), how);
            if (!went) return lost_front(r.typed + 2);
            if (how == ChatMethod::Vk) {
                r.trace.letter_min = r.trace.letters == 0 ? held : std::min(r.trace.letter_min, held);
                r.trace.letter_max = std::max(r.trace.letter_max, held);
            }
            ++r.trace.letters;
            ++r.typed;
        }

        // ---- check it arrived ------------------------------------------------
        const std::uint64_t last_key = clock_ms();
        r.trace.after.clear();
        bool arrived = false;
        for (int look = 0; look < 3 && !arrived; ++look) {
            if (!wait(look == 0 ? kDrawnMs : 250)) return halt(r.typed + 2);
            look_both(shown, dialog, &shot);
            r.last = shown;
            r.trace.after.emplace_back(static_cast<int>(clock_ms() - last_key), shown.ink);
            if (!dialog.empty()) {
                // Enter now would answer the dialog. What was typed is rubbed
                // out, with the line seen open, and the line waits for it to go.
                if (shown.state == ChatBarState::Open) rub_out(r.typed + clear);
                r.dialog = dialog;
                r.left_open = !(shown.state == ChatBarState::Closed && believe_shut(shown));
                r.trouble = dialog_wait(dialog) + " Câu chưa gửi.";
                put_off(5000);
                return r;
            }
            if (shown.state != ChatBarState::Open) {
                keep(shot, "lost_before_send", &shown);
                if (shown.state == ChatBarState::Closed && believe_shut(shown)) {
                    r.last = shown;
                    r.trouble = "Ô chat tự đóng lại trước khi gửi — câu chưa gửi.";
                    return r;
                }
                // Out of sight: not a key into a game that cannot be seen, a
                // Backspace least of all. Left to whoever watches the line
                // after, which rubs it out once a look shows it open.
                r.last = shown;
                r.left_open = true;
                r.trouble = "Không thấy ô chat trước khi gửi — câu chưa gửi.";
                put_off(10000);
                return r;
            }
            arrived = shown.ink >= before.ink + need;
        }
        if (arrived) break;
        if (attempt < tries && shown.ink <= before.ink + 2) {
            say("ô chat không hiện chữ vừa gõ (vết chữ " + std::to_string(shown.ink) + " điểm) — gõ lại lần " +
                std::to_string(attempt + 1));
            continue;
        }
        keep(shot, "no_text", &shown);
        say("ô chat không hiện chữ vừa gõ (vết chữ " + std::to_string(before.ink) + " -> " +
            std::to_string(shown.ink) + " điểm, cần thêm " + std::to_string(need) + ")");
        r.trace.outcome = "no_text";
        return give_up(std::string("Game không nhận chữ gõ vào ô chat (cách gõ ") + chat_method_name(how) +
                           ") — câu chưa gửi.",
                       r.typed + clear);
    }
    // A stop, or the guard, asked once more: the look just taken, with its
    // dialog check, takes a quarter of a second, and a DỪNG or a hold that
    // came meanwhile is not followed by the line going out. Rubbed out and
    // closed, as one stopped between keys is.
    {
        const std::string why = stop_why();
        if (!why.empty()) return abort_line(why, r.typed + 2, 5000);
    }
    // On the look just taken, which shows the line open with the text in it
    // and no dialog.
    r.trace.send_hold = hold(50, 50);
    if (!focused()) return lost_front(r.typed + 2);
    // The line is on its way to the chat from this key, and whoever runs the
    // sender is told before it goes down -- not after its hold, nor when the
    // looks below end: a game that acts on the key-down shows the line while
    // the key is held, chat.ini's hold up to half a second, and a game that
    // takes the key a second or two late shows it while the looks are still
    // going, or after they have given up on it (R8R2-1, R8bR-4). An Enter
    // that then does not go in leaves a line told of that was never said,
    // which costs no more than one the game drops.
    if (send_pressed_) send_pressed_();
    if (!enter(false, r.trace.send_hold)) return lost_front(r.typed + 2);
    r.send_pressed = true;

    // ---- closed? -------------------------------------------------------------
    // Watched for a second whatever a stop says: the line has gone, and what
    // is left to find out is whether it closed behind itself. The bar alone:
    // nothing more is pressed unless it shows open.
    ChatBarState seen = ChatBarState::Unknown;
    ChatBar after, open_look;
    for (int i = 0; i < 4 && seen != ChatBarState::Closed; ++i) {
        pause(250);
        look_bar(after, &shot);
        if (after.state == ChatBarState::Closed && believe_shut(after)) seen = ChatBarState::Closed;
        if (after.state == ChatBarState::Open) {
            seen = ChatBarState::Open;
            open_look = after;
        }
    }
    r.last = after;
    if (seen == ChatBarState::Closed) {
        r.sent = true;
        // How long everything else was held off, for the ten seconds a line
        // is meant to take at most: from the hold, which starts before the
        // line does, when the one running it says when that was.
        const std::uint64_t done = clock_ms();
        char took[160];
        if (held_since_ != 0 && held_since_ <= began)
            std::snprintf(took, sizeof took, "ô chat đã đóng (cả câu mất %.1f giây, bot bị giữ %.1f giây)",
                          static_cast<double>(done - began) / 1000.0,
                          static_cast<double>(done - held_since_) / 1000.0);
        else
            std::snprintf(took, sizeof took, "ô chat đã đóng (cả câu mất %.1f giây)",
                          static_cast<double>(done - began) / 1000.0);
        say(took);
        return r;
    }
    if (seen == ChatBarState::Open) {
        keep(shot, "still_open", &open_look);
        if (open_look.ink <= before.ink + 2) {
            // Empty and open: the line went, and the line stayed open. Shut
            // with another Enter, which sends nothing from an empty line.
            r.sent = true;
            tidy(r, 0);
        } else {
            // The text is still there: the Enter has not taken -- dropped, or
            // to be taken late, which sends the line all the same. Rubbed out
            // and closed rather than pressed again, which could send it twice.
            r.trouble = "Bấm Enter mà chưa thấy câu đi — đã xoá chữ còn trong ô chat. Game nhận Enter trễ thì "
                        "câu vẫn được gửi.";
            tidy(r, r.typed + clear);
        }
        if (r.left_open) {
            r.stop = true;
            r.trouble = kNotClosed;
        }
        return r;
    }
    // Not to be seen at all. The line was most likely sent -- the Enter went
    // in on a line showing the text -- but whether it closed is not known.
    keep(shot, "close_unknown", &after);
    r.sent = true;
    r.left_open = true;
    r.trouble = "Đã bấm Enter nhưng không thấy ô chat để biết đã đóng chưa.";
    return r;
}

// ------------------------------------------------------------------- doubt

void ChatDoubt::note(const ChatSender::Result& r, int rub, std::uint64_t now) {
    if (!r.left_open) return;
    if (!active_) {
        active_ = true;
        empty_ink_ = -1;
        rub_ = 0;
    }
    if (r.empty_ink >= 0) empty_ink_ = r.empty_ink;
    rub_ = std::max(rub_, rub);
    next_ms_ = now + 1000;
    if (hooks_.hold) hooks_.hold();
}

bool ChatDoubt::step(ChatSender& sender, std::uint64_t now) {
    if (!active_) return false;
    if (hooks_.hold) hooks_.hold();
    // The game gone -- closed, crashed, or logged back into as a new one --
    // and the line it was open in gone with it.
    if (hooks_.same_game && !hooks_.same_game()) {
        end("Game đã đóng hoặc đã mở lại, nên ô chat cũ không còn.");
        return true;
    }
    if (now < next_ms_) return false;
    next_ms_ = now + 1000;

    std::string why;
    switch (sender.check_left_open(&why)) {
        case ChatSender::LeftOpen::Shut:
            end("Ô chat đã đóng.");
            return true;
        case ChatSender::LeftOpen::Closable: {
            // Closed only with nobody at the mouse or keyboard: the open line
            // may be the player's by now, typing in it.
            bool tried = false, closed = false;
            sender.set_empty_ink(empty_ink_);
            if (hooks_.when_quiet)
                hooks_.when_quiet([&] {
                    tried = true;
                    closed = sender.close_left_open(rub_);
                    return closed;
                });
            if (closed) {
                end("Đã đóng ô chat còn mở.");
                return true;
            }
            if (tried && hooks_.say)
                hooks_.say("Chưa đóng được ô chat: trong đó có thể còn chữ không xoá được. Hãy xem ô "
                           "chat trong game: xoá chữ trong đó rồi bấm Enter.");
            if (tried) next_ms_ = now + 3000;
            return false;
        }
        case ChatSender::LeftOpen::Busy:
        case ChatSender::LeftOpen::Unseen:
            if (hooks_.say) hooks_.say(why);
            return false;
    }
    return false;
}

void ChatDoubt::end(const std::string& why) {
    active_ = false;
    if (hooks_.release) hooks_.release(why);
}

// ------------------------------------------------------------ said lately

std::uint64_t wall_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

namespace {

/// A whole number from `at` up to the space after it, which is passed too;
/// false for anything else.
bool said_number(const std::string& row, std::size_t& at, std::uint64_t& out) {
    const std::size_t from = at;
    std::uint64_t n = 0;
    while (at < row.size() && row[at] >= '0' && row[at] <= '9') {
        if (n > (~0ull - 9) / 10) return false;
        n = n * 10 + static_cast<std::uint64_t>(row[at] - '0');
        ++at;
    }
    if (at == from || at >= row.size() || row[at] != ' ') return false;
    ++at;
    out = n;
    return true;
}

/// The whole of a small file; empty when there is none. A megabyte at most.
std::string said_file_text(const std::string& path) {
    std::string text;
    std::FILE* f = open_file(path, "rb");
    if (f == nullptr) return text;
    char buf[4096];
    for (std::size_t n = 0; text.size() < (1u << 20) && (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
        text.append(buf, n);
    std::fclose(f);
    return text;
}

} // namespace

std::string SaidLately::default_path() {
    // Outside the program folder, as the webhook's SecretStore keeps its file:
    // a new version is unzipped into a new folder (START-HERE, UPDATING), and
    // the line the old one typed is still on the chat when it starts. Beside
    // the executable only when this Windows user's folder is not known.
    static const std::string path = [] {
        wchar_t env[MAX_PATH * 2] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", env, static_cast<DWORD>(std::size(env)));
        if (n > 0 && n < std::size(env)) return wide_to_utf8(std::wstring(env) + L"\\GoBot\\said_lately.txt");
        return exe_relative("chat_debug\\said_lately.txt");
    }();
    return path;
}

std::string SaidLately::format(const Line& l) {
    std::string text = l.text.substr(0, kLongestText);
    for (char& c : text)
        if (c == '\r' || c == '\n') c = ' ';
    return std::to_string(l.steady_ms) + " " + std::to_string(l.wall_ms) + " " + text + "\n";
}

std::vector<SaidLately::Line> SaidLately::parse(const std::string& text) {
    std::vector<Line> out;
    // A row is finished by its line end: one without, being written as this
    // reads, may hold half its text.
    for (std::size_t start = 0, end = 0; (end = text.find('\n', start)) != std::string::npos; start = end + 1) {
        std::string row = text.substr(start, end - start);
        if (!row.empty() && row.back() == '\r') row.pop_back();
        Line l;
        std::size_t at = 0;
        if (!said_number(row, at, l.steady_ms) || !said_number(row, at, l.wall_ms)) continue;
        l.text = row.substr(at);
        if (!l.text.empty()) out.push_back(std::move(l));
    }
    return out;
}

std::vector<SaidLately::Line> SaidLately::kept(const std::vector<Line>& lines, std::uint64_t steady_now,
                                               std::uint64_t wall_now) {
    // When the steady clock started, by the system clock: now, and as each
    // row saw it.
    const auto start_of = [](std::uint64_t wall, std::uint64_t steady) {
        return static_cast<std::int64_t>(wall) - static_cast<std::int64_t>(steady);
    };
    const std::int64_t start_now = start_of(wall_now, steady_now);
    std::vector<Line> out;
    for (const Line& l : lines) {
        if (l.steady_ms > steady_now || steady_now - l.steady_ms > kKeepMs) continue;
        const std::int64_t apart = start_of(l.wall_ms, l.steady_ms) - start_now;
        if (apart > kSameStartMs || apart < -kSameStartMs) continue;     // another start of the machine
        out.push_back(l);
    }
    std::stable_sort(out.begin(), out.end(), [](const Line& a, const Line& b) { return a.steady_ms < b.steady_ms; });
    if (out.size() > kKeep) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(kKeep));
    return out;
}

bool SaidLately::add(const std::string& typed, std::uint64_t steady, std::uint64_t wall) const {
    if (path_.empty() || typed.empty()) return false;
    const std::size_t slash = path_.find_last_of("\\/");
    if (slash != std::string::npos) make_folder(path_.substr(0, slash));
    std::FILE* f = open_file(path_, "ab");
    if (f == nullptr) return false;
    const std::string row = format(Line{ typed, steady, wall });
    bool ok = std::fwrite(row.data(), 1, row.size(), f) == row.size();
    const long size = ok ? std::ftell(f) : -1;
    ok = std::fclose(f) == 0 && ok;
    if (!ok) return false;
    if (size > kCutAt) {
        // Cut down to the rows still of use, written beside it -- a name of
        // this process's own, two may be at it -- and moved over it, so that
        // a reader never finds half of it. A reader that has it open at that
        // moment makes the move fail: it is cut on a later line instead. A
        // row another process appends meanwhile is lost with the old file;
        // two processes typing at once is what gobot_cli refuses.
        std::string text;
        for (const Line& l : kept(parse(said_file_text(path_)), steady, wall)) text += format(l);
        const std::string fresh = path_ + "." + std::to_string(GetCurrentProcessId()) + ".tmp";
        std::FILE* o = open_file(fresh, "wb");
        if (o != nullptr) {
            const bool wrote = std::fwrite(text.data(), 1, text.size(), o) == text.size();
            const bool closed = std::fclose(o) == 0;
            if (!(wrote && closed && move_file(fresh, path_))) remove_file(fresh);
        }
    }
    return true;
}

SaidLately::Said SaidLately::read(std::uint64_t steady_now, std::uint64_t wall_now) const {
    Said out;
    if (path_.empty()) return out;
    for (const Line& l : kept(parse(said_file_text(path_)), steady_now, wall_now)) out.emplace_back(l.text, l.steady_ms);
    return out;
}

// ---------------------------------------------------------------- machine

std::vector<std::string> vietnamese_typers_running() {
    static const char* const kNames[] = {
        "unikeynt.exe", "unikey.exe", "evkey.exe", "evkey32.exe", "evkey64.exe",
        "openkey.exe", "openkey32.exe", "openkey64.exe", "vietkey.exe",
    };
    std::vector<std::string> found;
    std::vector<std::string> seen;      // lower case, to say each once
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            const std::string exe = narrow(pe.szExeFile);
            const std::string name = lower(exe);
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
            for (const char* want : kNames)
                if (name == want) {
                    seen.push_back(name);
                    found.push_back(exe);
                }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

std::uint16_t window_keyboard_layout(void* hwnd) {
    if (hwnd == nullptr || !IsWindow(static_cast<HWND>(hwnd))) return 0;
    const DWORD thread = GetWindowThreadProcessId(static_cast<HWND>(hwnd), nullptr);
    if (thread == 0) return 0;
    const HKL layout = GetKeyboardLayout(thread);
    return static_cast<std::uint16_t>(reinterpret_cast<std::uintptr_t>(layout) & 0xFFFF);
}

} // namespace gobot
