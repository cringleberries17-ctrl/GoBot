#include "gobot/alerts.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string_view>

#include <windows.h>
// Not pulled in by windows.h under WIN32_LEAN_AND_MEAN.
#include <wincrypt.h>
#include <winhttp.h>
#include <shlobj.h>
#include <knownfolders.h>

namespace gobot {

namespace {

// ---- text -------------------------------------------------------------------

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

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [](unsigned char c) { return c >= '0' && c <= '9'; });
}

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

void wipe_wide(std::wstring& w) {
    w.resize(w.capacity());
    if (!w.empty()) SecureZeroMemory(w.data(), w.size() * sizeof(wchar_t));
    w.clear();
}

/// How many bytes the UTF-8 character starting with `c` has; 0 when no
/// character starts with it.
std::size_t utf8_length(unsigned char c) {
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF4) return 4;
    return 0;
}

/// Whether the `n` bytes at `at` are one well-formed UTF-8 character: no
/// overlong forms, no surrogates, nothing past U+10FFFF.
bool utf8_char(const std::string& s, std::size_t at, std::size_t n) {
    if (n == 0 || at + n > s.size()) return false;
    for (std::size_t k = 1; k < n; ++k)
        if ((static_cast<unsigned char>(s[at + k]) & 0xC0) != 0x80) return false;
    const unsigned char a = static_cast<unsigned char>(s[at]);
    const unsigned char b = n > 1 ? static_cast<unsigned char>(s[at + 1]) : 0;
    if (n == 3 && a == 0xE0 && b < 0xA0) return false;
    if (n == 3 && a == 0xED && b > 0x9F) return false;
    if (n == 4 && a == 0xF0 && b < 0x90) return false;
    if (n == 4 && a == 0xF4 && b > 0x8F) return false;
    return true;
}

/// Calls `each(start, length)` for every character of `s`; a byte that is not
/// part of a well-formed one counts as a character of its own.
template <typename F>
void each_char(const std::string& s, F each) {
    for (std::size_t at = 0; at < s.size();) {
        std::size_t n = utf8_length(static_cast<unsigned char>(s[at]));
        const bool good = utf8_char(s, at, n);
        if (!good) n = 1;
        if (!each(at, n, good)) return;
        at += n;
    }
}

/// `s` with anything that is not well-formed UTF-8 as U+FFFD: Discord refuses
/// a body that is not UTF-8, and a name read off the screen is not trusted
/// to be.
std::string valid_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    each_char(s, [&](std::size_t at, std::size_t n, bool good) {
        if (good) out.append(s, at, n);
        else out += "\xEF\xBF\xBD";
        return true;
    });
    return out;
}

/// What a person typed or what was read off the screen, made fit for a
/// message: no control characters, no '@' or '`', trimmed, at most `max`
/// characters.
std::string clean_free_text(const std::string& raw, std::size_t max) {
    std::string kept;
    for (const char ch : valid_utf8(raw)) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F || c == '@' || c == '`') continue;
        kept += ch;
    }
    kept = trim(kept);
    std::string out;
    std::size_t chars = 0;
    each_char(kept, [&](std::size_t at, std::size_t n, bool) {
        if (chars == max) return false;
        out.append(kept, at, n);
        ++chars;
        return true;
    });
    return trim(out);
}

/// What a message with "×N" ends with.
std::string span(const AlertBatch& b, const AlertClock& clock) {
    return " (×" + std::to_string(b.n) + ", " + alert_hhmm(clock.local(b.first.wall)) + "–" +
           alert_hhmm(clock.local(b.last.wall)) + ")";
}

/// When a batch happened, on the wall clock: the queue's own notices are
/// stamped only on the steady one.
std::int64_t first_wall(const AlertBatch& b, const AlertClock& clock) {
    return b.kind == AlertBatch::Kind::CapNotice ? clock.wall(b.first.ms) : b.first.wall;
}

std::string body_of(const AlertBatch& b, const AlertClock& clock) {
    switch (b.kind) {
        case AlertBatch::Kind::Summary:
            return alert_text::offline_summary(b.n, alert_hhmm(clock.local(b.last.wall)));
        case AlertBatch::Kind::CapNotice:
            return alert_text::too_many(alert_hhmm(clock.local(clock.wall(b.resume_ms))));
        case AlertBatch::Kind::Test:
            return b.text;
        case AlertBatch::Kind::Event:
            return b.n > 1 ? b.text + span(b, clock) : b.text;
    }
    return b.text;
}

std::string when_text(const AlertBatch& b, std::uint64_t now_ms, const AlertClock& clock) {
    const std::tm t = clock.local(first_wall(b, clock));
    std::string s = alert_hhmm(t);
    // More than an hour late -- after a night offline -- the time alone could
    // be yesterday's or today's.
    if (now_ms > b.first.ms + 3'600'000ull) {
        char date[16];
        std::snprintf(date, sizeof date, " %02d/%02d", t.tm_mday, t.tm_mon + 1);
        s += date;
    }
    return s;
}

/// printf into a string, for a log line made now and written later.
template <typename... Args>
std::string format(const char* fmt, Args... args) {
    char buf[512];
    std::snprintf(buf, sizeof buf, fmt, args...);
    return buf;
}

std::string seconds_vi(std::uint64_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(ms) / 1000.0);
    std::string s = buf;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

// ---- the file -----------------------------------------------------------------

/// What an alerts.ini starts as when the tab has to make one. The shipped
/// file says more; this is enough to be read back.
const char* const kNewFile =
    "# Thông báo: những gì GoBot báo, và báo qua đâu. Thẻ Thông báo trên màn hình\n"
    "# đơn giản ghi file này; file alerts.ini đi kèm GoBot giải thích từng dòng.\n"
    "# Đường dẫn webhook không bao giờ nằm ở đây.\n"
    "\n"
    "[Alerts]\n"
    "enabled=false\n"
    "\n"
    "[Events]\n";

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool parse_bool(const std::string& v, bool fallback) {
    const std::string s = lower(trim(v));
    if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    return fallback;
}

/// A whole number in base ten, whatever its leading zeros; `fallback` when
/// there is none.
int parse_int(const std::string& v, int fallback) {
    const std::string t = trim(v);
    if (t.empty()) return fallback;
    try {
        return std::stoi(t, nullptr, 10);
    } catch (...) {
        return fallback;
    }
}

// ---- the stored webhook --------------------------------------------------------

/// Mixed into the encryption, so that another program of this user's calling
/// CryptUnprotectData on the file without knowing it gets nothing.
DATA_BLOB entropy() {
    static const char kEntropy[] = "GoBot alerts webhook v1";
    DATA_BLOB b{};
    b.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
    b.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    return b;
}

bool write_bytes(const std::wstring& path, const BYTE* data, DWORD size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, data, size, &wrote, nullptr) && wrote == size &&
                    FlushFileBuffers(f);
    CloseHandle(f);
    return ok;
}

bool read_bytes(const std::wstring& path, std::vector<BYTE>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    // A webhook encrypted is well under a kilobyte; anything this big is not one.
    const bool sized = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024;
    bool ok = false;
    if (sized) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) &&
             got == out.size();
    }
    CloseHandle(f);
    return ok;
}

/// Where Discord's refusal of the stored webhook is kept: beside it, as the
/// number Discord answered with -- nothing of the address. Without it a
/// webhook that is gone was posted to again by every GoBot opened afterwards,
/// and Discord holds that against the address it comes from.
std::wstring refusal_file(const std::string& secret_path) {
    return secret_path.empty() ? std::wstring() : widen(secret_path + ".refused");
}

