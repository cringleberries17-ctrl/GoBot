// gobot_test -- the keeper's watchdog: a game that runs and answers but has
// nobody in the world.
//
// Scripted looks on a pretend clock for the rule; frames saved from the live
// client, and frames drawn here, for what a look can tell. No game is looked
// at -- a window of the test's own, off every screen, stands in for the
// game's where one is needed -- and nothing is ended, started or clicked.

#include "gobot/stall_watch.hpp"
#include "gobot/alerts.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/crash_watch.hpp"
#include "gobot/game_install.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

template <typename... Args>
std::string fmt(const char* f, Args... args) {
    char buf[512];
    std::snprintf(buf, sizeof buf, f, args...);
    return buf;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// A folder in this run's own temp folder, which main.cpp makes and removes:
/// see temp_file there.
std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

const char* const kLiveBuild = "C:\\Users\\player\\source\\GoBot\\build\\";

constexpr std::uint64_t kLookMs = 5000;
constexpr std::uint64_t kMinute = 60'000;

StallSettings on(int minutes = 5) {
    StallSettings s;
    s.enabled = true;
    s.stall_min = minutes;
    s.loading_max_min = 3 * minutes;
    return s;
}

StallLook nobody() { return StallLook{}; }

/// Looks every 5 s from `from` to `to`, each made by `look(t)`; the first
/// moment it says Stalled, or 0 when it never does.
std::uint64_t stalls_at(StallWatch& w, std::uint64_t from, std::uint64_t to,
                        const std::function<StallLook(std::uint64_t)>& look) {
    for (std::uint64_t t = from; t <= to; t += kLookMs)
        if (w.look(look(t), t).state == StallWatch::State::Stalled) return t;
    return 0;
}

std::string mmss(std::uint64_t ms) {
    return fmt("%llu:%02llu", static_cast<unsigned long long>(ms / kMinute),
               static_cast<unsigned long long>(ms % kMinute / 1000));
}

Frame plain_frame(std::uint8_t grey = 40) {
    Frame f;
    f.width = 1280;
    f.height = 720;
    f.pixels.assign(static_cast<std::size_t>(f.width) * f.height * 4, grey);
    for (std::size_t i = 3; i < f.pixels.size(); i += 4) f.pixels[i] = 255;
    return f;
}

void blit(Frame& f, const Frame& pic, int x, int y) {
    for (int py = 0; py < pic.height; ++py)
        for (int px = 0; px < pic.width; ++px) {
            const int fx = x + px, fy = y + py;
            if (fx < 0 || fy < 0 || fx >= f.width || fy >= f.height) continue;
            const std::uint8_t* s = &pic.pixels[(static_cast<std::size_t>(py) * pic.width + px) * 4];
            if (s[3] < TemplateMatcher::kOpaque) continue;
            std::uint8_t* d = &f.pixels[(static_cast<std::size_t>(fy) * f.width + fx) * 4];
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
}

void test_rule() {
    section("Watchdog: no HUD for stall_min minutes of looks that count (WD-A1 to WD-A5)");
    const std::uint64_t t0 = 1'000'000;   // a clock that does not start at nought
    {
        StallWatch w(on(5));
        const std::uint64_t at = stalls_at(w, t0, t0 + 20 * kMinute, [](std::uint64_t) { return nobody(); });
        check(at == t0 + 5 * kMinute, "no HUD, a screen that does not move: stalled at 5:00 -- " +
                                          mmss(at - t0) + " (WD-A1)");
        StallWatch v(on(5));
        stalls_at(v, t0, t0 + 5 * kMinute - kLookMs, [](std::uint64_t) { return nobody(); });
        check(v.last().state == StallWatch::State::Counting && v.last().minutes() == 4,
              "and at 4:55 it is still counting, at minute " + std::to_string(v.last().minutes()));
    }
    {
        StallWatch w(on(5));
        const std::uint64_t at = stalls_at(w, t0, t0 + 20 * kMinute, [&](std::uint64_t t) {
            StallLook l;
            l.hud = t == t0 + 2 * kMinute;
            return l;
        });
        // The HUD at 2:00 clears it; the look after counts from nought.
        check(at == t0 + 7 * kMinute + kLookMs, "the HUD back at 2:00: the count starts again from there -- " +
                                                    mmss(at - t0) + " (WD-A2)");
    }
    {
        StallWatch w(on(5));
        const std::uint64_t at = stalls_at(w, t0, t0 + 10 * kMinute, [](std::uint64_t) {
            StallLook l;
            l.death = true;
            return l;
        });
        check(at == 0 && w.last().state == StallWatch::State::Watching,
              "the death panel up for 10 minutes: never (WD-A3)");
    }
    {
        StallWatch w(on(5));
        const std::uint64_t at = stalls_at(w, t0, t0 + 30 * kMinute, [](std::uint64_t) {
            StallLook l;
            l.changed = true;
            return l;
        });
        check(at == t0 + 15 * kMinute, "a screen that keeps moving, no login page -- a map loading: "
                                       "stalled at 15:00, not before -- " + mmss(at - t0) + " (WD-A4)");
    }
    for (const bool animated : { false, true }) {
        StallWatch w(on(5));
        const std::uint64_t at = stalls_at(w, t0, t0 + 6 * kMinute, [&](std::uint64_t) {
            StallLook l;
            l.login_page = true;
            l.changed = animated;
            return l;
        });
        check(at == t0 + 5 * kMinute, std::string("the character select, ") +
                                          (animated ? "moving" : "still") + ", left for 6 minutes: stalled at 5:00 -- " +
                                          mmss(at - t0) + " (WD-A5)");
    }
}

void test_pauses() {
    section("Watchdog: what holds the count where it is (WD-A6, WD-A7)");
    const std::uint64_t t0 = 5'000'000;
    struct Pause {
        const char* what;
        void (*set)(StallLook&);
    };
    const Pause pauses[] = {
        { "blind looks -- the game behind another window", [](StallLook& l) { l.blind = true; } },
        { "GoBot holding the game", [](StallLook& l) { l.held = true; } },
        { "the keeper crossing", [](StallLook& l) { l.crossing = true; } },
    };
    for (const Pause& p : pauses) {
        StallWatch w(on(5));
        // Two minutes counted, two paused, then on.
        const std::uint64_t at = stalls_at(w, t0, t0 + 20 * kMinute, [&](std::uint64_t t) {
            StallLook l;
            if (t > t0 + 2 * kMinute && t <= t0 + 4 * kMinute) p.set(l);
            return l;
        });
        // The pause costs the step into it and the one out of it.
        const std::uint64_t counted = at - t0 - 2 * kMinute;
        check(at != 0 && counted >= 5 * kMinute && counted <= 5 * kMinute + 2 * kLookMs,
              std::string(p.what) + " for 2 of the minutes: paused, not started again -- stalled at " +
                  mmss(at - t0));
    }
    {
        StallWatch w(on(5));
        w.restart(t0);
        const std::uint64_t at = stalls_at(w, t0, t0 + 20 * kMinute, [](std::uint64_t) { return nobody(); });
        check(at == t0 + StallWatch::kGraceMs + 5 * kMinute,
              "just in the world: the first minute does not count -- stalled at " + mmss(at - t0));
    }
    {
        StallSettings off = on(1);
        off.enabled = false;
        StallWatch w(off);
        const std::uint64_t at = stalls_at(w, t0, t0 + 60 * kMinute, [](std::uint64_t) { return nobody(); });
        check(at == 0 && w.last().state == StallWatch::State::Off, "stall_enabled=false: never (WD-A7)");
    }
    {
        StallWatch w(on(5));
        w.look(nobody(), t0);
        w.look(nobody(), t0 + kLookMs);
        // The keeper away for an hour: a step, not an hour.
        const auto v = w.look(nobody(), t0 + 60 * kMinute);
        check(v.state == StallWatch::State::Counting && v.counted_ms <= kLookMs + StallWatch::kMaxStepMs,
              "an hour between two looks counts as one step, not an hour");
    }
    {
        IniFile ini;
        ini.set("Crash", "stall_enabled", "true");
        ini.set("Crash", "stall_min", "90");
        ini.set("Crash", "stall_loading_max_min", "2");
        StallSettings s;
        s.load(ini);
        check(s.enabled && s.stall_min == StallSettings::kMaxMinutes && s.loading_max_min == s.stall_min,
              "stall_min held to 1-30, and the loading allowance never shorter than it");
#ifdef GOBOT_SOURCE_DIR
        IniFile shipped;
        StallSettings d;
        d.enabled = true;
        check(shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/app.ini") &&
                  shipped.has_key("Crash", "stall_enabled") && shipped.has_key("Crash", "stall_min") &&
                  shipped.has_key("Crash", "stall_loading_max_min"),
              "the shipped app.ini has stall_enabled, stall_min and stall_loading_max_min");
        d.load(shipped);
        check(!d.enabled && d.stall_min == 5 && d.loading_max_min == 15,
              "shipped off, at 5 and 15 minutes");
#endif
    }
}

void test_eyes() {
    section("Watchdog: what a look can tell (WD-A10)");
    check(stall_frame_trusted(true, false, false, false), "a frame the game drew itself: trusted wherever it is");
    check(stall_frame_trusted(true, false, true, true),
          "off the screen with the game in front and nothing over it: trusted");
    check(!stall_frame_trusted(true, false, true, false),
          "off the screen with another window in front, or over it: blind");
    check(!stall_frame_trusted(false, false, false, true) && !stall_frame_trusted(true, true, false, true),
          "no frame, or the window minimised: blind");
    // What "nothing over it" is, as the grab decides it: in front, nothing
    // over its client area, and the HUD's corner on a screen -- all three.
    check(stall_screen_clear(true, true, true), "in front, nothing over it, the HUD's corner on a screen: clear");
    check(!stall_screen_clear(false, true, true) && !stall_screen_clear(true, false, true),
          "another window in front, or one over the game: not clear");
    check(!stall_screen_clear(true, true, false) &&
              !stall_frame_trusted(true, false, true, stall_screen_clear(true, true, false)),
          "in front with nothing over it, but the HUD's corner past the screen's edge: not clear, so blind "
          "(the 2A tester's problem 11)");

    Frame a = plain_frame(60);
    StallEyes eyes;
    check(!eyes.changed(a), "the first frame has nothing to be compared with");
    check(!eyes.changed(a), "the same frame again: not moved");
    Frame b = a;
    for (int y = 0; y < 300; ++y)
        for (int x = 0; x < 600; ++x) {
            std::uint8_t* p = &b.pixels[(static_cast<std::size_t>(y) * b.width + x) * 4];
            p[0] = p[1] = p[2] = 200;
        }
    check(eyes.changed(b), "a fifth of the screen gone bright: moved");
    Frame c = b;
    for (int x = 0; x < 40; ++x) {
        std::uint8_t* p = &c.pixels[(static_cast<std::size_t>(700) * c.width + x) * 4];
        p[0] = p[1] = p[2] = 255;
    }
    check(!eyes.changed(c), "a few pixels: not moved");
    eyes.forget();
    check(!eyes.changed(a), "after a blind look: nothing to compare with again");

    // The saved frames, through the watchdog's own look.
    StallLooker looker;
    looker.load(exe_relative("configs/templates.ini"));
    const AutoLogin::Settings login;
    const TemplateStore no_pages;
    Frame frame;
    const std::string frame_png = std::string(kLiveBuild) + "bin\\Release\\frame.png";
    if (!(load_png(exe_relative("frame.png"), frame) || (exists(frame_png) && load_png(frame_png, frame)))) {
        std::printf("  [skip] no frame.png from the live client here\n");
        return;
    }
    check(looker.look_at(frame, true, false, false, login, no_pages).hud, "frame.png: in the world");
    std::vector<std::string> rounds;
    for (const char* pattern : { "r1_A_*.png", "r1_B_*.png", "r2_A_*.png", "r2_B_*.png", "r3_A_*.png",
                                 "r3_B_*.png", "r3_C_*.png", "r4_A_*.png", "r4_B_*.png", "r4_C_*.png" }) {
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((std::string(kLiveBuild) + "live\\shots\\" + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::string name = fd.cFileName;
            // Not the two frames another window covers: the next check's.
            if (name != "r2_B_000.png" && name != "r2_B_001.png")
                rounds.push_back(std::string(kLiveBuild) + "live\\shots\\" + name);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (rounds.empty()) {
        std::printf("  [skip] no live round frames here\n");
        return;
    }
    std::sort(rounds.begin(), rounds.end());
    int world = 0, total = 0;
    std::string missed;
    // Every fourth of them: some two hundred, with the bag open on many.
    for (std::size_t i = 0; i < rounds.size(); i += 4) {
        Frame f;
        if (!load_png(rounds[i], f)) continue;
        ++total;
        const bool w = looker.look_at(f, true, false, false, login, no_pages).hud;
        world += w ? 1 : 0;
        if (!w && missed.empty()) missed = rounds[i].substr(rounds[i].find_last_of('\\') + 1);
    }
    check(world == total, fmt("the HUD is seen on all %d round frames looked at -- every one was taken "
                              "in the world", total) + (missed.empty() ? std::string{} : " -- not " + missed));

    // The frames another window covers were read off the screen with the game
    // behind it: blind, whatever they show, never "no HUD".
    int blind = 0, covered = 0;
    for (const char* rel : { "live\\shots\\r1_before.png", "live\\shots\\r1_pre.png", "live\\shots\\r2_before.png",
                             "live\\shots\\r2_B_000.png", "live\\shots\\r2_B_001.png", "live\\shots\\r3_D_000.png",
                             "live\\shots\\r3_D_001.png", "live\\shots\\r3_D_002.png", "live\\shots\\r3_D_003.png",
                             "live\\shots\\r3_pre.png", "frozen\\live\\shots\\p1_now.png" }) {
        Frame f;
        if (!load_png(std::string(kLiveBuild) + rel, f)) continue;
        ++covered;
        const bool trusted = stall_frame_trusted(true, false, true, false);
        const StallLook l = looker.look_at(f, trusted, false, false, login, no_pages);
        blind += l.blind && !l.hud ? 1 : 0;
    }
    if (covered > 0)
        check(blind == covered, fmt("%d of %d covered frames, read off the screen behind another window: "
                                    "blind", blind, covered));
}

void test_keeper() {
    section("Watchdog: the keeper's side of it (WD-A8, WD-A9)");
    const std::uint64_t t0 = 9'000'000;
    const AutoLogin::Settings login;
    const TemplateStore no_pages;
    {
        // A dark screen with nothing on it, a look every 5 s, stall_min=1 as
        // the live test sets it: the keeper is told to end the client a
        // minute after the minute's grace, with the look it ends it by.
        StallKeeper k;
        k.set_settings(on(1), exe_relative("configs/templates.ini"));
        k.restart(t0);
        const Frame dark = plain_frame(30);
        const std::string kept = temp_path("stall_kept");
        CrashWatch::Look out;
        std::uint64_t at = 0;
        for (std::uint64_t t = t0; t <= t0 + 4 * kMinute && at == 0; t += kLookMs)
            if (k.judge(dark, true, false, login, no_pages, t, out, kept)) at = t;
        check(at == t0 + StallWatch::kGraceMs + 1 * kMinute,
              "nobody in the world: stalled 1 minute after the minute's grace -- at " + mmss(at - t0));
        check(out.what == CrashWatch::Trouble::Stalled && out.minutes == 1 &&
                  out.detail == "no character in the world for 1 min (screen: unknown; popup: none)",
              "the look to end it by: Stalled, 1 minute -- \"" + out.detail + "\"");
        check(!files_in(kept, "stall_*.png").empty(), "and the frame kept, as keeper_debug keeps it");
        check(CrashWatch::next_move(out, true, false) == CrashWatch::Move::Restart,
              "which the keeper takes the way it takes a crash: a restart, counted against max_restarts");
        CrashWatch::Look again;
        check(!k.judge(dark, true, false, login, no_pages, at + kLookMs, again, {}),
              "and the count starts afresh once it has said so");
    }
    {
        // A screen read while the game was behind another window, or GoBot
        // holding the game, never ends it.
        StallKeeper k;
        k.set_settings(on(1), exe_relative("configs/templates.ini"));
        const Frame dark = plain_frame(30);
        CrashWatch::Look out;
        bool any = false;
        for (std::uint64_t t = t0; t <= t0 + 3 * kMinute; t += kLookMs)
            any = any || k.judge(dark, false, false, login, no_pages, t, out, {}) ||
                  k.judge(dark, true, true, login, no_pages, t + 1, out, {});
        check(!any, "blind looks and GoBot's own holds, for three times stall_min: never");
    }
    {
        StallSettings off = on(1);
        off.enabled = false;
        StallKeeper k;
        k.set_settings(off, exe_relative("configs/templates.ini"));
        CrashWatch::Look out;
        bool any = false;
        for (std::uint64_t t = t0; t <= t0 + 30 * kMinute; t += kLookMs)
            any = any || k.check(nullptr, "Game.exe", false, login, no_pages, t, out, {});
        check(!any, "switched off: never, and no game is even looked at");
    }

    // The in-game dialog read before the client is ended: its words into the
    // log, and nothing clicked -- the looker has no hands to click with.
    StallLooker looker;
    looker.load(exe_relative("configs/templates.ini"));
    TemplateStore pictures;
    Frame dialog;
    GlyphFont font;
    if (!TemplateStore::load_one("grabbed/dialog_ok", exe_relative("configs/templates.ini"), dialog) ||
        !font.load(GlyphFont::default_dir())) {
        std::printf("  [skip] no grabbed/dialog_ok or no glyphs here\n");
        return;
    }
    Frame shot = plain_frame(30);
    blit(shot, dialog, 540, 320);
    Frame words;
    check(font.render_line("Mat ket noi", words), "the words drawn with the game's own glyphs");
    blit(shot, words, 540 + 24, 320 + 14);
    const std::string said = looker.ok_popup(shot);
    check(said.find("a notice with Xác Nhận at") != std::string::npos &&
              said.find("Mat ket noi") != std::string::npos,
          "dialog_ok with words on it: read for the log -- " + said + " (WD-A9)");
    check(looker.ok_popup(plain_frame(30)) == "none", "and none when there is no dialog");

    // A question -- [Xác Nhận][Hủy Bỏ] -- is said to be one, its words read
    // where they are: the pair picture alone missed the yes/no questions.
    Frame ask;
    if (TemplateStore::load_one("grabbed/del_item_confirm_1", exe_relative("configs/templates.ini"), ask)) {
        Frame q = plain_frame(30);
        blit(q, ask, 500, 300);
        const std::string asked = looker.ok_popup(q);
        check(asked.find("a question with Xác Nhận at") != std::string::npos,
              "the bin's question drawn: a question -- " + asked);
    }
    // The revive question too, which the tester saw logged as a notice.
    Frame revive;
    if (TemplateStore::load_one("hoisinh_confirm", exe_relative("configs/templates.ini"), revive)) {
        Frame q = plain_frame(30);
        blit(q, revive, 500, 300);
        const std::string asked = looker.ok_popup(q);
        check(asked.find("a question with Xác Nhận at") != std::string::npos,
              "the revive question drawn: a question, not a notice -- " + asked + " (WD-R5)");
    }
    Frame live;
    if (load_png(std::string(kLiveBuild) + "frozen\\live\\shots\\p5_probe_050.png", live)) {
        const std::string asked = looker.ok_popup(live);
        check(asked.find("a question with Xác Nhận at") != std::string::npos,
              "the live bin's question (p5_probe_050): a question, not a notice -- " + asked);
        // Its words read where a question has them, no longer from a notice's
        // place beside them: "Bạn có thật sự muốn bỏ vào thùng rác không".
        check(asked.find("Bạn có") != std::string::npos && asked.find("rác") != std::string::npos,
              "and its words read from where they are, not garbage (WD-R5, WD-A9)");
    } else {
        std::printf("  [skip] no frozen\\live\\shots\\p5_probe_050.png here\n");
    }
}

void test_texts() {
    section("Watchdog: what the player is told (WD-A8, WD-R5, WD-R6)");
    check(alert_text::disconnect(CrashWatch::Trouble::Stalled, "", true, 5) ==
              "Mất kết nối: không thấy nhân vật trong game 5 phút (game vẫn chạy). GoBot đang vào lại.",
          "the alert: " + alert_text::disconnect(CrashWatch::Trouble::Stalled, "", true, 5));
    check(alert_text::disconnect(CrashWatch::Trouble::Stalled, "", false, 7) ==
              "Mất kết nối: không thấy nhân vật trong game 7 phút (game vẫn chạy).",
          "and when GoBot is not coming back");
    check(alert_text::disconnect(CrashWatch::Trouble::Gone, "", true) ==
              "Mất kết nối: game đã tắt. GoBot đang vào lại.",
          "the others are as they were");
    CrashWatch::Look stalled;
    stalled.what = CrashWatch::Trouble::Stalled;
    stalled.minutes = 5;
    check(CrashWatch::trouble_vi(stalled) + " — GoBot đóng game rồi vào lại sau 15 giây." ==
              "Game không vào được thế giới 5 phút — GoBot đóng game rồi vào lại sau 15 giây.",
          "the simple screen: " + CrashWatch::trouble_vi(stalled) + " — GoBot đóng game rồi vào lại sau 15 giây.");
    CrashWatch::Look gone, hung, box;
    gone.what = CrashWatch::Trouble::Gone;
    hung.what = CrashWatch::Trouble::Hung;
    box.what = CrashWatch::Trouble::ErrorBox;
    check(CrashWatch::trouble_vi(gone) == "Game đã tắt" && CrashWatch::trouble_vi(hung) == "Game bị treo" &&
              CrashWatch::trouble_vi(box) == "Game hiện bảng lỗi",
          "and the crashes' words are the keeper's as they were");
    check(std::string(CrashWatch::trouble_name(CrashWatch::Trouble::Stalled)).find("nobody") != std::string::npos,
          std::string("in the log: ") + CrashWatch::trouble_name(CrashWatch::Trouble::Stalled));
    check(CrashWatch::next_move(stalled, true, false) == CrashWatch::Move::Restart,
          "a stall is a restart, like a crash");
#ifdef GOBOT_SOURCE_DIR
    const std::string src = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/stall_watch.cpp");
    std::string lower = src;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    check(!src.empty() && lower.find("captcha\"") == std::string::npos &&
              lower.find("captcha/") == std::string::npos,
          "stall_watch.cpp looks for no captcha picture (WD-R7)");
    check(src.find("click_at") == std::string::npos && src.find("key_press") == std::string::npos &&
              src.find("type_text") == std::string::npos && src.find("focus_window") == std::string::npos,
          "and clicks, presses and brings forward nothing: the in-game dialog is only read");
#endif
}

void test_games_window() {
    section("Watchdog: only the game's own window is looked at (GameInstall)");
    // The window GoBot follows can be any window -- picked by hand from a
    // list, or found by its title alone -- and one that is not the game never
    // shows the HUD: counted, its looks would end a healthy game. So only a
    // window of the game's own program, by where it runs from, is looked at.
    // This test's own program stands in for the game, as in the crash watch's
    // tests, and a folder of the test's own for anywhere else.
    wchar_t self_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    GameInstall here_too;
    here_too.programs = { wide_to_utf8(self_path) };
    GameInstall elsewhere;
    elsewhere.programs = { temp_path("gobot_test.exe") };
    const std::uint64_t t0 = 9'000'000;
    const AutoLogin::Settings login;
    const TemplateStore no_pages;

    check(!stall_window_is_the_games(nullptr, "gobot_test.exe", here_too), "no window: none of the game's");

    // A window of this test's own, put up off every screen and never brought
    // forward, for the game's.
    const HINSTANCE self = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = self;
    wc.lpszClassName = "GoBotTestStallWindow";
    RegisterClassA(&wc);        // already there from an earlier call: the same
    const int far_x = GetSystemMetrics(SM_XVIRTUALSCREEN) - 4000;
    const int far_y = GetSystemMetrics(SM_YVIRTUALSCREEN) - 4000;
    HWND w = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "GoBot test window",
                             WS_POPUP, far_x, far_y, 200, 100, nullptr, nullptr, self, nullptr);
    if (w) ShowWindow(w, SW_SHOWNOACTIVATE);
    bool listed = false;
    for (const InputManager::WindowInfo& info : InputManager::windows_of("gobot_test.exe"))
        listed = listed || (w != nullptr && info.hwnd == w);
    if (!listed) {
        std::printf("  [skip] could not put up a window of this test's own here\n");
    } else {
        check(stall_window_is_the_games(w, "gobot_test.exe", here_too),
              "a window of the program installed as the game: the game's own");
        check(!stall_window_is_the_games(w, "gobot_test.exe", elsewhere),
              "the same window with the game installed elsewhere: only called what the game is called, "
              "so not the game's");
        check(!stall_window_is_the_games(w, "Game.exe", here_too),
              "nor when the game's program has another name");

        // The keeper's side: a window that is not the game's is not looked at,
        // and its looks count for nothing however long it shows no HUD.
        StallKeeper other;
        other.set_settings(on(1), exe_relative("configs/templates.ini"));
        other.set_install(elsewhere);
        other.restart(t0);
        CrashWatch::Look out;
        bool any = false, looked = false;
        for (std::uint64_t t = t0; t <= t0 + 3 * kMinute; t += kLookMs) {
            any = any || other.check(w, "gobot_test.exe", false, login, no_pages, t, out, {});
            looked = looked || other.looking_at_the_game();
        }
        check(!any && !looked,
              "a window that is not the game's, for three times stall_min: never looked at, never stalled");

        StallKeeper game;
        game.set_settings(on(1), exe_relative("configs/templates.ini"));
        game.set_install(here_too);
        game.restart(t0);
        game.check(w, "gobot_test.exe", false, login, no_pages, t0, out, {});
        check(game.looking_at_the_game(), "the game's own window: looked at");
        game.check(nullptr, "gobot_test.exe", false, login, no_pages, t0 + kLookMs, out, {});
        check(!game.looking_at_the_game(), "and no window at all: nothing looked at");

        // Off every screen, its HUD's corner is not seen: read off the screen
        // it would be black, which is no HUD, and the look is blind instead.
        std::string why;
        const bool seen = AutoLogin::part_seen(w, StallLooker::kHudCorner, &why);
        check(!seen && why == "off the screen", "the HUD's corner of a window off the screen: not seen, so blind -- " +
                                                    why + " (the 2A tester's problem 11)");
        // And the look the grab's word makes of a frame read off the screen,
        // with the game taken to be in front and nothing over it: blind.
        StallLooker looker;
        looker.load(exe_relative("configs/templates.ini"));
        const bool trusted = stall_frame_trusted(true, false, true, stall_screen_clear(true, true, seen));
        const StallLook l = looker.look_at(plain_frame(30), trusted, false, false, login, no_pages);
        check(!trusted && l.blind, "so its frame, read off the screen, is a blind look, which counts for nothing");
    }
    if (w) DestroyWindow(w);
}

} // namespace

void test_stall(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_rule();
    test_pauses();
    test_eyes();
    test_keeper();
    test_games_window();
    test_texts();
}
