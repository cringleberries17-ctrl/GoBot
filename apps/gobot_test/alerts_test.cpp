// gobot_test -- the alerts.
//
// What the messages say, the merging and the caps on a pretend clock, what
// each of Discord's replies does, the webhook kept with DPAPI, and what must
// never be sent or logged. Nothing here goes near the network: every message
// goes to a MockChannel, and the one real channel made is never asked to send.

#include "gobot/alerts.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/common.hpp"
#include "gobot/crash_watch.hpp"
#include "gobot/logger.hpp"
#include "gobot/reviver.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::size_t code_points(const std::string& s) {
    std::size_t n = 0;
    for (const char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

/// A file in the temp folder, in UTF-8, for the tests that need a real one.
std::string temp_path(const char* name) {
    wchar_t dir[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    // In this run's own folder, which main.cpp makes and removes: see temp_file there.
    std::wstring w;
    if (n > 0 && n <= MAX_PATH)
        w = std::wstring(dir) + L"gobot_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    w += std::wstring(name, name + std::strlen(name));
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string read_all(const std::string& path) {
    std::ifstream in(wide(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, const std::string& bytes) {
    std::ofstream out(wide(path), std::ios::binary | std::ios::trunc);
    out << bytes;
}

void remove_file(const std::string& path) { DeleteFileW(wide(path).c_str()); }

// ---- a pretend clock ---------------------------------------------------------

/// 2026-10-15 03:10:00, read as UTC whatever this machine's time zone, so
/// that every HH:MM below is known in advance. Steady time 0 is that moment.
std::int64_t base_wall() {
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 9;
    t.tm_mday = 15;
    t.tm_hour = 3;
    t.tm_min = 10;
    return static_cast<std::int64_t>(_mkgmtime(&t));
}

AlertClock fake_clock() {
    const std::int64_t base = base_wall();
    AlertClock c;
    c.wall = [base](std::uint64_t ms) { return base + static_cast<std::int64_t>(ms / 1000); };
    c.local = [](std::int64_t s) {
        std::tm t{};
        const std::time_t tt = static_cast<std::time_t>(s);
        gmtime_s(&t, &tt);
        return t;
    };
    return c;
}

// ---- JSON, checked by hand -----------------------------------------------------

/// Enough of a JSON reader to say whether a body is valid JSON -- strictly:
/// no raw control characters in a string, every escape known -- and to read
/// back the top level's "content" and whether "allowed_mentions" has an empty
/// "parse" list.
class JsonCheck {
public:
    explicit JsonCheck(const std::string& text) : s_(text) {}

    bool valid() {
        i_ = 0;
        ok_ = true;
        space();
        value(0, "");
        space();
        return ok_ && i_ == s_.size();
    }

    std::string content;
    std::string username;
    bool        no_mentions = false;

private:
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    void space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }
    bool eat(char c) {
        if (peek() != c) return ok_ = false;
        ++i_;
        return true;
    }
    void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    unsigned hex4() {
        if (i_ + 4 > s_.size()) { ok_ = false; return 0; }
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else ok_ = false;
        }
        return v;
    }
    std::string string() {
        std::string out;
        if (!eat('"')) return out;
        while (ok_) {
            if (i_ >= s_.size()) { ok_ = false; break; }
            const unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') break;
            if (c < 0x20) { ok_ = false; break; }
            if (c != '\\') { out += static_cast<char>(c); continue; }
            const char e = i_ < s_.size() ? s_[i_++] : '\0';
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (!eat('\\') || !eat('u')) break;
                        const unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: ok_ = false; break;
            }
        }
        return out;
    }
    void number() {
        const std::size_t start = i_;
        if (peek() == '-') ++i_;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' ||
                                  s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
            ++i_;
        if (i_ == start) ok_ = false;
    }
    void value(int depth, const std::string& key) {
        if (depth > 16) { ok_ = false; return; }
        space();
        const char c = peek();
        if (c == '{') {
            ++i_;
            space();
            if (peek() == '}') { ++i_; return; }
            while (ok_) {
                space();
                const std::string k = string();
                space();
                eat(':');
                space();
                const std::size_t at = i_;
                value(depth + 1, k);
                if (depth == 0 && k == "content") content = decoded_;
                if (depth == 0 && k == "username") username = decoded_;
                if (depth == 1 && key == "allowed_mentions" && k == "parse")
                    no_mentions = s_.compare(at, 2, "[]") == 0;
                space();
                if (peek() == ',') { ++i_; continue; }
                eat('}');
                break;
            }
        } else if (c == '[') {
            ++i_;
            space();
            if (peek() == ']') { ++i_; return; }
            while (ok_) {
                value(depth + 1, "");
                space();
                if (peek() == ',') { ++i_; continue; }
                eat(']');
                break;
            }
        } else if (c == '"') {
            decoded_ = string();
        } else if (s_.compare(i_, 4, "true") == 0 || s_.compare(i_, 4, "null") == 0) {
            i_ += 4;
        } else if (s_.compare(i_, 5, "false") == 0) {
            i_ += 5;
        } else {
            number();
        }
    }

    const std::string  s_;          // a copy: it is handed temporaries
    std::size_t        i_ = 0;
    bool               ok_ = true;
    std::string        decoded_;
};

std::string content_of(const std::string& body) {
    JsonCheck j(body);
    return j.valid() ? j.content : std::string("<not JSON>");
}

/// "(×N," in a message, or 1.
int times_in(const std::string& content) {
    const std::size_t at = content.find("(×");
    if (at == std::string::npos) return 1;
    return std::atoi(content.c_str() + at + std::strlen("(×"));
}

// ---- a pretend Discord, and a pretend Windows ------------------------------------

/// What the pretend channel was sent, and what it says back.
struct MockState {
    std::vector<std::string>   bodies;
    std::vector<std::uint64_t> times;       ///< the pretend time of each request
    std::deque<ChannelReply>   replies;     ///< said in turn, then `otherwise`
    ChannelReply               otherwise{ 204, 0, 0 };
    std::function<ChannelReply(std::uint64_t)> by_time;    ///< instead, when set
    const std::uint64_t*       now = nullptr;
    int                        cancels = 0;
};

class MockChannel final : public AlertChannel {
public:
    explicit MockChannel(std::shared_ptr<MockState> s) : s_(std::move(s)) {}
    ChannelReply send(const std::string& json) override {
        const std::uint64_t t = s_->now ? *s_->now : 0;
        s_->bodies.push_back(json);
        s_->times.push_back(t);
        if (s_->by_time) return s_->by_time(t);
        if (!s_->replies.empty()) {
            const ChannelReply r = s_->replies.front();
            s_->replies.pop_front();
            return r;
        }
        return s_->otherwise;
    }
    void cancel() override { ++s_->cancels; }

private:
    std::shared_ptr<MockState> s_;
};

ChannelReply reply(int status, std::uint64_t retry_after_ms = 0, unsigned long net_error = 0) {
    ChannelReply r;
    r.http_status = status;
    r.retry_after_ms = retry_after_ms;
    r.net_error = net_error;
    return r;
}

struct CountingNotifier final : LocalNotifier {
    int beeps = 0;
    std::vector<std::pair<std::string, std::string>> toasts;
    void beep() override { ++beeps; }
    void toast(const std::string& title, const std::string& text) override {
        toasts.emplace_back(title, text);
    }
};

AlertSettings switched_on() {
    AlertSettings s;
    s.enabled = true;
    s.events["party_change"] = true;    // every event, for the tests that want them all
    return s;
}

/// A service stepped by hand on the pretend clock, with a pretend Discord.
struct Rig {
    explicit Rig(const AlertSettings& s, bool webhook = true) : svc("") {
        mock->now = &now;
        svc.set_clock(fake_clock());
        svc.set_notifier(&notifier);
        svc.set_settings(s);
        if (webhook) svc.set_channel(std::make_unique<MockChannel>(mock));
    }
    void at(std::uint64_t t) {
        now = t;
        svc.step_discord(t);
        svc.step_local(t);
    }
    void run(std::uint64_t from, std::uint64_t to, std::uint64_t step) {
        for (std::uint64_t t = from; t <= to; t += step) at(t);
    }
    bool raise(const char* key, const std::string& text, std::uint64_t t) {
        now = t;
        return svc.raise(key, text, t);
    }
    std::vector<std::string> contents() const {
        std::vector<std::string> out;
        for (const std::string& b : mock->bodies) out.push_back(content_of(b));
        return out;
    }

    std::shared_ptr<MockState> mock = std::make_shared<MockState>();
    CountingNotifier           notifier;
    AlertService               svc;
    std::uint64_t              now = 0;
};

/// One sentence per event, for raising them all.
struct Sample {
    const char* key;
    std::string text;
};
std::vector<Sample> every_event() {
    return {
        { "death",          alert_text::death(true) },
        { "revived",        alert_text::revived() },
        { "revive_failed",  Reviver::out_of_tries_vi() },
        { "disconnect",     alert_text::disconnect(CrashWatch::Trouble::Gone, "", true) },
        { "keeper_gave_up", alert_text::gave_up_restarts(5) },
        { "back_in",        alert_text::back_in(1, 5) },
        { "trash_stuck",    alert_text::trash_stuck(false) },
        { "not_in_world",   alert_text::not_in_world(3) },
        { "chat_stuck",     alert_text::chat_stuck(false) },
        { "chat_stopped",   alert_text::chat_stopped(3) },
        { "quest_trouble",  alert_text::quest_trouble(3, "không mở được bảng nhiệm vụ") },
        { "party_change",   alert_text::party_joined("", false) },
    };
}

const char* const kToken = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789_-AbCdEfGhIjKlMnOpQrStUvWxYz01";
const char* const kId = "123456789012345678";

std::string webhook(const char* host = "discord.com", const char* token = kToken) {
    return std::string("https://") + host + "/api/webhooks/" + kId + "/" + token;
}

// ------------------------------------------------------------------- texts

void test_texts() {
    section("Alerts: the sentences (AL-A1)");

    check(alert_text::death(true) == "Nhân vật đã chết. GoBot đang hồi sinh tại chỗ.", "death, reviving");
    check(alert_text::death(false) == "Nhân vật đã chết. Tự hồi sinh đang tắt — nhân vật nằm tại chỗ.",
          "death, Tự hồi sinh off");
    check(alert_text::revived() == "Đã hồi sinh tại chỗ, bot chạy tiếp.", "revived");
    check(Reviver::out_of_tries_vi().rfind("Đã dừng bot: hồi sinh 3 lần không được", 0) == 0,
          "revive_failed: the Reviver's own words, which say the bot has stopped");
    check(alert_text::disconnect(CrashWatch::Trouble::Gone, "", false) == "Mất kết nối: game đã tắt.",
          "disconnect: gone");
    check(alert_text::disconnect(CrashWatch::Trouble::Gone, "", true) ==
              "Mất kết nối: game đã tắt. GoBot đang vào lại.",
          "disconnect: gone, coming back");
    check(alert_text::disconnect(CrashWatch::Trouble::Hung, "", false) ==
              "Mất kết nối: game bị treo (không phản hồi).",
          "disconnect: hung");
    check(alert_text::disconnect(CrashWatch::Trouble::ErrorBox, "122", true) ==
              "Mất kết nối: game hiện bảng lỗi (mã 122). GoBot đang vào lại.",
          "disconnect: an error box with its code");
    check(alert_text::disconnect(CrashWatch::Trouble::ErrorBox, "0x8007000E", false) ==
              "Mất kết nối: game hiện bảng lỗi (mã 0x8007000E).",
          "disconnect: a hex code");
    check(alert_text::disconnect(CrashWatch::Trouble::ErrorBox, "", false) ==
              "Mất kết nối: game hiện bảng lỗi.",
          "disconnect: an error box with no code");
    check(alert_text::back_in(1, 5) == "Đã vào lại game (lần 1/5). Bot chạy tiếp.", "back_in");
    check(alert_text::gave_up_restarts(5) ==
              "GoBot ngừng tự vào lại game: game văng 5 lần liền. Cần người kiểm tra.",
          "keeper_gave_up: max restarts");
    check(alert_text::gave_up_not_set_up() ==
              "GoBot ngừng tự vào lại game: chưa cài đặt đủ các màn hình đăng nhập. Cần người kiểm tra.",
          "keeper_gave_up: not set up");
    check(alert_text::gave_up_not_elevated() ==
              "GoBot ngừng tự vào lại game: GoBot không chạy bằng quyền Administrator. Cần người kiểm tra.",
          "keeper_gave_up: not elevated");
    check(alert_text::gave_up_crossing(3, AutoLogin::Why::SignIn) ==
              "GoBot ngừng tự vào lại game: thử 3 lần không vào lại được (VTCZone đang hiện màn hình "
              "đăng nhập). Cần người kiểm tra.",
          "keeper_gave_up: the crossing failed, with a fixed reason");
    check(alert_text::gave_up_crossing(3, AutoLogin::Why::Cancelled) ==
              "GoBot ngừng tự vào lại game: thử 3 lần không vào lại được. Cần người kiểm tra.",
          "keeper_gave_up: a reason with no words of its own adds none");
    check(alert_text::not_in_world(3) ==
              "Không thấy nhân vật trong game đã 3 phút — có thể mất kết nối hoặc đang ở màn hình chọn "
              "nhân vật.",
          "not_in_world");
    check(alert_text::in_world_again() == "Đã thấy lại nhân vật trong game.", "not_in_world: back");
    check(alert_text::trash_stuck(true) ==
              "Dọn rác bị dừng giữa chừng: hộp thoại thùng rác có thể vẫn mở, nên đánh quái, bình và buff "
              "đã dừng. Cần bấm Hủy Bỏ trong game rồi bấm BẮT ĐẦU.",
          "trash_stuck: at a stop");
    check(alert_text::trash_stuck(false) ==
              "Món đồ có thể vẫn nằm trong thùng rác đã 2 phút: đánh quái, bình và buff đang tạm dừng. "
              "Cần bấm Hủy Bỏ trong game.",
          "trash_stuck: two minutes held");
    check(alert_text::chat_stuck(true) ==
              "Ô chat trong game có thể vẫn đang mở, nên đánh quái, bình và buff đã dừng. Cần đóng ô chat "
              "(xoá chữ rồi bấm Enter) rồi bấm BẮT ĐẦU.",
          "chat_stuck: handed over");
    check(alert_text::chat_stuck(false) ==
              "Ô chat trong game có thể đang mở đã 2 phút: đánh quái, bình và buff đang tạm dừng.",
          "chat_stuck: two minutes watched");
    check(alert_text::chat_stopped(3) == "Tự chat đã tự dừng: 3 lần liền không gửi được câu chat.",
          "chat_stopped");
    check(alert_text::quest_trouble(3, "không mở được bảng nhiệm vụ") ==
              "Làm nhiệm vụ nhanh đã tự tắt: 3 lần liền không mở được bảng nhiệm vụ.",
          "quest_trouble");
    check(alert_text::party_joined("Abc", false) == "Đã vào một tổ đội (nhận lời mời).", "party: joined, no name");
    check(alert_text::party_invited("Abc", false) == "Đã mời một người vào tổ đội.", "party: invited, no name");
    check(alert_text::party_joined("Abc", true) == "Đã vào tổ đội của Abc.", "party: joined, named");
    check(alert_text::party_invited("Abc", true) == "Đã mời Abc vào tổ đội.", "party: invited, named");
    check(alert_text::test() ==
              "Tin thử từ GoBot: nếu bạn đọc được tin này trên điện thoại thì thông báo đã hoạt động.",
          "test");

    // The header, the label, the merge and the length.
    const AlertClock clock = fake_clock();
    AlertBatch b;
    b.key = "death";
    b.text = alert_text::death(true);
    b.n = 1;
    b.first = b.last = { 0, clock.wall(0) };
    check(alert_message(b, "", 0, clock) == "GoBot · 03:10 — Nhân vật đã chết. GoBot đang hồi sinh tại chỗ.",
          "header: 'GoBot · HH:MM — '");
    check(alert_message(b, "Máy 1", 0, clock) ==
              "GoBot · Máy 1 · 03:10 — Nhân vật đã chết. GoBot đang hồi sinh tại chỗ.",
          "header with a label: 'GoBot · Máy 1 · HH:MM — '");
    b.n = 3;
    b.last = { 70'000, clock.wall(70'000) };
    check(ends_with(alert_message(b, "", 70'000, clock), " (×3, 03:10–03:11)"),
          "a merged message ends ' (×3, 03:10–03:11)'");
    check(contains(alert_message(b, "", 2 * 3600'000ull, clock), "GoBot · 03:10 15/10 — "),
          "more than an hour late: the date after the time");
    check(!contains(alert_message(b, "", 3500'000ull, clock), "15/10"), "less than an hour late: no date");

    AlertBatch big = b;
    big.n = 1;
    big.text = std::string(5000, 'a');
    const std::string cut = alert_message(big, "", 0, clock);
    check(code_points(cut) <= kAlertMaxChars && ends_with(cut, "…"), "no message is longer than 1800 characters");
    std::string vi;
    for (int i = 0; i < 3000; ++i) vi += "ệ";
    big.text = vi;
    const std::string cut_vi = alert_message(big, "", 0, clock);
    check(code_points(cut_vi) == kAlertMaxChars && ends_with(cut_vi, "…") && JsonCheck(discord_json(cut_vi)).valid(),
          "a long Vietnamese message is cut between letters, never inside one");

    // What counts as a code CrashWatch read.
    check(alert_text::plain_error_code("122") && alert_text::plain_error_code("0x8007000E"),
          "error codes of code_in's shapes are kept");
    check(!alert_text::plain_error_code("C:/Users/Someone") && !alert_text::plain_error_code("12a") &&
              !alert_text::plain_error_code("") && !alert_text::plain_error_code("0x"),
          "anything else is not a code");
}

// -------------------------------------------------------------------- JSON

void test_json() {
    section("Alerts: Discord's body (AL-A2)");
    const std::string text = "Có \"ngoặc\" và \\ gạch, dòng\nmới, tab\tvà \xF0\x9F\x98\x80 @everyone";
    const std::string body = discord_json(text);
    JsonCheck j(body);
    check(j.valid(), "the body is valid JSON");
    check(j.content == text, "the content reads back exactly, quote, backslash, newline and tab included");
    check(body.find('\n') == std::string::npos && body.find('\t') == std::string::npos,
          "no raw newline or tab in the body");
    check(contains(body, "\\u000A") && contains(body, "\\u0009") && contains(body, "\\\"") && contains(body, "\\\\"),
          "control characters as \\u00XX, quotes and backslashes escaped");
    check(contains(body, "ngoặc") && contains(body, "\xF0\x9F\x98\x80"), "Vietnamese and the emoji go as UTF-8");
    check(j.username == "GoBot", "posted as GoBot");
    check(j.no_mentions && contains(body, "\"allowed_mentions\":{\"parse\":[]}"),
          "allowed_mentions with an empty parse list: no @everyone");

    const std::string broken = std::string("a\xC3") + "b\xFF" + "c";
    JsonCheck jb(discord_json(broken));
    check(jb.valid() && jb.content == "a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c",
          "bytes that are not UTF-8 become U+FFFD and the body stays valid");
}

// ----------------------------------------------------------------- merging

void test_merging() {
    section("Alerts: merging on a pretend clock (AL-A3)");
    Rig r(switched_on());
    const std::string text = alert_text::death(true);
    // Ten deaths in 30 s, from 03:10:45.
    for (int i = 0; i < 10; ++i) {
        const std::uint64_t t = 45'000 + static_cast<std::uint64_t>(i) * 3000;
        r.raise("death", text, t);
        r.at(t);
    }
    r.run(72'250, 400'000, 250);
    const auto got = r.contents();
    check(got.size() == 2, "ten deaths in 30 s: two messages (" + std::to_string(got.size()) + ")");
    if (got.size() == 2) {
        check(got[0] == "GoBot · 03:10 — " + text, "the first at once, on its own");
        check(ends_with(got[1], " (×9, 03:10–03:11)"), "then the other nine as one: " + got[1]);
        check(r.mock->times[1] >= 45'000 + 60'000, "and only once the merge window was over");
    }
    check(r.notifier.toasts.size() == 2, "the Windows notification is merged the same way");
}

// -------------------------------------------------------------------- caps

void test_caps() {
    section("Alerts: caps on a pretend clock (AL-A4)");
    {
        Rig r(switched_on());
        const auto events = every_event();
        for (std::size_t i = 0; i < events.size(); ++i)
            r.raise(events[i].key, events[i].text, i * 800);
        r.run(10'000, 400'000, 250);
        const auto got = r.contents();
        check(got.size() == events.size(), "12 events, none lost: " + std::to_string(got.size()) + " sent");
        int first_minute = 0;
        for (const std::uint64_t t : r.mock->times) first_minute += t < 10'000 + 60'000 ? 1 : 0;
        check(first_minute == 5, "5 sent in the first minute (" + std::to_string(first_minute) + ")");
        bool cap_kept = true;
        for (std::size_t i = 0; i < r.mock->times.size(); ++i) {
            int in_window = 0;
            for (const std::uint64_t t : r.mock->times)
                in_window += t >= r.mock->times[i] && t < r.mock->times[i] + 60'000 ? 1 : 0;
            cap_kept = cap_kept && in_window <= 5;
        }
        check(cap_kept, "no 60 s window has more than 5");
        int critical_first = 0;
        for (std::size_t i = 0; i < 5 && i < got.size(); ++i)
            for (const char* key : { "revive_failed", "keeper_gave_up", "trash_stuck", "chat_stuck", "quest_trouble" })
                for (const Sample& s : events)
                    if (std::strcmp(s.key, key) == 0 && contains(got[i], s.text)) ++critical_first;
        check(critical_first == 5, "the five critical ones are the first five");
    }
    {
        AlertSettings s = switched_on();
        s.max_per_hour = 30;
        Rig r(s);
        const std::string text = alert_text::death(true);
        int raised = 0;
        for (std::uint64_t t = 0; t <= 3'599'000; t += 1000) {
            if (t % 90'000 == 0 && raised < 40) {
                r.raise("death", text, t);
                ++raised;
            }
            r.at(t);
        }
        int normal = 0, notices = 0;
        for (const std::string& c : r.contents()) {
            if (contains(c, "Quá nhiều thông báo")) ++notices;
            else ++normal;
        }
        check(raised == 40, "40 events over the hour");
        check(normal <= 30 && notices == 1,
              "at most 30 messages and one cap notice in that hour (" + std::to_string(normal) + " + " +
                  std::to_string(notices) + ")");
        bool says_when = false;
        for (const std::string& c : r.contents())
            says_when = says_when || c == "GoBot · 03:55 — Quá nhiều thông báo — GoBot gộp lại và gửi tiếp lúc 04:10.";
        check(says_when, "the notice says when sending goes on");
        r.run(3'600'000, 7'300'000, 1000);
        int deaths = 0;
        for (const std::string& c : r.contents())
            if (contains(c, text)) deaths += times_in(c);
        check(deaths == 40, "every one of the 40 is in a message in the end (" + std::to_string(deaths) + ")");
    }
    {
        // The hour's cap reached: what only says what happened waits, what
        // needs a person does not.
        AlertSettings s = switched_on();
        s.max_per_hour = 3;
        Rig r(s);
        const std::string text = alert_text::death(true);
        for (std::uint64_t t : { 0ull, 61'000ull, 122'000ull }) {
            r.raise("death", text, t);
            r.at(t);
        }
        r.raise("disconnect", alert_text::disconnect(CrashWatch::Trouble::Gone, "", true), 200'000);
        r.run(200'000, 250'000, 1000);
        r.raise("keeper_gave_up", alert_text::gave_up_restarts(5), 300'000);
        r.run(300'000, 310'000, 1000);
        const auto got = r.contents();
        check(got.size() == 5, "three, the cap notice, and the critical one (" + std::to_string(got.size()) + ")");
        if (got.size() == 5) {
            check(contains(got[3], "Quá nhiều thông báo"), "the disconnect waits behind the notice");
            check(contains(got[4], alert_text::gave_up_restarts(5)) && r.mock->times[4] == 300'000,
                  "keeper_gave_up goes at once, cap or no cap");
        }
    }
}

// ----------------------------------------------------------------- replies

void test_replies() {
    section("Alerts: Discord's replies (AL-A5)");
    const std::string death = alert_text::death(true);
    {
        Rig r(switched_on());
        r.mock->replies = { reply(429, 2500) };
        r.raise("death", death, 0);
        r.run(0, 10'000, 100);
        check(r.mock->bodies.size() == 2, "429: one more request, and no more");
        if (r.mock->bodies.size() == 2) {
            check(r.mock->times[1] >= 2500, "429 with retry_after 2.5: nothing before 2.5 s");
            check(r.mock->bodies[0] == r.mock->bodies[1], "and then the same message again");
        }
        check(r.svc.status().discord == AlertStatus::Discord::Sent, "429 is not a failure: it went");
    }
    {
        Rig r(switched_on());
        r.mock->replies = { reply(404) };
        r.raise("death", death, 0);
        r.at(0);
        const AlertStatus st = r.svc.status();
        check(st.discord == AlertStatus::Discord::Disabled && st.code == 404, "404: sending stops");
        const std::size_t toasts = r.notifier.toasts.size();
        const char* keys[] = { "revived", "disconnect", "back_in", "not_in_world", "chat_stopped" };
        std::uint64_t t = 1000;
        for (const char* key : keys) {
            r.raise(key, std::string("x ") + key, t);
            r.run(t, t + 70'000, 1000);
            t += 71'000;
        }
        check(r.mock->bodies.size() == 1, "the next 5 events make no request");
        check(r.notifier.toasts.size() - toasts == 5, "while the notifications here go on: 5");
        check(r.svc.send_test(t), "Gửi thử is still allowed");
        r.at(t);
        check(r.mock->bodies.size() == 2 && r.svc.status().discord == AlertStatus::Discord::Sent,
              "and a Gửi thử that goes through starts sending again");
    }
    {
        Rig r(switched_on());
        r.mock->replies = { reply(500), reply(500) };
        r.raise("death", death, 0);
        r.run(0, 120'000, 100);
        const auto& times = r.mock->times;
        check(times.size() == 3, "500, 500, 204: three requests");
        if (times.size() == 3)
            check(times[0] == 0 && times[1] == 10'000 && times[2] == 40'000,
                  "tried again at +10 s, then +30 s");
        check(r.svc.status().discord == AlertStatus::Discord::Sent, "and delivered once");
    }
    {
        Rig r(switched_on());
        r.mock->by_time = [](std::uint64_t t) { return t < 600'000 ? reply(0, 0, 12007) : reply(204); };
        r.raise("death", death, 0);
        r.at(0);
        const std::string gone = alert_text::disconnect(CrashWatch::Trouble::Gone, "", true);
        r.run(500, 59'500, 500);
        r.raise("disconnect", gone, 60'000);
        r.run(60'000, 119'500, 500);
        r.raise("death", death, 120'000);
        r.run(120'000, 1'200'000, 500);
        std::vector<std::string> delivered;
        for (std::size_t i = 0; i < r.mock->times.size(); ++i)
            if (r.mock->times[i] >= 600'000) delivered.push_back(content_of(r.mock->bodies[i]));
        check(delivered.size() == 2, "no network for 10 minutes: then 2 messages (" +
                                         std::to_string(delivered.size()) + ")");
        bool deaths = false, lost = false;
        for (const std::string& c : delivered) {
            deaths = deaths || c == "GoBot · 03:10 — " + death + " (×2, 03:10–03:12)";
            lost = lost || c == "GoBot · 03:11 — " + gone;
        }
        check(deaths, "the deaths merged, with their own time");
        check(lost, "the disconnect with its own time");
    }
    {
        Rig r(switched_on());
        r.mock->by_time = [](std::uint64_t t) {
            return t < 9'000'000 ? reply(0, 0, 12029) : reply(204);
        };
        r.raise("death", death, 0);
        r.raise("disconnect", alert_text::disconnect(CrashWatch::Trouble::Hung, "", false), 1000);
        r.raise("back_in", alert_text::back_in(1, 5), 2000);
        r.raise("keeper_gave_up", alert_text::gave_up_restarts(5), 3000);
        r.run(3000, 10'800'000, 5000);
        std::vector<std::string> delivered;
        for (std::size_t i = 0; i < r.mock->times.size(); ++i)
            if (r.mock->times[i] >= 9'000'000) delivered.push_back(content_of(r.mock->bodies[i]));
        check(delivered.size() == 2, "after 2.5 h offline: two messages (" + std::to_string(delivered.size()) + ")");
        if (delivered.size() == 2) {
            check(delivered[0] == "GoBot · 03:10 15/10 — " + alert_text::gave_up_restarts(5),
                  "the one that needs a person, first, with its time and date");
            check(contains(delivered[1], "Trong lúc mất mạng có 3 thông báo không gửi được (lần cuối 03:10)."),
                  "messages older than 2 h become one summary: " + delivered[1]);
        }
    }
}

// ----------------------------------------------------------- never blocks

void test_never_blocks() {
    section("Alerts: never blocks (AL-A6)");
    struct Slow {
        std::mutex              m;
        std::condition_variable cv;
        bool                    cancelled = false;
        std::atomic<bool>       in_send{ false };
    };
    class SlowChannel final : public AlertChannel {
    public:
        explicit SlowChannel(std::shared_ptr<Slow> s) : s_(std::move(s)) {}
        ChannelReply send(const std::string&) override {
            s_->in_send = true;
            std::unique_lock<std::mutex> lock(s_->m);
            s_->cv.wait_for(lock, std::chrono::seconds(5), [&] { return s_->cancelled; });
            return s_->cancelled ? reply(0, 0, 12017) : reply(204);
        }
        void cancel() override {
            std::lock_guard<std::mutex> lock(s_->m);
            s_->cancelled = true;
            s_->cv.notify_all();
        }

    private:
        std::shared_ptr<Slow> s_;
    };

    auto slow = std::make_shared<Slow>();
    AlertService svc("");
    svc.set_settings(switched_on());
    svc.set_channel(std::make_unique<SlowChannel>(slow));
    svc.start();
    svc.raise("death", alert_text::death(true), now_ms());
    for (int i = 0; i < 300 && !slow->in_send; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(slow->in_send, "a send is under way, and will take 5 s");

    const auto t0 = std::chrono::steady_clock::now();
    const auto events = every_event();
    for (int i = 0; i < 100; ++i) {
        const Sample& s = events[static_cast<std::size_t>(i) % events.size()];
        svc.raise(s.key, s.text, now_ms());
    }
    const auto raised_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(raised_ms < 50, "100 raise() calls meanwhile: " + std::to_string(raised_ms) + " ms in all");

    const auto t1 = std::chrono::steady_clock::now();
    svc.stop();
    const auto stop_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
    check(stop_ms < 3000, "stop() with the send still under way: " + std::to_string(stop_ms) + " ms");
    check(slow->cancelled, "the send was cancelled, not waited for");
    check(!svc.running() && !svc.status().webhook, "the worker is gone, and the webhook with it");
}

// ------------------------------------------------------------ secret store

void test_secret_store() {
    section("Alerts: the webhook kept with DPAPI (AL-A7)");
    const std::string path = temp_path("gobot_test_alerts_webhook.bin");
    const SecretStore store(path);
    store.clear();
    const std::string url = webhook();
    check(store.save(url) && store.exists(), "saved");
    const std::string bytes = read_all(path);
    std::wstring wurl = wide(url);
    const std::string url16(reinterpret_cast<const char*>(wurl.data()), wurl.size() * sizeof(wchar_t));
    check(!bytes.empty() && !contains(bytes, url) && !contains(bytes, kToken) && !contains(bytes, kId) &&
              !contains(bytes, url16),
          "the file holds neither the address, the token nor the id");
    std::string back;
    check(store.load(back) && back == url, "and gives the address back");

    WebhookUrl parsed;
    check(WebhookUrl::parse(url, parsed) && parsed.masked() == "…Yz01",
          "masked(): the token's last 4 characters and nothing else");

    write_all(path, bytes.substr(0, bytes.size() / 2));
    check(!store.load(back) && back.empty(), "a cut-short file does not load, and nothing breaks");
    std::string altered = bytes;
    altered[altered.size() / 2] = static_cast<char>(altered[altered.size() / 2] ^ 0x5A);
    write_all(path, altered);
    check(!store.load(back), "an altered file does not load");
    write_all(path, "");
    check(!store.load(back), "an empty file does not load");
    check(store.clear() && !store.exists(), "clear() deletes the file");
    check(store.clear(), "and clearing nothing is no failure");

    // Through the service, as the tab does it: on the worker's step.
    const std::string svc_path = temp_path("gobot_test_alerts_webhook_svc.bin");
    SecretStore(svc_path).clear();
    auto mock = std::make_shared<MockState>();
    {
        AlertService svc(svc_path);
        svc.set_channel_factory([mock](const WebhookUrl&) { return std::make_unique<MockChannel>(mock); });
        svc.save_webhook("  " + url + "\n");
        check(svc.status().busy, "a save waits for the worker");
        svc.step_jobs();
        const AlertStatus st = svc.status();
        check(st.webhook && st.masked == "…Yz01" && !st.busy, "saved by the service: the tab shows …Yz01");
        bool clean = true;
        for (const std::string& l : st.lines) clean = clean && !contains(l, kToken) && !contains(l, kId);
        check(clean, "and its status lines do not show the address");
    }
    {
        AlertService svc(svc_path);
        svc.set_channel_factory([mock](const WebhookUrl&) { return std::make_unique<MockChannel>(mock); });
        svc.load_webhook();
        svc.step_jobs();
        check(svc.status().webhook, "read back by the next GoBot");
        svc.save_webhook("https://hooks.slack.com/services/T000/B000/XXXX");
        svc.step_jobs();
        check(svc.status().webhook && SecretStore(svc_path).load(back) && back == url,
              "a paste that is not a webhook changes nothing");
        svc.clear_webhook();
        svc.step_jobs();
        check(!svc.status().webhook && !SecretStore(svc_path).exists(), "Xoá webhook deletes it");
    }
    {
        write_all(svc_path, "not a DPAPI blob at all");
        AlertService svc(svc_path);
        svc.load_webhook();
        svc.step_jobs();
        const AlertStatus st = svc.status();
        check(!st.webhook && st.unreadable, "a file that will not decrypt counts as no webhook");
        bool said = false;
        for (const std::string& l : st.lines) said = said || contains(l, "Không đọc được webhook đã lưu — hãy dán lại.");
        check(said, "and the tab says to paste it again");
    }

    // A webhook Discord refused is still refused by the next GoBot opened
    // (AL-CR-3), until a new one is saved or a Gửi thử goes through.
    {
        const std::string marker = svc_path + ".refused";
        const auto marked = [&] { return GetFileAttributesW(wide(marker).c_str()) != INVALID_FILE_ATTRIBUTES; };
        auto refusing = std::make_shared<MockState>();
        refusing->otherwise = reply(404);
        const auto factory = [refusing](const WebhookUrl&) { return std::make_unique<MockChannel>(refusing); };
        {
            AlertService svc(svc_path);
            svc.set_channel_factory(factory);
            svc.set_settings(switched_on());
            svc.save_webhook(url);
            svc.step_jobs();
            svc.raise("death", alert_text::death(true), 0);
            svc.step_discord(0);
            check(svc.status().discord == AlertStatus::Discord::Disabled && marked() &&
                      read_all(marker) == "404",
                  "a 404 is kept beside the stored webhook, as the number alone");
        }
        {
            AlertService svc(svc_path);
            svc.set_channel_factory(factory);
            svc.set_settings(switched_on());
            svc.load_webhook();
            svc.step_jobs();
            const AlertStatus st = svc.status();
            check(st.webhook && st.discord == AlertStatus::Discord::Disabled && st.code == 404 &&
                      contains(st.note, "Discord trả lời 404"),
                  "the next GoBot reads it back: still refused, said in red");
            const std::size_t before = refusing->bodies.size();
            svc.raise("death", alert_text::death(true), 1000);
            svc.step_discord(1000);
            check(refusing->bodies.size() == before, "and posts nothing to the dead webhook");
            refusing->replies = { reply(204) };
            svc.send_test(2000);
            svc.step_discord(2000);
            check(svc.status().discord == AlertStatus::Discord::Sent && !marked(),
                  "a Gửi thử that goes through ends it, on disk too");
        }
        {
            AlertService svc(svc_path);
            svc.set_channel_factory(factory);
            svc.set_settings(switched_on());
            svc.load_webhook();
            svc.step_jobs();
            svc.raise("death", alert_text::death(true), 0);
            svc.step_discord(0);
            check(marked(), "refused again");
            svc.save_webhook(url);
            svc.step_jobs();
            check(!marked() && svc.status().discord == AlertStatus::Discord::Ready,
                  "a webhook saved anew starts clean");
            svc.raise("death", alert_text::death(true), 5000);
            svc.step_discord(5000);
            svc.clear_webhook();
            svc.step_jobs();
            check(!marked() && !SecretStore(svc_path).exists(), "Xoá webhook takes the refusal with it");
        }
        DeleteFileW(wide(marker).c_str());
    }
    SecretStore(svc_path).clear();
    wipe_string(back);
}

// --------------------------------------------------------------- the address

void test_webhook_url() {
    section("Alerts: what counts as a webhook (AL-A8)");
    const std::string tail = std::string("/api/webhooks/") + kId + "/" + kToken;
    WebhookUrl u;
    for (const std::string& ok : std::vector<std::string>{ webhook(), webhook("discordapp.com"), webhook("canary.discord.com"),
                                   webhook("ptb.discord.com"),
                                   "https://discord.com/api/v10/webhooks/" + std::string(kId) + "/" + kToken,
                                   webhook() + "?thread_id=987654321098765432",
                                   "   " + webhook() + " \r\n",
                                   "HTTPS://Discord.COM" + tail })
        check(WebhookUrl::parse(ok, u), "accepted: " + ok.substr(0, 40) + "...");
    check(WebhookUrl::parse("  " + webhook("ptb.discord.com") + "?thread_id=42 ", u) && u.host == "ptb.discord.com" &&
              u.id == kId && u.token == kToken && u.path == tail + "?thread_id=42",
          "taken apart: host, id, token, and the path with its thread");

    for (const std::string& bad : std::vector<std::string>{
             "http://discord.com" + tail,
             "https://discord.com.evil.com" + tail,
             "https://evil.com/discord.com" + tail,
             "https://discord.com:8443" + tail,
             "https://user@discord.com" + tail,
             "https://discord.com/api/webhooks/" + std::string(kId),
             "https://discord.com/api/webhooks/" + std::string(kId) + "/",
             // Made at run time: package.ps1 refuses a staged file -- this
             // test's executable among them -- with a webhook address in it.
             "https://discord.com/api/webhooks/" + std::string("1234567890123456") + "/" + kToken,
             "https://discord.com/api/webhooks/" + std::string("123456789012345678901") + "/" + kToken,
             "https://discord.com/api/webhooks/" + std::string(kId) + "/" + std::string(39, 'a'),
             "https://discord.com/api/webhooks/" + std::string(kId) + "/" + std::string(kToken) + "/",
             "https://discord.com/api/webhooks/" + std::string(kId) + "/abc.def" + std::string(40, 'a'),
             webhook() + "?wait=true",
             "https://hooks.slack.com/services/T000/B000/XXXX",
             "https://discord.com/api/channels/123/messages",
             std::string(),
             std::string("   "),
         })
        check(!WebhookUrl::parse(bad, u) && u.token.empty(), "refused: " + (bad.empty() ? "(empty)" : bad.substr(0, 46)));
    check(std::string(kWebhookRefused) ==
              "Đây không phải đường dẫn webhook Discord (phải bắt đầu bằng https://discord.com/api/webhooks/).",
          "and the tab says what it wants");
}

// ----------------------------------------------------------------- privacy

void test_privacy() {
    section("Alerts: what is never sent (AL-A9)");
    const std::string path = "C:/Users/Someone/AppData/VTCZone/VTCZone.exe";
    check(contains(AutoLogin::advice(AutoLogin::Why::LauncherMissing, path), "C:/Users/Someone"),
          "(the advice for the screen does name the path)");
    const std::string missing = alert_text::gave_up_crossing(3, AutoLogin::Why::LauncherMissing);
    check(!contains(missing, "C:/") && !contains(missing, "Someone"), "keeper_gave_up does not: " + missing);
    const std::string covered = alert_text::gave_up_crossing(3, AutoLogin::Why::Covered);
    check(!contains(covered, "Zalo - Someone") && !contains(AutoLogin::advice(AutoLogin::Why::Covered, "Zalo - Someone"), covered),
          "nor the title of a window in the way: " + covered);
    check(!contains(alert_text::disconnect(CrashWatch::Trouble::ErrorBox, "C:/Users/Someone", true), "Someone"),
          "a 'code' that is not one is left out of disconnect");
    check(!contains(alert_text::party_joined("Someone", false), "Someone") &&
              !contains(alert_text::party_invited("Someone", false), "Someone"),
          "party_change without party_names has no name");
    check(contains(alert_text::party_joined("Someone", true), "Someone"), "with it, it has");
    check(alert_text::party_joined("@everyone\n`x`", true) == "Đã vào tổ đội của everyonex.",
          "a name read off the screen loses '@', '`' and control characters");

    // A whole run, with a webhook carrying a marker, and the log looked through.
    const std::string marker_token = "MaRkErToKeN_" + std::string(48, 'Q') + "_zZ9";
    const std::string marker_id = "987654321098765432";
    const std::string url = "https://discord.com/api/webhooks/" + marker_id + "/" + marker_token;
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(4000);
    log.clear_ring();
    log.set_level(LogLevel::Trace);
    {
        const std::string store_path = temp_path("gobot_test_alerts_marker.bin");
        SecretStore(store_path).clear();
        auto mock = std::make_shared<MockState>();
        std::uint64_t now = 0;
        mock->now = &now;
        mock->replies = { reply(204), reply(429, 1500), reply(500), reply(0, 0, 12007), reply(400),
                          reply(404) };
        AlertService svc(store_path);
        svc.set_clock(fake_clock());
        svc.set_channel_factory([mock](const WebhookUrl&) { return std::make_unique<MockChannel>(mock); });
        AlertSettings s = switched_on();
        s.label = "Máy 1";
        svc.set_settings(s);
        svc.save_webhook(url);
        svc.step_jobs();
        svc.load_webhook();
        svc.step_jobs();
        for (const Sample& e : every_event()) svc.raise(e.key, e.text, now);
        for (now = 0; now < 900'000; now += 500) svc.step_discord(now);
        svc.send_test(now);
        svc.step_discord(now);
        {
            WebhookUrl u;
            WebhookUrl::parse(url, u);
            auto real = make_discord_channel(u);       // made, never asked to send
            u.wipe();
        }
        const std::string ini = temp_path("gobot_test_alerts_marker.ini");
        write_all(ini, "[Alerts]\nenabled=true\nwebhook=" + url + "\nlabel=" + url + "\n");
        AlertSettings from_file;
        check(from_file.load(ini) && from_file.webhook_in_file && from_file.label.empty(),
              "a webhook written in alerts.ini is ignored, in any key");
        remove_file(ini);
        svc.clear_webhook();
        svc.step_jobs();
        bool clean = true;
        for (const std::string& l : svc.status().lines)
            clean = clean && !contains(l, marker_token) && !contains(l, marker_id);
        check(clean, "the tab's status lines carry neither the token nor the id");
        svc.stop();
    }
    bool clean = true;
    std::size_t lines = 0;
    for (const LogLine& l : log.ring()) {
        ++lines;
        if (contains(l.text, marker_token) || contains(l.text, marker_id) ||
            contains(l.text, "discord.com/api/webhooks") || contains(l.text, "discordapp.com/api/webhooks")) {
            clean = false;
            std::printf("    leaked: %s\n", l.text.c_str());
        }
    }
    check(lines > 5, "the run logged (" + std::to_string(lines) + " lines)");
    check(clean, "no log line has the token, the id or the webhook's address");
    log.set_level(was);
    log.set_ring_capacity(0);

#ifdef GOBOT_SOURCE_DIR
    // Only where this was built: the source tree is not on a tester's machine.
    if (read_all(std::string(GOBOT_SOURCE_DIR) + "/src/alerts.cpp").empty()) {
        std::printf("  [skip] the source tree is not here, so its files are not read for captchas\n");
    } else {
        bool none = true;
        for (const char* file : { "/include/gobot/alerts.hpp", "/src/alerts.cpp",
                                  "/apps/gobot_gui/simple_alerts.cpp", "/configs/alerts.ini" }) {
            std::string text = read_all(std::string(GOBOT_SOURCE_DIR) + file);
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (text.empty() || contains(text, "captcha")) {
                none = false;
                std::printf("    %s: %s\n", file, text.empty() ? "not read" : "says captcha");
            }
        }
        check(none, "alerts.hpp, alerts.cpp, simple_alerts.cpp and alerts.ini say nothing of captchas");
    }
#endif
}

// ---------------------------------------------------------- off means off

void test_off_means_off() {
    section("Alerts: off means off (AL-A10)");
    const auto raise_all = [](Rig& r) {
        for (int round = 0; round < 3; ++round)
            for (const Sample& e : every_event()) r.raise(e.key, e.text, r.now);
        r.run(r.now, r.now + 300'000, 500);
    };
    {
        AlertSettings s = switched_on();
        s.enabled = false;
        Rig r(s);
        raise_all(r);
        check(r.mock->bodies.empty() && r.notifier.toasts.empty() && r.notifier.beeps == 0,
              "enabled=false: no request, no beep, no notification");
        check(r.svc.send_test(r.now), "Gửi thử");
        r.run(r.now, r.now + 5000, 500);
        check(r.mock->bodies.size() == 1, "still makes exactly one request");
    }
    {
        AlertSettings s = switched_on();
        for (const AlertEventInfo& e : alert_events()) s.events[e.key] = false;
        Rig r(s);
        raise_all(r);
        check(r.mock->bodies.empty() && r.notifier.toasts.empty(), "every event unticked: nothing at all");
        check(r.svc.send_test(r.now), "Gửi thử");
        r.run(r.now, r.now + 5000, 500);
        check(r.mock->bodies.size() == 1, "still makes exactly one request");
        check(!r.svc.send_test(r.now), "and not a second within 30 s");
    }
    {
        Rig r(switched_on(), false);
        raise_all(r);
        check(r.mock->bodies.empty(), "no webhook saved: no request");
        check(!r.notifier.toasts.empty() && r.notifier.beeps > 0, "while the beep and notification go on");
    }
    {
        AlertSettings s = switched_on();
        s.discord = false;
        Rig r(s);
        raise_all(r);
        check(r.mock->bodies.empty() && !r.notifier.toasts.empty(), "Gửi vào Discord unticked: only this machine");
    }
    {
        Rig r(switched_on());
        check(!r.svc.raise("captain", "x", 0) && !r.svc.raise("test", "x", 0), "a key no event has is not sent");
        AlertSettings s = switched_on();
        s.events["party_change"] = false;
        Rig q(s);
        check(!q.svc.raise("party_change", alert_text::party_joined("", false), 0), "party_change is off by default's tick");
        AlertSettings fresh;
        check(!fresh.event_on("party_change") && fresh.event_on("death") && fresh.event_on("revive_failed"),
              "the defaults: every event on but party_change");
    }
    {
        Rig r(switched_on());
        r.raise("death", alert_text::death(true), 0);
        AlertSettings off = switched_on();
        off.enabled = false;
        r.svc.set_settings(off);
        r.run(0, 200'000, 500);
        check(r.mock->bodies.empty() && r.notifier.toasts.empty(), "turned off with one waiting: it is dropped");
    }
}

// ------------------------------------------------------------- this machine

void test_local() {
    section("Alerts: the beep and the notification");
    AlertSettings s = switched_on();
    s.label = "Máy 1";
    Rig r(s, false);
    r.raise("death", alert_text::death(true), 0);
    r.at(0);
    r.raise("disconnect", alert_text::disconnect(CrashWatch::Trouble::Gone, "", true), 2000);
    r.at(2000);
    r.raise("back_in", alert_text::back_in(1, 5), 4000);
    r.at(4000);
    check(r.notifier.toasts.size() == 3 && r.notifier.beeps == 1, "three notifications, one beep within 10 s");
    r.raise("revived", alert_text::revived(), 12'000);
    r.at(12'000);
    check(r.notifier.beeps == 2, "and the next beep 10 s on");
    if (!r.notifier.toasts.empty()) {
        check(r.notifier.toasts[0].first == "GoBot · Máy 1", "the title is GoBot and the label");
        check(r.notifier.toasts[0].second == "03:10 — " + alert_text::death(true), "the text is the time and the message");
    }

    AlertSettings quiet = switched_on();
    quiet.sound = false;
    quiet.toast = false;
    Rig q(quiet, false);
    q.raise("death", alert_text::death(true), 0);
    q.at(0);
    check(q.notifier.beeps == 0 && q.notifier.toasts.empty(), "sound and notification unticked: neither");
}

// ---------------------------------------------------------------- settings

void test_settings() {
    section("Alerts: configs/alerts.ini (AL-A11)");
    const std::string path = temp_path("gobot_test_alerts.ini");
    remove_file(path);
    AlertSettings none;
    check(!none.load(path) && !none.enabled && none.discord && none.sound && none.toast && none.merge_s == 60 &&
              none.max_per_minute == 5 && none.max_per_hour == 30 && none.not_in_world_min == 3 &&
              !none.party_names,
          "no file: the defaults, with alerts off");

    check(AlertSettings::save_key(path, "Alerts", "enabled", "true"), "a key written to a file not there makes it");
    AlertSettings::save_key(path, "Alerts", "label", "Máy 1");
    AlertSettings::save_key(path, "Alerts", "merge_s", "120");
    AlertSettings::save_key(path, "Alerts", "max_per_minute", "3");
    AlertSettings::save_key(path, "Alerts", "not_in_world_min", "7");
    AlertSettings::save_key(path, "Alerts", "sound", "false");
    AlertSettings::save_key(path, "Events", "death", "false");
    AlertSettings::save_key(path, "Events", "party_change", "true");
    AlertSettings::save_key(path, "Events", "party_names", "true");
    AlertSettings back;
    check(back.load(path) && back.enabled && back.label == "Máy 1" && back.merge_s == 120 &&
              back.max_per_minute == 3 && back.not_in_world_min == 7 && !back.sound && back.toast &&
              !back.event_on("death") && back.event_on("party_change") && back.party_names &&
              back.event_on("disconnect"),
          "it round-trips");

    write_all(path, "# giữ dòng này\n[Alerts]\n# và dòng này\nenabled=false\n\n[Events]\n# cả dòng này\n");
    AlertSettings::save_key(path, "Alerts", "enabled", "true");
    AlertSettings::save_key(path, "Events", "revived", "false");
    const std::string text = read_all(path);
    check(contains(text, "# giữ dòng này\n") && contains(text, "# và dòng này\n") && contains(text, "# cả dòng này\n") &&
              contains(text, "enabled=true\n") && contains(text, "revived=false\n") && !contains(text, "\r"),
          "set_in_place keeps the comments, and the LF line ends");

    write_all(path, "\xEF\xBB\xBF[ALERTS]\nEnabled = TRUE\nmerge_s=5\nmax_per_minute=99\nnot_in_world_min=0\n"
                    "max_per_hour=060\nlabel=@everyone `Máy` 2 có một cái tên rất là dài quá mức cho phép\n"
                    "[events]\nDEATH=no\n");
    AlertSettings odd;
    check(odd.load(path) && odd.enabled && odd.merge_s == 10 && odd.max_per_minute == 10 &&
              odd.not_in_world_min == 1 && odd.max_per_hour == 60 && !odd.event_on("death"),
          "a BOM, any case, and numbers brought into range");
    check(odd.label == "everyone Máy 2 có một cái tên rấ" && code_points(odd.label) == 32,
          "the label loses '@' and '`' and is cut to 32 characters: " + odd.label);

    write_all(path, "[Alerts]\nenabled=true\nwebhook=" + webhook() + "\n");
    AlertSettings with_hook;
    check(with_hook.load(path) && with_hook.enabled && with_hook.webhook_in_file,
          "a webhook= line is noticed, and not used");

    // A file there already is only ever changed, never emptied and started
    // again (AL-CR-1); an empty one gets the few lines a new one starts with.
    write_all(path, "[Alerts]\nenabled=false\n");
    AlertSettings::save_key(path, "Alerts", "sound", "false");
    const std::string kept = read_all(path);
    check(kept.rfind("[Alerts]\nenabled=false\n", 0) == 0 && contains(kept, "sound=false\n") &&
              !contains(kept, "# "),
          "save_key on a file there already changes it and adds nothing else");
    write_all(path, "");
    AlertSettings::save_key(path, "Alerts", "sound", "false");
    AlertSettings fresh;
    check(fresh.load(path) && !fresh.enabled && !fresh.sound && contains(read_all(path), "# "),
          "an empty file is started the way a missing one is, with alerts off");
    remove_file(path);

    // A path in the system's code page, with a folder of a non-English name
    // in it -- as exe_relative gave them before it gave UTF-8, and as an older
    // editor may have saved one in an ini. Asked about one way and opened
    // another, that folder was not found, and every edit emptied the file --
    // comments, enabled and all. Bytes that are not UTF-8 are read in the
    // code page, so it still works. Only where the code page is not UTF-8;
    // there both read the same.
    if (GetACP() == CP_UTF8) {
        std::printf("  [skip] this machine's code page is UTF-8, where a path reads the same both ways\n");
    } else {
        const std::wstring wdir = wide(temp_path("gobot_test_\xC3\xA9t\xC3\xA9"));
        char ansi[MAX_PATH] = {};
        BOOL lossy = FALSE;
        const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wdir.c_str(), -1, ansi,
                                          MAX_PATH, nullptr, &lossy);
        if (n <= 0 || lossy) {
            std::printf("  [skip] this machine's code page cannot write that folder's name\n");
        } else {
            CreateDirectoryW(wdir.c_str(), nullptr);
            const std::wstring wfile = wdir + L"\\alerts.ini";
            {
                std::ofstream out(wfile, std::ios::binary | std::ios::trunc);
                out << "# giữ dòng này\n[Alerts]\nenabled=true\n";
            }
            const bool saved = AlertSettings::save_key(std::string(ansi) + "\\alerts.ini", "Alerts",
                                                       "sound", "false");
            std::ifstream in(wfile, std::ios::binary);
            const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            check(saved && contains(after, "# giữ dòng này\n") && contains(after, "enabled=true\n") &&
                      contains(after, "sound=false"),
                  "in a folder with a non-English name, given in the system's code page: the file keeps "
                  "its comments and its other keys");
            DeleteFileW(wfile.c_str());
            RemoveDirectoryW(wdir.c_str());
        }
    }

#ifdef GOBOT_SOURCE_DIR
    const std::string shipped = std::string(GOBOT_SOURCE_DIR) + "/configs/alerts.ini";
    if (read_all(shipped).empty()) {
        // Only where this was built: the source tree is not on a tester's machine.
        std::printf("  [skip] the source tree's configs/alerts.ini is not here\n");
        return;
    }
    AlertSettings ship;
    check(ship.load(shipped) && !ship.enabled && ship.discord && ship.sound && ship.toast && ship.label.empty() &&
              ship.merge_s == 60 && ship.max_per_minute == 5 && ship.max_per_hour == 30 &&
              ship.not_in_world_min == 3 && !ship.party_names && !ship.webhook_in_file,
          "the shipped configs/alerts.ini loads, with alerts off");
    const std::string body = read_all(shipped);
    bool every = true;
    for (const AlertEventInfo& e : alert_events()) {
        if (e.label_vi == nullptr) continue;
        const bool listed = contains(body, std::string("\n") + e.key + "=");
        every = every && listed && ship.event_on(e.key) == e.on;
    }
    check(every, "and lists every event, each at its default");
    check(!contains(body, "\r") && body.compare(0, 3, "\xEF\xBB\xBF") != 0, "UTF-8 without a BOM, LF");
#endif
}

// -------------------------------------------------------------------- watch

void test_watch() {
    section("Alerts: something that has to last");
    AlertWatch w(2000);
    check(!w.due(true, 1000) && !w.due(true, 2999), "not before it has lasted");
    check(w.due(true, 3000) && w.told(), "once it has");
    check(!w.due(true, 9000), "and only once");
    check(!w.due(false, 9100) && !w.told(), "over: ready again");
    check(!w.due(true, 9200) && w.due(true, 11'200), "a second time, after it lasts again");

    // Not in the world (AL-R4c).
    using Say = AwayWatch::Say;
    const std::uint64_t min3 = 3 * 60'000;
    {
        AwayWatch a(min3);
        bool any = false;
        for (std::uint64_t t = 0; t <= 2 * min3; t += 1000)
            any = any || a.step(true, true, false, -1.0f, t) != Say::Nothing;
        check(!any, "a reader that never read the bars says nothing, however long");
    }
    {
        AwayWatch a(min3);
        a.step(true, true, false, 80.0f, 0);
        check(a.step(true, true, false, -1.0f, 1000) == Say::Nothing &&
                  a.step(true, true, false, -1.0f, 1000 + min3 - 1) == Say::Nothing,
              "the bars gone for less than 3 minutes: nothing");
        check(a.step(true, true, false, -1.0f, 1000 + min3) == Say::Away, "3 minutes: away");
        check(a.step(true, true, false, -1.0f, 1000 + 2 * min3) == Say::Nothing, "said once");
        check(a.step(true, true, false, 75.0f, 1000 + 2 * min3 + 500) == Say::Back, "and back once they are");
        check(a.step(true, true, false, 75.0f, 1000 + 2 * min3 + 1000) == Say::Nothing, "back only once");
    }
    {
        AwayWatch a(min3);
        a.step(true, true, false, 80.0f, 0);
        bool any = false;
        for (std::uint64_t t = 1000; t <= 2 * min3; t += 1000)
            any = any || a.step(true, true, true, -1.0f, t) != Say::Nothing;
        check(!any, "nothing while the keeper is crossing back in");
        a.step(true, true, false, -1.0f, 2 * min3 + 1000);
        check(a.step(true, true, false, -1.0f, 3 * min3 + 1000) == Say::Away,
              "the time counts from the end of the crossing");
        a.step(false, true, false, -1.0f, 3 * min3 + 2000);
        check(a.step(true, true, false, 90.0f, 3 * min3 + 3000) == Say::Nothing,
              "a run stopped in between: no 'back'");
    }

    // A death told by HP alone (AL-R4a).
    {
        DeathByHp d(3000);
        check(!d.step(true, 0, 9000, 0) && !d.step(true, 0, 9000, 2999), "HP 0 for under 3 s: not yet");
        check(d.step(true, 0, 9000, 3000), "HP 0 of 9000 for 3 s: dead");
        check(!d.step(true, 0, 9000, 60'000), "said once a death");
        check(!d.step(true, -1, -1, 61'000) && !d.step(true, 0, 9000, 120'000),
              "an unread figure does not make it news again");
        check(!d.step(false, 0, 9000, 121'000) && !d.step(true, 0, 9000, 200'000),
              "nor a run stopped and started while dead");
        check(!d.step(true, 4500, 9000, 201'000) && !d.step(true, 0, 9000, 202'000) &&
                  d.step(true, 0, 9000, 205'000),
              "HP above 0 again, then 0 for 3 s: the next death");
        DeathByHp z(3000);
        check(!z.step(true, 0, 0, 0) && !z.step(true, 0, 0, 10'000) && !z.step(true, 0, -1, 20'000),
              "0 of nothing, or of an unread maximum, is no death");
    }
}

} // namespace

void test_alerts(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_texts();
    test_json();
    test_merging();
    test_caps();
    test_replies();
    test_never_blocks();
    test_secret_store();
    test_webhook_url();
    test_privacy();
    test_off_means_off();
    test_local();
    test_settings();
    test_watch();
}