void keep_refusal(const std::string& secret_path, int code) {
    const std::wstring file = refusal_file(secret_path);
    if (file.empty()) return;
    const std::string text = std::to_string(code);
    if (!write_bytes(file, reinterpret_cast<const BYTE*>(text.data()), static_cast<DWORD>(text.size())))
        GB_WARN("Alerts: could not note beside the webhook that Discord refused it");
}

/// 401, 403 or 404 when Discord refused the webhook last time; 0 otherwise.
int read_refusal(const std::string& secret_path) {
    const std::wstring file = refusal_file(secret_path);
    std::vector<BYTE> bytes;
    if (file.empty() || !read_bytes(file, bytes)) return 0;
    const int code = parse_int(std::string(bytes.begin(), bytes.end()), 0);
    return code == 401 || code == 403 || code == 404 ? code : 0;
}

void forget_refusal(const std::string& secret_path) {
    const std::wstring file = refusal_file(secret_path);
    if (!file.empty()) DeleteFileW(file.c_str());
}

// ---- Discord, over WinHTTP -------------------------------------------------------

/// The number after `key` in a small JSON body, or -1. Only for Discord's
/// "retry_after": a figure in seconds, with a fraction.
double number_after(const std::string& body, const char* key) {
    const std::size_t at = body.find(key);
    if (at == std::string::npos) return -1.0;
    std::size_t p = at + std::strlen(key);
    while (p < body.size() && (body[p] == ' ' || body[p] == ':' || body[p] == '\t')) ++p;
    if (p >= body.size()) return -1.0;
    char* end = nullptr;
    const double v = std::strtod(body.c_str() + p, &end);
    if (end == body.c_str() + p || v < 0.0) return -1.0;
    return v;
}

class DiscordWebhookChannel final : public AlertChannel {
public:
    explicit DiscordWebhookChannel(const WebhookUrl& url)
        : host_(widen(url.host)), path_(widen(url.path)) {}

    ~DiscordWebhookChannel() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close_all();
        }
        wipe_wide(path_);
        wipe_wide(host_);
    }

    ChannelReply send(const std::string& json) override {
        ChannelReply reply;
        HINTERNET request = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_) {
                reply.net_error = ERROR_WINHTTP_OPERATION_CANCELLED;
                return reply;
            }
            // The system's proxy settings as Windows works them out, or the
            // ones set with netsh where that is not offered.
            session_ = WinHttpOpen(L"GoBot", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session_ == nullptr)
                session_ = WinHttpOpen(L"GoBot", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session_ == nullptr) return failed(reply);
            WinHttpSetTimeouts(session_, 5000, 5000, 10000, 10000);
            connect_ = WinHttpConnect(session_, host_.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (connect_ == nullptr) return failed(reply);
            // Over TLS, with the certificate checked: nothing here ever asks
            // WinHTTP to overlook a bad one.
            request_ = WinHttpOpenRequest(connect_, L"POST", path_.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
            if (request_ == nullptr) return failed(reply);
            // A redirect would carry the message somewhere the address did not
            // say, and a webhook has no use for cookies.
            DWORD off = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
            WinHttpSetOption(request_, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
            request = request_;
        }

        // Outside the lock, so that cancel() can close the handle under it,
        // which is what ends a call that is waiting on the network.
        const wchar_t* const headers = L"Content-Type: application/json\r\n";
        const DWORD size = static_cast<DWORD>(json.size());
        BOOL ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
                                     const_cast<char*>(json.data()), size, size, 0);
        if (ok) ok = WinHttpReceiveResponse(request, nullptr);
        if (!ok) {
            reply.net_error = GetLastError();
            finish();
            return reply;
        }

        DWORD status = 0;
        DWORD status_size = sizeof status;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                WINHTTP_NO_HEADER_INDEX))
            reply.http_status = static_cast<int>(status);
        else
            reply.net_error = GetLastError();

        if (reply.http_status == 429) {
            // Discord says how long to wait twice: in the Retry-After header,
            // and with its fraction as retry_after in the body.
            double seconds = -1.0;
            wchar_t header[64] = {};
            DWORD header_size = sizeof header;
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    header, &header_size, WINHTTP_NO_HEADER_INDEX))
                seconds = std::wcstod(header, nullptr);
            const double in_body = number_after(read_body(request, 4096), "\"retry_after\"");
            if (in_body >= 0.0) seconds = in_body;
            if (seconds > 0.0)
                reply.retry_after_ms = static_cast<std::uint64_t>(seconds * 1000.0 + 0.5);
        }
        finish();
        return reply;
    }

    void cancel() override {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        close_all();
    }

private:
    ChannelReply& failed(ChannelReply& reply) {
        reply.net_error = GetLastError();
        close_all();
        return reply;
    }

    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        close_all();
    }

    /// Under mutex_.
    void close_all() {
        if (request_ != nullptr) WinHttpCloseHandle(request_);
        if (connect_ != nullptr) WinHttpCloseHandle(connect_);
        if (session_ != nullptr) WinHttpCloseHandle(session_);
        request_ = connect_ = session_ = nullptr;
    }

    static std::string read_body(HINTERNET request, std::size_t most) {
        std::string body;
        while (body.size() < most) {
            DWORD ready = 0;
            if (!WinHttpQueryDataAvailable(request, &ready) || ready == 0) break;
            const DWORD take = static_cast<DWORD>(std::min<std::size_t>(ready, most - body.size()));
            std::string chunk(take, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(request, chunk.data(), take, &got) || got == 0) break;
            body.append(chunk, 0, got);
        }
        return body;
    }

    std::wstring host_;
    std::wstring path_;
    std::mutex   mutex_;
    HINTERNET    session_ = nullptr;
    HINTERNET    connect_ = nullptr;
    HINTERNET    request_ = nullptr;
    bool         cancelled_ = false;
};

} // namespace

// ------------------------------------------------------------------- events

const std::vector<AlertEventInfo>& alert_events() {
    // Critical: a person has to do something, and nothing will happen until
    // they do. The rest says what happened; most of it sorts itself out.
    static const std::vector<AlertEventInfo> kEvents = {
        { "death",          false, true,  "Nhân vật chết" },
        { "revived",        false, true,  "Đã hồi sinh" },
        { "revive_failed",  true,  true,  "Hồi sinh không được" },
        { "disconnect",     false, true,  "Mất kết nối / game tắt" },
        { "back_in",        false, true,  "Đã vào lại game" },
        { "keeper_gave_up", true,  true,  "Không tự vào lại được (cần người)" },
        { "not_in_world",   false, true,  "Không thấy nhân vật trong game quá" },
        { "trash_stuck",    true,  true,  "Dọn rác bị kẹt" },
        { "chat_stuck",     true,  true,  "Ô chat có thể đang mở" },
        { "chat_stopped",   false, true,  "Tự chat tự dừng" },
        { "quest_trouble",  true,  true,  "Làm nhiệm vụ nhanh gặp lỗi" },
        { "party_change",   false, false, "Tổ đội thay đổi" },
        { "test",           false, true,  nullptr },
    };
    return kEvents;
}

const AlertEventInfo* find_alert_event(const std::string& key) {
    for (const AlertEventInfo& e : alert_events())
        if (key == e.key) return &e;
    return nullptr;
}

// ----------------------------------------------------------------- settings

bool AlertSettings::event_on(const std::string& key) const {
    if (key == "test") return true;
    const auto it = events.find(key);
    if (it != events.end()) return it->second;
    const AlertEventInfo* info = find_alert_event(key);
    return info != nullptr && info->on;
}

std::string AlertSettings::clean_label(const std::string& raw) {
    // A webhook pasted into the wrong box would otherwise be written to the
    // file and put at the head of every message.
    const std::string l = lower(raw);
    if (l.find("/api/webhooks/") != std::string::npos || l.find("/webhooks/") != std::string::npos)
        return {};
    return clean_free_text(raw, kLongestLabel);
}

bool AlertSettings::load(const std::string& path) {
    // Read here rather than by IniFile::load, which wants its sections and keys
    // in exactly one case and is blinded by a BOM in front of the first line --
    // Notepad's "UTF-8 with BOM" -- and a hand-edited file may have both.
    std::string all;
    if (!read_file(path, all)) return false;
    // Looked at through views, so that a webhook somebody wrote in here by
    // hand is never copied out of the buffer, which is wiped at the end.
    const std::string_view text(all);
    std::size_t at = text.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0;
    const auto trimmed = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
        return v;
    };

    AlertSettings s;
    bool have = false;
    std::string section;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::string_view line =
            trimmed(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close == std::string_view::npos) continue;
            section = lower(std::string(trimmed(line.substr(1, close - 1))));
            have = have || section == "alerts";
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key = lower(std::string(trimmed(line.substr(0, eq))));
        if (key.rfind("webhook", 0) == 0) {
            // Never taken from here, in any section: the tab keeps it encrypted
            // outside the program folder, where a copied folder cannot carry it.
            s.webhook_in_file = true;
            continue;
        }
        std::string value(trimmed(line.substr(eq + 1)));
        if (section == "alerts") {
            if (key == "enabled")               s.enabled = parse_bool(value, s.enabled);
            else if (key == "discord")          s.discord = parse_bool(value, s.discord);
            else if (key == "sound")            s.sound = parse_bool(value, s.sound);
            else if (key == "toast")            s.toast = parse_bool(value, s.toast);
            else if (key == "label")            s.label = clean_label(value);
            else if (key == "merge_s")
                s.merge_s = std::clamp(parse_int(value, s.merge_s), kMinMerge, kMaxMerge);
            else if (key == "max_per_minute")
                s.max_per_minute = std::clamp(parse_int(value, s.max_per_minute), 1, kMaxPerMinute);
            else if (key == "max_per_hour")
                s.max_per_hour = std::clamp(parse_int(value, s.max_per_hour), 1, kMaxPerHour);
            else if (key == "not_in_world_min")
                s.not_in_world_min = std::clamp(parse_int(value, s.not_in_world_min), 1, kMaxAway);
        } else if (section == "events") {
            if (key == "party_names") {
                s.party_names = parse_bool(value, s.party_names);
            } else if (key != "test") {
                if (const AlertEventInfo* info = find_alert_event(key))
                    s.events[key] = parse_bool(value, info->on);
            }
        }
        // A label is free text, and somebody may have pasted anything there.
        wipe_string(value);
    }
    wipe_string(all);
    if (!have) return false;
    *this = std::move(s);
    return true;
}

bool AlertSettings::save_key(const std::string& path, const std::string& section,
                             const std::string& key, const std::string& value) {
    {
        // Opened to add to, which makes the file when it is not there and
        // never empties one that is. The path is opened the way the change
        // below opens it, in the system's code page as exe_relative gives it:
        // asked about as UTF-8 instead, a folder with a Vietnamese name read
        // as missing, and the file with its comments was emptied every time.
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) return false;
        out.seekp(0, std::ios::end);
        if (out.tellp() == std::streampos(0)) out << kNewFile;
        if (!out) return false;
    }
    return IniFile::set_in_place(path, section, key, value);
}

// ------------------------------------------------------------------- texts

namespace alert_text {

namespace {

std::string gave_up(const std::string& reason) {
    return "GoBot ngừng tự vào lại game: " + reason + ". Cần người kiểm tra.";
}

/// Each way a crossing can fail, in a few fixed words. Never AutoLogin's own
/// advice, which names the launcher's path or another program's window.
const char* crossing_reason(AutoLogin::Why why) {
    using W = AutoLogin::Why;
    switch (why) {
        case W::NotSetUp:
        case W::NoPicture:        return "thiếu hình nhận diện màn hình đăng nhập";
        case W::NoLauncher:
        case W::LauncherMissing:
        case W::LauncherFailed:   return "không mở được VTCZone";
        case W::LauncherNoWindow: return "không thấy cửa sổ VTCZone";
        case W::SignIn:           return "VTCZone đang hiện màn hình đăng nhập";
        case W::GamePage:         return "không thấy nút Chơi game trong VTCZone";
        case W::ClientStart:      return "launcher của game không hiện nút START";
        case W::SecurityAlert:    return "launcher của game đang hiện hộp Security Alert";
        case W::GameWindow:       return "đã bấm START nhưng cửa sổ game không mở";
        case W::Connect:          return "không thấy nút Kết Nối trong game";
        case W::Channel:          return "không thấy danh sách kênh";
        case W::Character:        return "không thấy màn hình chọn nhân vật";
        case W::World:            return "đã chọn nhân vật nhưng chưa vào được game";
        case W::Covered:          return "có cửa sổ khác che chỗ cần bấm";
        default:                  return nullptr;
    }
}

} // namespace

std::string death(bool reviving) {
    return reviving ? "Nhân vật đã chết. GoBot đang hồi sinh tại chỗ."
                    : "Nhân vật đã chết. Tự hồi sinh đang tắt — nhân vật nằm tại chỗ.";
}

std::string revived() { return "Đã hồi sinh tại chỗ, bot chạy tiếp."; }

std::string revive_failed(int tries) {
    return "Hồi sinh " + std::to_string(tries) +
           " lần không được — có thể đã hết giấy hồi sinh/bùa hồi sinh.";
}

bool plain_error_code(const std::string& code) {
    if (code.size() > 2 && code[0] == '0' && (code[1] == 'x' || code[1] == 'X')) {
        const std::string hex = code.substr(2);
        return hex.size() <= 16 &&
               std::all_of(hex.begin(), hex.end(),
                           [](unsigned char c) { return std::isxdigit(c) != 0; });
    }
    return code.size() <= 10 && all_digits(code);
}

std::string disconnect(CrashWatch::Trouble what, const std::string& code, bool coming_back) {
    std::string s;
    switch (what) {
        case CrashWatch::Trouble::Hung:
            s = "Mất kết nối: game bị treo (không phản hồi).";
            break;
        case CrashWatch::Trouble::ErrorBox:
            s = plain_error_code(code) ? "Mất kết nối: game hiện bảng lỗi (mã " + code + ")."
                                       : std::string("Mất kết nối: game hiện bảng lỗi.");
            break;
        default:
            s = "Mất kết nối: game đã tắt.";
            break;
    }
    if (coming_back) s += " GoBot đang vào lại.";
    return s;
}

std::string back_in(int restart, int max_restarts) {
    return "Đã vào lại game (lần " + std::to_string(restart) + "/" + std::to_string(max_restarts) +
           "). Bot chạy tiếp.";
}

std::string gave_up_restarts(int restarts) {
    return gave_up("game văng " + std::to_string(restarts) + " lần liền");
}

std::string gave_up_not_set_up() { return gave_up("chưa cài đặt đủ các màn hình đăng nhập"); }

std::string gave_up_not_elevated() {
    return gave_up("GoBot không chạy bằng quyền Administrator");
}

std::string gave_up_crossing(int attempts, AutoLogin::Why why) {
    std::string reason = "thử " + std::to_string(std::max(1, attempts)) + " lần không vào lại được";
    if (const char* r = crossing_reason(why)) reason += std::string(" (") + r + ")";
    return gave_up(reason);
}

std::string not_in_world(int minutes) {
    return "Không thấy nhân vật trong game đã " + std::to_string(minutes) +
           " phút — có thể mất kết nối hoặc đang ở màn hình chọn nhân vật.";
}

std::string in_world_again() { return "Đã thấy lại nhân vật trong game."; }

std::string trash_stuck(bool stopped) {
    return stopped ? "Dọn rác bị dừng giữa chừng: hộp thoại thùng rác có thể vẫn mở, nên đánh "
                     "quái, bình và buff đã dừng. Cần bấm Hủy Bỏ trong game rồi bấm BẮT ĐẦU."
                   : "Món đồ có thể vẫn nằm trong thùng rác đã 2 phút: đánh quái, bình và buff "
                     "đang tạm dừng. Cần bấm Hủy Bỏ trong game.";
}

std::string chat_stuck(bool stopped) {
    return stopped ? "Ô chat trong game có thể vẫn đang mở, nên đánh quái, bình và buff đã dừng. "
                     "Cần đóng ô chat (xoá chữ rồi bấm Enter) rồi bấm BẮT ĐẦU."
                   : "Ô chat trong game có thể đang mở đã 2 phút: đánh quái, bình và buff đang "
                     "tạm dừng.";
}

std::string chat_stopped(int failures) {
    return "Tự chat đã tự dừng: " + std::to_string(failures) +
           " lần liền không gửi được câu chat.";
}

std::string quest_trouble(int times, const std::string& why) {
    return "Làm nhiệm vụ nhanh đã tự tắt: " + std::to_string(times) + " lần liền " + why + ".";
}

std::string party_joined(const std::string& who, bool with_name) {
    const std::string name = with_name ? clean_free_text(who, 32) : std::string();
    return name.empty() ? std::string("Đã vào một tổ đội (nhận lời mời).")
                        : "Đã vào tổ đội của " + name + ".";
}

std::string party_invited(const std::string& who, bool with_name) {
    const std::string name = with_name ? clean_free_text(who, 32) : std::string();
    return name.empty() ? std::string("Đã mời một người vào tổ đội.")
                        : "Đã mời " + name + " vào tổ đội.";
}

std::string test() {
    return "Tin thử từ GoBot: nếu bạn đọc được tin này trên điện thoại thì thông báo đã hoạt động.";
}

std::string offline_summary(int count, const std::string& last_hhmm) {
    return "Trong lúc mất mạng có " + std::to_string(count) +
           " thông báo không gửi được (lần cuối " + last_hhmm + ").";
}

std::string too_many(const std::string& resume_hhmm) {
    return "Quá nhiều thông báo — GoBot gộp lại và gửi tiếp lúc " + resume_hhmm + ".";
}

} // namespace alert_text

// ------------------------------------------------------------- time, format

AlertClock AlertClock::system() {
    AlertClock c;
    c.wall = [](std::uint64_t ms) {
        const std::int64_t wall_now = static_cast<std::int64_t>(std::time(nullptr));
        const std::int64_t steady_now = static_cast<std::int64_t>(now_ms());
        return wall_now + (static_cast<std::int64_t>(ms) - steady_now) / 1000;
    };
    c.local = [](std::int64_t s) {
        std::tm t{};
        const std::time_t tt = static_cast<std::time_t>(s);
        localtime_s(&t, &tt);
        return t;
    };
    return c;
}

std::string alert_hhmm(const std::tm& t) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", t.tm_hour, t.tm_min);
    return buf;
}

std::string cut_utf8(const std::string& s, std::size_t max) {
    if (max == 0) return {};
    std::vector<std::size_t> starts;
    each_char(s, [&](std::size_t at, std::size_t, bool) {
        starts.push_back(at);
        return starts.size() <= max;
    });
    if (starts.size() <= max) return s;
    return s.substr(0, starts[max - 1]) + "…";
}

std::string alert_message(const AlertBatch& b, const std::string& label, std::uint64_t now_ms,
                          const AlertClock& clock) {
    std::string head = "GoBot";
    if (!label.empty()) head += " · " + label;
    head += " · " + when_text(b, now_ms, clock) + " — ";
    return cut_utf8(head + body_of(b, clock), kAlertMaxChars);
}

std::string alert_toast_text(const AlertBatch& b, std::uint64_t now_ms, const AlertClock& clock) {
    return cut_utf8(when_text(b, now_ms, clock) + " — " + body_of(b, clock), kAlertMaxChars);
}

std::string discord_json(const std::string& content) {
    std::string out = "{\"content\":\"";
    for (const char ch : valid_utf8(content)) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20 || c == 0x7F) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04X", static_cast<unsigned>(c));
            out += esc;
        } else {
            out += ch;
        }
    }
    // An empty parse list: Discord turns no @everyone, @here, role or user in
    // the text into a mention, whatever the text says.
    out += "\",\"username\":\"GoBot\",\"allowed_mentions\":{\"parse\":[]}}";
    return out;
}

// -------------------------------------------------------------------- queue

void AlertQueue::push(const std::string& key, const std::string& text, bool critical, AlertTime at) {
    AlertBatch b;
    b.key = key;
    b.text = text;
    b.critical = critical;
    b.n = 1;
    b.first = b.last = at;
    const std::string id = b.id();
    for (AlertBatch& p : pending_) {
        if (p.kind != AlertBatch::Kind::Event || p.id() != id) continue;
        ++p.n;
        if (at.ms >= p.last.ms) p.last = at;
        if (at.ms < p.first.ms) p.first = at;
        return;
    }
    insert(std::move(b));
}

void AlertQueue::push_test(const std::string& text, AlertTime at) {
    AlertBatch b;
    b.kind = AlertBatch::Kind::Test;
    b.key = "test";
    b.text = text;
    b.n = 1;
    b.first = b.last = at;
    test_ = std::move(b);
}

void AlertQueue::insert(AlertBatch b) {
    if (pending_.size() >= limits_.max_entries) {
        // The oldest that needs nobody makes room. The summary is many
        // messages' count, and a critical one is never dropped: one of them
        // may make the queue a little longer than its limit.
        const auto droppable = [](const AlertBatch& p) {
            return !p.critical && p.kind == AlertBatch::Kind::Event;
        };
        int victim = -1;
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            if (!droppable(pending_[i])) continue;
            if (victim < 0 || pending_[i].first.ms < pending_[static_cast<std::size_t>(victim)].first.ms)
                victim = static_cast<int>(i);
        }
        const bool newcomer_first = droppable(b) &&
            (victim < 0 || b.first.ms < pending_[static_cast<std::size_t>(victim)].first.ms);
        if (newcomer_first) {
            dropped_ += b.n;
            return;
        }
        if (victim >= 0) {
            dropped_ += pending_[static_cast<std::size_t>(victim)].n;
            pending_.erase(pending_.begin() + victim);
        }
    }
    pending_.push_back(std::move(b));
}

void AlertQueue::fold_stale(std::uint64_t now) {
    if (limits_.stale_ms == 0) return;
    const auto is_stale = [&](const AlertBatch& b) {
        return b.kind == AlertBatch::Kind::Event && !b.critical && now >= b.last.ms + limits_.stale_ms;
    };
    if (std::none_of(pending_.begin(), pending_.end(), is_stale)) return;
    std::vector<AlertBatch> keep, stale;
    for (AlertBatch& b : pending_) {
        if (is_stale(b)) stale.push_back(std::move(b));
        else keep.push_back(std::move(b));
    }
    pending_ = std::move(keep);
    AlertBatch* summary = nullptr;
    for (AlertBatch& b : pending_)
        if (b.kind == AlertBatch::Kind::Summary) summary = &b;
    if (summary == nullptr) {
        AlertBatch s;
        s.kind = AlertBatch::Kind::Summary;
        s.key = "summary";
        s.first = stale.front().first;
        s.last = stale.front().last;
        pending_.push_back(std::move(s));
        summary = &pending_.back();
    }
    for (const AlertBatch& b : stale) {
        summary->n += b.n;
        if (b.first.ms < summary->first.ms) summary->first = b.first;
        if (b.last.ms > summary->last.ms) summary->last = b.last;
    }
}

void AlertQueue::prune(std::uint64_t now) {
    while (!sent_.empty() && sent_.front().at + 3'600'000ull <= now) sent_.pop_front();
    for (auto it = last_sent_.begin(); it != last_sent_.end();) {
        if (it->second + limits_.merge_ms <= now) it = last_sent_.erase(it);
        else ++it;
    }
}

std::optional<AlertBatch> AlertQueue::take(std::uint64_t now, std::uint64_t& wake) {
    wake = 0;
    capped_ = false;
    const auto later = [&](std::uint64_t t) {
        if (wake == 0 || t < wake) wake = t;
    };

    // One request at a time and a second apart, and none while Discord has
    // asked for a pause -- the test included.
    std::uint64_t ready = blocked_until_;
    if (last_request_) ready = std::max(ready, *last_request_ + limits_.spacing_ms);
    if (test_) {
        if (now < ready) {
            later(ready);
            return std::nullopt;
        }
        AlertBatch b = std::move(*test_);
        test_.reset();
        return b;
    }
    if (pending_.empty()) return std::nullopt;
    // After a failure, the next try waits its turn; the test does not.
    ready = std::max(ready, retry_at_);
    if (now < ready) {
        later(ready);
        return std::nullopt;
    }
    fold_stale(now);
    prune(now);

    // Which may go: those whose merge window is over. Critical first, then
    // the oldest.
    int best = -1;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        const AlertBatch& b = pending_[i];
        const auto it = last_sent_.find(b.id());
        if (b.kind == AlertBatch::Kind::Event && it != last_sent_.end() &&
            now < it->second + limits_.merge_ms) {
            later(it->second + limits_.merge_ms);
            continue;
        }
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const AlertBatch& was = pending_[static_cast<std::size_t>(best)];
        if ((b.critical && !was.critical) || (b.critical == was.critical && b.first.ms < was.first.ms))
            best = static_cast<int>(i);
    }
    if (best < 0) return std::nullopt;

    if (limits_.per_minute > 0) {
        int count = 0;
        std::uint64_t oldest = 0;
        for (const Sent& s : sent_) {
            if (s.at + 60'000 <= now) continue;
            if (count == 0) oldest = s.at;
            ++count;
        }
        if (count >= limits_.per_minute) {
            capped_ = true;
            later(oldest + 60'000);
            return std::nullopt;
        }
    }
    // The hour's cap holds back what only says what happened. One that needs
    // a person goes all the same -- the keeper giving up, an hour late, would
    // be no use to anybody -- and there are only ever a few of those.
    if (limits_.per_hour > 0 && !pending_[static_cast<std::size_t>(best)].critical) {
        int count = 0;
        std::uint64_t oldest = 0;
        for (const Sent& s : sent_) {
            if (!s.hourly || s.at + 3'600'000ull <= now) continue;
            if (count == 0) oldest = s.at;
            ++count;
        }
        if (count >= limits_.per_hour) {
            capped_ = true;
            const std::uint64_t resume = oldest + 3'600'000ull;
            // Said once an hour at most, so that the player knows the quiet
            // is the cap and not a dead bot.
            if (!last_notice_ || now >= *last_notice_ + 3'600'000ull) {
                AlertBatch notice;
                notice.kind = AlertBatch::Kind::CapNotice;
                notice.key = "too_many";
                notice.n = 1;
                notice.first.ms = notice.last.ms = now;
                notice.resume_ms = resume;
                return notice;
            }
            later(resume);
            return std::nullopt;
        }
    }

    AlertBatch b = std::move(pending_[static_cast<std::size_t>(best)]);
    pending_.erase(pending_.begin() + best);
    return b;
}

void AlertQueue::sent(const AlertBatch& b, std::uint64_t now) {
    switch (b.kind) {
        case AlertBatch::Kind::Event:
            last_sent_[b.id()] = now;
            sent_.push_back({ now, true });
            break;
        case AlertBatch::Kind::Summary:
            sent_.push_back({ now, true });
            break;
        case AlertBatch::Kind::CapNotice:
            // Counted for the minute -- it is a message all the same -- but
            // not for the hour, or it would hold up the very messages it is
            // about.
            sent_.push_back({ now, false });
            last_notice_ = now;
            break;
        case AlertBatch::Kind::Test:
            sent_.push_back({ now, false });
            break;
    }
}

void AlertQueue::put_back(AlertBatch b) {
    switch (b.kind) {
        case AlertBatch::Kind::Test:
            if (!test_) test_ = std::move(b);
            return;
        case AlertBatch::Kind::CapNotice:
            return;     // made again if the cap still holds
        default:
            break;
    }
    const std::string id = b.id();
    for (AlertBatch& p : pending_) {
        if (p.kind != b.kind || p.id() != id) continue;
        p.n += b.n;
        if (b.first.ms < p.first.ms) p.first = b.first;
        if (b.last.ms > p.last.ms) p.last = b.last;
        return;
    }
    insert(std::move(b));
}

void AlertQueue::clear() {
    pending_.clear();
    test_.reset();
}

void AlertQueue::drop_if(const std::function<bool(const AlertBatch&)>& drop) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), drop), pending_.end());
    if (test_ && drop(*test_)) test_.reset();
}

// -------------------------------------------------------------- the webhook

const char* const kWebhookRefused =
    "Đây không phải đường dẫn webhook Discord (phải bắt đầu bằng "
    "https://discord.com/api/webhooks/).";

void wipe_string(std::string& s) {
    // Resized to its capacity first, so that what an earlier, longer value
    // left behind the end is zeroed as well.
    s.resize(s.capacity());
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

bool WebhookUrl::parse(const std::string& text, WebhookUrl& out) {
    out.wipe();
    // Looked at in place, through views, so that no copy of the token is left
    // lying about unwiped -- only `out` gets one, and only when it is a webhook.
    std::string_view t(text);
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t' || t.front() == '\r' || t.front() == '\n'))
        t.remove_prefix(1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n'))
        t.remove_suffix(1);

    const std::string_view scheme = "https://";
    if (t.size() <= scheme.size()) return false;
    for (std::size_t i = 0; i < scheme.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(t[i])) != scheme[i]) return false;

    const std::size_t slash = t.find('/', scheme.size());
    if (slash == std::string_view::npos) return false;
    // Exactly one of Discord's own hosts: no port, no user, no other name with
    // Discord's in front of it.
    const std::string host = lower(std::string(t.substr(scheme.size(), slash - scheme.size())));
    static const char* const kHosts[] = { "discord.com", "discordapp.com", "canary.discord.com",
                                          "ptb.discord.com" };
    if (std::none_of(std::begin(kHosts), std::end(kHosts),
                     [&](const char* h) { return host == h; }))
        return false;

    std::string_view path = t.substr(slash);
    std::string_view query;
    const std::size_t q = path.find('?');
    if (q != std::string_view::npos) {
        query = path.substr(q);
        path = path.substr(0, q);
    }
    if (path.substr(0, 4) != "/api") return false;
    std::size_t p = 4;
    if (path.substr(p, 2) == "/v") {
        std::size_t d = p + 2;
        while (d < path.size() && path[d] >= '0' && path[d] <= '9') ++d;
        if (d == p + 2 || d - (p + 2) > 3) return false;
        p = d;
    }
    if (path.substr(p, 10) != "/webhooks/") return false;
    p += 10;
    const std::size_t id_end = path.find('/', p);
    if (id_end == std::string_view::npos) return false;
    const std::string_view id = path.substr(p, id_end - p);
    const auto digits = [](std::string_view s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (id.size() < 17 || id.size() > 20 || !digits(id)) return false;
    const std::string_view token = path.substr(id_end + 1);
    if (token.size() < 40 || token.size() > 100) return false;
    for (const char c : token) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    if (!query.empty()) {
        // A thread of a forum channel; nothing else.
        if (query.substr(0, 11) != "?thread_id=") return false;
        const std::string_view thread = query.substr(11);
        if (thread.size() > 20 || !digits(thread)) return false;
    }
    out.host = host;
    out.path.reserve(path.size() + query.size());
    out.path.append(path);
    out.path.append(query);
    out.id.assign(id);
    out.token.assign(token);
    return true;
}

std::string WebhookUrl::masked() const {
    return token.size() >= 4 ? "…" + token.substr(token.size() - 4) : std::string("…");
}

void WebhookUrl::wipe() {
    wipe_string(host);
    wipe_string(path);
    wipe_string(id);
    wipe_string(token);
}

std::unique_ptr<AlertChannel> make_discord_channel(const WebhookUrl& url) {
    return std::make_unique<DiscordWebhookChannel>(url);
}

// ----------------------------------------------------------- secret store

std::string SecretStore::default_path() {
    std::wstring base;
    wchar_t env[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", env, static_cast<DWORD>(std::size(env)));
    if (n > 0 && n < std::size(env)) {
        base = env;
    } else {
        PWSTR known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known)
            base = known;
        CoTaskMemFree(known);
    }
    if (base.empty()) return {};
    return narrow(base + L"\\GoBot\\alerts_webhook.bin");
}

bool SecretStore::exists() const {
    if (path_.empty()) return false;
    const DWORD a = GetFileAttributesW(widen(path_).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool SecretStore::save(const std::string& secret) const {
    if (path_.empty() || secret.empty()) return false;
    const std::wstring file = widen(path_);
    const std::size_t cut = file.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0) {
        const std::wstring dir = file.substr(0, cut);
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }

    // For this Windows user on this machine, and without any prompt: there is
    // nobody to answer one in the middle of a run.
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(secret.size());
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()));
    DATA_BLOB salt = entropy();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"GoBot Discord webhook", &salt, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    const std::wstring fresh = file + L".tmp";
    bool ok = write_bytes(fresh, out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (ok)
        ok = MoveFileExW(fresh.c_str(), file.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(fresh.c_str());
    return ok;
}

bool SecretStore::load(std::string& secret) const {
    wipe_string(secret);
    if (path_.empty()) return false;
    std::vector<BYTE> bytes;
    if (!read_bytes(widen(path_), bytes)) return false;
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(bytes.size());
    in.pbData = bytes.data();
    DATA_BLOB salt = entropy();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    secret.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool SecretStore::clear() const {
    if (path_.empty()) return true;
    if (DeleteFileW(widen(path_).c_str())) return true;
    const DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

// ------------------------------------------------------------------ service

namespace {

constexpr std::uint64_t kBeepGapMs = 10'000;
constexpr std::size_t   kStatusLines = 8;

/// The waits after a request that got no answer, or a 5xx: Discord down, or
/// the network. Then every five minutes, for as long as it lasts.
constexpr std::uint64_t kBackoffMs[] = { 10'000, 30'000, 60'000, 120'000, 300'000 };

const char* kind_name(const AlertBatch& b) {
    switch (b.kind) {
        case AlertBatch::Kind::Test:      return "test";
        case AlertBatch::Kind::Summary:   return "offline summary";
        case AlertBatch::Kind::CapNotice: return "too-many notice";
        case AlertBatch::Kind::Event:     break;
    }
    return b.key.c_str();
}

/// The tab's word for what was sent.
std::string sent_vi(const AlertBatch& b) {
    switch (b.kind) {
        case AlertBatch::Kind::Test:      return "Đã gửi thử vào Discord.";
        case AlertBatch::Kind::Summary:   return "Đã gửi tin tóm tắt lúc mất mạng.";
        case AlertBatch::Kind::CapNotice: return "Đã gửi tin báo quá nhiều thông báo.";
        case AlertBatch::Kind::Event:     break;
    }
    const AlertEventInfo* info = find_alert_event(b.key);
    std::string what = info && info->label_vi ? info->label_vi : b.key;
    if (b.key == "not_in_world") what = "Không thấy nhân vật trong game";
    return "Đã gửi: " + what + (b.n > 1 ? " (×" + std::to_string(b.n) + ")." : std::string("."));
}

} // namespace

AlertService::AlertService(std::string secret_path)
    : store_(std::move(secret_path)), factory_(make_discord_channel) {
    discord_.set_limits(discord_limits());
    local_.set_limits(local_limits());
}

AlertService::~AlertService() { stop(); }

AlertQueue::Limits AlertService::discord_limits() const {
    AlertQueue::Limits l;
    l.merge_ms = static_cast<std::uint64_t>(settings_.merge_s) * 1000;
    l.per_minute = settings_.max_per_minute;
    l.per_hour = settings_.max_per_hour;
    return l;
}

AlertQueue::Limits AlertService::local_limits() const {
    // The same merging, and nothing else: no caps and no spacing on this
    // machine -- nobody is banned for beeping -- and nothing ever waits long
    // enough to need a summary.
    AlertQueue::Limits l;
    l.merge_ms = static_cast<std::uint64_t>(settings_.merge_s) * 1000;
    l.per_minute = 0;
    l.per_hour = 0;
    l.spacing_ms = 0;
    l.stale_ms = 0;
    return l;
}

void AlertService::drop_unwanted() {
    const auto unwanted = [this](const AlertBatch& b) {
        return b.kind == AlertBatch::Kind::Event &&
               (!settings_.enabled || !settings_.event_on(b.key));
    };
    discord_.drop_if([&](const AlertBatch& b) {
        return unwanted(b) || (b.kind != AlertBatch::Kind::Test && !settings_.discord);
    });
    local_.drop_if([&](const AlertBatch& b) {
        return unwanted(b) || (b.kind != AlertBatch::Kind::Test && !settings_.sound && !settings_.toast);
    });
}

void AlertService::set_settings(const AlertSettings& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = s;
    settings_.label = AlertSettings::clean_label(s.label);
    discord_.set_limits(discord_limits());
    local_.set_limits(local_limits());
    drop_unwanted();
    ++work_;
    wake_.notify_all();
}

AlertSettings AlertService::settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void AlertService::set_notifier(LocalNotifier* n) {
    std::lock_guard<std::mutex> lock(mutex_);
    notifier_ = n;
}

void AlertService::set_clock(AlertClock c) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(c);
}

void AlertService::set_channel_factory(ChannelFactory f) {
    std::lock_guard<std::mutex> lock(mutex_);
    factory_ = std::move(f);
}

void AlertService::set_channel(std::unique_ptr<AlertChannel> c) {
    std::lock_guard<std::mutex> lock(mutex_);
    channel_ = std::move(c);
    disabled_ = false;
    disabled_code_ = 0;
    fail_streak_ = 0;
    state_ = AlertStatus::Discord::Ready;
    discord_.clear();
    discord_.retry_at(0);
    ++work_;
    wake_.notify_all();
}

void AlertService::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    stopping_ = false;
    running_ = true;
    worker_ = std::thread([this] { run(); });
}

void AlertService::stop() {
    std::shared_ptr<AlertChannel> busy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        busy = in_flight_;
        ++work_;
    }
    // Closing the request's handles is what makes a send that is waiting on
    // the network return, rather than in up to half a minute.
    if (busy) busy->cancel();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    in_flight_.reset();
    // The channel holds the webhook's address, and goes with it.
    channel_.reset();
    for (Job& job : jobs_) wipe_string(job.text);
    jobs_.clear();
    jobs_open_ = 0;
}

bool AlertService::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

bool AlertService::raise(const std::string& event, const std::string& text_vi, std::uint64_t now) {
    const AlertEventInfo* info = find_alert_event(event);
    std::lock_guard<std::mutex> lock(mutex_);
    if (info == nullptr || event == "test" || !settings_.enabled || !settings_.event_on(event))
        return false;
    const AlertTime at{ now, clock_.wall(now) };
    bool queued = false;
    if (notifier_ != nullptr && (settings_.sound || settings_.toast)) {
        local_.push(event, text_vi, info->critical, at);
        queued = true;
    }
    if (settings_.discord && channel_ && !disabled_) {
        discord_.push(event, text_vi, info->critical, at);
        queued = true;
        ++work_;
        wake_.notify_all();
    }
    return queued;
}

bool AlertService::send_test(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_test_ && now < *last_test_ + kTestGapMs) return false;
    last_test_ = now;
    const AlertTime at{ now, clock_.wall(now) };
    const std::string text = alert_text::test();
    if (notifier_ != nullptr) local_.push_test(text, at);
    if (channel_) discord_.push_test(text, at);
    ++work_;
    wake_.notify_all();
    return true;
}

void AlertService::save_webhook(std::string url) {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back({ Job::Kind::Save, std::move(url) });
    ++jobs_open_;
    ++work_;
    wake_.notify_all();
}

void AlertService::load_webhook() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back({ Job::Kind::Load, {} });
    ++jobs_open_;
    ++work_;
    wake_.notify_all();
}

void AlertService::clear_webhook() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back({ Job::Kind::Clear, {} });
    ++jobs_open_;
    ++work_;
    wake_.notify_all();
}

void AlertService::say(const std::string& vi, std::uint64_t now) {
    lines_.push_back(alert_hhmm(clock_.local(clock_.wall(now))) + " " + vi);
    while (lines_.size() > kStatusLines) lines_.pop_front();
    ++seq_;
}

std::string AlertService::title() const {
    return settings_.label.empty() ? std::string("GoBot") : "GoBot · " + settings_.label;
}

void AlertService::step_jobs() {
    std::vector<Job> jobs;
    ChannelFactory factory;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs.swap(jobs_);
        factory = factory_;
    }
    for (Job& job : jobs) {
        const std::uint64_t now = now_ms();
        if (job.kind == Job::Kind::Clear) {
            const bool gone = store_.clear();
            forget_refusal(store_.path());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (jobs_open_ > 0) --jobs_open_;
                channel_.reset();
                masked_.clear();
                unreadable_ = false;
                disabled_ = false;
                disabled_code_ = 0;
                state_ = AlertStatus::Discord::Ready;
                discord_.clear();
                say(gone ? "Đã xoá webhook." : "Không xoá được file webhook.", now);
            }
            if (gone) GB_INFO("Alerts: the Discord webhook was removed");
            else GB_WARN("Alerts: the saved webhook's file could not be deleted");
            continue;
        }

        WebhookUrl url;
        bool ok = false;
        bool unreadable = false;
        const char* trouble = nullptr;
        if (job.kind == Job::Kind::Save) {
            if (!WebhookUrl::parse(job.text, url)) {
                trouble = kWebhookRefused;
            } else {
                // Kept as Discord gave it, without the blanks a paste brings.
                std::string clean;
                clean.reserve(8 + url.host.size() + url.path.size());
                clean.append("https://").append(url.host).append(url.path);
                ok = store_.save(clean);
                wipe_string(clean);
                if (!ok) {
                    trouble = "Không lưu được webhook (không ghi được file trong thư mục "
                              "%LOCALAPPDATA%\\GoBot).";
                    GB_WARN("Alerts: the webhook could not be saved");
                }
            }
        } else {
            std::string text;
            if (!store_.exists()) {
                trouble = nullptr;      // nothing saved: nothing to say
            } else if (store_.load(text) && WebhookUrl::parse(text, url)) {
                ok = true;
            } else {
                unreadable = true;
                trouble = "Không đọc được webhook đã lưu — hãy dán lại.";
                GB_WARN("Alerts: the saved webhook could not be read (another Windows user or "
                        "machine, or a damaged file)");
            }
            wipe_string(text);
        }
        wipe_string(job.text);

        std::unique_ptr<AlertChannel> made;
        std::string masked;
        if (ok) {
            made = factory ? factory(url) : nullptr;
            masked = url.masked();
        }
        url.wipe();
        // A new webhook starts clean; the one saved before is still refused
        // if Discord refused it before GoBot was last closed.
        int refused = 0;
        if (ok && job.kind == Job::Kind::Save) forget_refusal(store_.path());
        if (ok && job.kind == Job::Kind::Load) refused = read_refusal(store_.path());

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (jobs_open_ > 0) --jobs_open_;
            if (ok) {
                channel_ = std::move(made);
                masked_ = masked;
                unreadable_ = false;
                disabled_ = refused != 0;
                disabled_code_ = refused;
                fail_streak_ = 0;
                state_ = refused != 0 ? AlertStatus::Discord::Disabled : AlertStatus::Discord::Ready;
                discord_.clear();
                discord_.retry_at(0);
                discord_.block_until(0);
                if (job.kind == Job::Kind::Save) say("Đã lưu webhook (" + masked + ").", now);
                if (refused != 0) {
                    note_ = "Webhook không còn dùng được (Discord trả lời " + std::to_string(refused) +
                            ") — hãy tạo webhook mới và dán lại ở thẻ Thông báo.";
                    ++note_seq_;
                    say(note_, now);
                }
            } else {
                if (unreadable) {
                    channel_.reset();
                    masked_.clear();
                    unreadable_ = true;
                }
                if (trouble != nullptr) say(trouble, now);
            }
        }
        if (ok)
            GB_INFO(job.kind == Job::Kind::Save ? "Alerts: a Discord webhook was saved"
                                                : "Alerts: the saved Discord webhook was read");
        if (refused != 0)
            GB_WARN("Alerts: Discord said %d to this webhook before GoBot was closed - nothing is sent "
                    "until a new webhook is saved or a test goes through",
                    refused);
    }
}

std::uint64_t AlertService::step_discord(std::uint64_t now) {
    std::shared_ptr<AlertChannel> channel;
    AlertBatch batch;
    std::string body;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!channel_ || stopping_) return 0;
        std::uint64_t wake = 0;
        std::optional<AlertBatch> got = discord_.take(now, wake);
        if (!got) return wake;
        batch = std::move(*got);
        body = discord_json(alert_message(batch, settings_.label, now, clock_));
        channel = channel_;
        in_flight_ = channel;
        discord_.requested(now);
    }

    const ChannelReply reply = channel->send(body);

    // What to log is worked out under the lock and written after it: a worker
    // raising an alert meanwhile is not to wait on the log file. So is a
    // refusal kept on disk, or forgotten.
    std::string info, warn;
    int refused = 0;
    bool accepted_again = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        in_flight_.reset();
        const int code = reply.http_status;
        const bool test = batch.kind == AlertBatch::Kind::Test;
        const std::string what = kind_name(batch);
        if (stopping_ && code == 0) {
            // Cut short by stop(): not the network's fault, and not worth a word.
            discord_.put_back(std::move(batch));
            return 0;
        }
        if (channel != channel_) {
            // The webhook was changed or removed while this was on its way.
            return now;
        }

        if (code >= 200 && code < 300) {
            discord_.sent(batch, now);
            fail_streak_ = 0;
            discord_.retry_at(0);
            if (test && disabled_) {
                disabled_ = false;
                disabled_code_ = 0;
                accepted_again = true;
            }
            state_ = AlertStatus::Discord::Sent;
            sent_wall_ = clock_.wall(now);
            info = format("Alerts: sent %s (%d)", what.c_str(), code);
            say(sent_vi(batch), now);
        } else if (code == 429) {
            // However long Discord asks, a minute at most -- and the same
            // message again then. Not a failure: Discord is only saying "slower".
            std::uint64_t wait = reply.retry_after_ms > 0 ? reply.retry_after_ms : 1000;
            wait = std::min<std::uint64_t>(wait, 60'000);
            discord_.block_until(now + wait);
            discord_.put_back(std::move(batch));
            state_ = AlertStatus::Discord::Waiting;
            resume_wall_ = clock_.wall(now + wait);
            info = format("Alerts: Discord said 429, retry in %.1f s", static_cast<double>(wait) / 1000.0);
            say("Discord bảo gửi chậm lại — gửi lại sau " + seconds_vi(wait) + " giây.", now);
        } else if (code == 401 || code == 403 || code == 404) {
            // The webhook is gone or was never right. Posting to it again and
            // again is what gets an address banned by Discord, so nothing more
            // goes until the player gives a new one or a Gửi thử goes through.
            disabled_ = true;
            disabled_code_ = code;
            refused = code;
            discord_.clear();
            state_ = AlertStatus::Discord::Disabled;
            warn = format("Alerts: Discord said %d - sending stopped until a new webhook is saved", code);
            note_ = "Webhook không còn dùng được (Discord trả lời " + std::to_string(code) +
                    ") — hãy tạo webhook mới và dán lại ở thẻ Thông báo.";
            ++note_seq_;
            say(note_, now);
        } else if (code == 0 || code >= 500) {
            if (test) {
                // A Gửi thử is the player's, for now; it is not tried again
                // behind their back.
                warn = code == 0 ? format("Alerts: test not sent - no answer from Discord (error %lu)",
                                          reply.net_error)
                                 : format("Alerts: test not sent - Discord said %d", code);
                say("Gửi thử không được: không có mạng hoặc Discord đang lỗi.", now);
            } else {
                const std::size_t step = std::min<std::size_t>(static_cast<std::size_t>(fail_streak_),
                                                               std::size(kBackoffMs) - 1);
                const std::uint64_t wait = kBackoffMs[step];
                ++fail_streak_;
                discord_.put_back(std::move(batch));
                discord_.retry_at(now + wait);
                state_ = AlertStatus::Discord::Retrying;
                resume_wall_ = clock_.wall(now + wait);
                warn = code == 0 ? format("Alerts: no answer from Discord (error %lu) - trying again in %llu s",
                                          reply.net_error, static_cast<unsigned long long>(wait / 1000))
                                 : format("Alerts: Discord said %d - trying again in %llu s", code,
                                          static_cast<unsigned long long>(wait / 1000));
                say("Chưa gửi được: không có mạng hoặc Discord đang lỗi — GoBot sẽ thử lại lúc " +
                        alert_hhmm(clock_.local(resume_wall_)) + ".", now);
            }
        } else {
            // 400, 413 and the like: this message will not go however often it
            // is sent. The next one may.
            warn = format("Alerts: Discord said %d - that message was dropped", code);
            note_ = "Discord không nhận một tin thông báo (mã " + std::to_string(code) + ") — đã bỏ tin đó.";
            ++note_seq_;
            say(note_, now);
        }
    }
    // Kept for the next GoBot too: a webhook that is gone stays refused until
    // a new one is saved or a Gửi thử goes through.
    if (refused != 0) keep_refusal(store_.path(), refused);
    if (accepted_again) forget_refusal(store_.path());
    if (!info.empty()) GB_INFO("%s", info.c_str());
    if (!warn.empty()) GB_WARN("%s", warn.c_str());
    return now;
}

std::uint64_t AlertService::step_local(std::uint64_t now) {
    LocalNotifier* notifier = nullptr;
    std::vector<std::string> toasts;
    bool beep = false;
    std::string heading;
    std::uint64_t wake = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notifier = notifier_;
        if (notifier == nullptr) return 0;
        for (;;) {
            std::uint64_t w = 0;
            std::optional<AlertBatch> b = local_.take(now, w);
            if (!b) {
                wake = w;
                break;
            }
            local_.sent(*b, now);
            if (settings_.toast) toasts.push_back(alert_toast_text(*b, now, clock_));
            if (settings_.sound && (!last_beep_ || now >= *last_beep_ + kBeepGapMs)) {
                beep = true;
                last_beep_ = now;
            }
        }
        heading = title();
    }
    // Outside the lock: the shell can take a moment, and a worker raising an
    // alert meanwhile is not to wait for it.
    if (beep) notifier->beep();
    for (const std::string& t : toasts) notifier->toast(heading, t);
    return wake;
}

AlertStatus AlertService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AlertStatus s;
    s.enabled = settings_.enabled;
    s.running = running_;
    s.webhook = static_cast<bool>(channel_);
    s.masked = channel_ ? masked_ : std::string();
    s.unreadable = unreadable_;
    s.busy = jobs_open_ > 0;
    s.discord = !channel_ ? AlertStatus::Discord::None
              : disabled_ ? AlertStatus::Discord::Disabled
                          : state_;
    s.code = disabled_code_;
    s.sent_wall = sent_wall_;
    s.resume_wall = resume_wall_;
    s.capped = discord_.capped();
    s.queued = discord_.size();
    s.dropped = discord_.dropped();
    s.test_ready_ms = last_test_ ? *last_test_ + kTestGapMs : 0;
    s.lines.assign(lines_.begin(), lines_.end());
    s.seq = seq_;
    s.note = note_;
    s.note_seq = note_seq_;
    return s;
}

void AlertService::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        const std::uint64_t seen = work_;
        lock.unlock();
        step_jobs();
        const std::uint64_t wake = step_discord(now_ms());
        lock.lock();
        if (stopping_) break;
        if (work_ != seen) continue;
        const auto woken = [&] { return stopping_ || work_ != seen; };
        if (wake == 0) {
            wake_.wait(lock, woken);
        } else {
            const std::uint64_t t = now_ms();
            if (wake > t) wake_.wait_for(lock, std::chrono::milliseconds(wake - t), woken);
        }
    }
}

// -------------------------------------------------------------------- watch

bool AlertWatch::due(bool on, std::uint64_t now) {
    if (!on) {
        active_ = false;
        told_ = false;
        return false;
    }
    if (!active_) {
        active_ = true;
        since_ = now;
    }
    if (told_ || now < since_ + ms_) return false;
    told_ = true;
    return true;
}

AwayWatch::Say AwayWatch::step(bool running, bool reading, bool crossing, float hp_pct,
                               std::uint64_t now) {
    if (!running) seen_ = false;
    else if (reading && hp_pct >= 0.0f) seen_ = true;
    const bool watching = running && reading && !crossing && seen_;
    const bool told = watch_.told();
    if (watch_.due(watching && hp_pct < 0.0f, now)) return Say::Away;
    // Back only to somebody who was told it was away: a crossing or a stop in
    // between says nothing more.
    if (told && watching && hp_pct >= 0.0f) return Say::Back;
    return Say::Nothing;
}

bool DeathByHp::step(bool watching, int hp, int hp_max, std::uint64_t now) {
    if (!watching) {
        since_.reset();
        return false;
    }
    if (hp > 0) {
        since_.reset();
        told_ = false;
        return false;
    }
    if (hp != 0 || hp_max <= 0) return false;
    if (!since_) since_ = now;
    if (told_ || now < *since_ + ms_) return false;
    told_ = true;
    return true;
}

} // namespace gobot
