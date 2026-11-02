// gobot_test -- the heavy tabs made lighter: a config file read again only
// when it changes (FileWatch, gobot/file_watch.hpp), the log's change count
// the Log tab copies by (Logger::ring_sequence), and where Dọn rác, Tự đăng
// nhập, the Log tab and the full screen's party and keeper panels use them,
// read off their source.
//
// The rule runs on a clock and file times of the test's own. The one file
// touched is the test's own, in the temp folder; no window is made, nothing
// is drawn, and nothing is read from the desktop or the registry.

#include "gobot/common.hpp"
#include "gobot/file_watch.hpp"
#include "gobot/logger.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

void test_file_watch_rule() {
    section("Heavy tabs: a config file read again only when it changes (FileWatch)");

    constexpr std::uint64_t kT0 = 5'000'000;
    std::uint64_t stamp = 111;
    int asked = 0;
    const auto ask = [&] {
        ++asked;
        return stamp;
    };

    FileWatch w;     // a look a second
    const bool first = w.due(kT0, ask);
    check(first && asked == 1, "the first look reads the file, and asks its time once");

    const bool soon = w.due(kT0 + 999, ask);
    check(!soon && asked == 1, "inside the second: not read, and its time not even asked");

    const bool same = w.due(kT0 + 1000, ask);
    check(!same && asked == 2, "a second on, the time unchanged: asked, not read");

    stamp = 222;
    const bool early = w.due(kT0 + 1500, ask);
    const bool changed = w.due(kT0 + 2000, ask);
    check(!early && changed && asked == 3,
          "written by hand: read at the next look a second on, not before -- and then once");

    const bool after = w.due(kT0 + 3000, ask);
    check(!after, "and not again while it stays as it is");

    w.forget();
    const bool own = w.due(kT0 + 3001, ask);
    check(own, "written by the tab itself (forget): read on the very next frame, whatever the time");

    w.retry();
    const bool held = w.due(kT0 + 3500, ask);
    const bool again = w.due(kT0 + 4001, ask);
    const bool then = w.due(kT0 + 5001, ask);
    check(!held && again && !then,
          "a read that came to nothing (retry): tried again a second later though its time has not moved, "
          "and only that once");

    // A tab left open for ten seconds, drawn sixty times a second.
    FileWatch left_open;
    int looks = 0;
    const auto look = [&] {
        ++looks;
        return std::uint64_t{ 7 };
    };
    int reads = 0;
    for (std::uint64_t t = kT0; t < kT0 + 10'000; t += 16) {
        const bool again_read = left_open.due(t, look);
        if (again_read) ++reads;
    }
    check(reads == 1 && looks >= 9 && looks <= 11,
          "a tab open for ten seconds at sixty frames a second: the file read once, its time asked about "
          "once a second (" + std::to_string(looks) + " times)");

    FileWatch quick(250);
    int quick_looks = 0;
    const auto quick_look = [&] {
        ++quick_looks;
        return std::uint64_t{ 7 };
    };
    for (std::uint64_t t = kT0; t < kT0 + 1'000; t += 16) (void)quick.due(t, quick_look);
    check(quick_looks >= 4 && quick_looks <= 5, "and with a gap of its own -- 250 ms, the routines panel's -- by that");
}

void test_file_write_stamp() {
    section("Heavy tabs: a file's last-write time (file_write_stamp)");

    wchar_t dir[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) {
        skip("no temp folder to make a file in\n");
        return;
    }
    const std::string path =
        wide_to_utf8(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "_file_watch.ini";
    const std::wstring wide = utf8_to_wide(path);
    DeleteFileW(wide.c_str());
    check(file_write_stamp(path) == 0 && file_write_stamp("") == 0, "no such file, or no name at all: 0");

    {
        std::ofstream out(wide.c_str(), std::ios::binary);
        out << "[Login]\r\nchannel_index=0\r\n";
    }
    // Set rather than waited for: a write a moment after the last can carry
    // the same time, and a test that sleeps to tell them apart is a slow one.
    const auto set_time = [&wide](std::uint64_t t) {
        HANDLE h = CreateFileW(wide.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        FILETIME ft{};
        ft.dwLowDateTime = static_cast<DWORD>(t & 0xFFFFFFFFull);
        ft.dwHighDateTime = static_cast<DWORD>(t >> 32);
        const BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
        return ok != FALSE;
    };
    // Some time in 2024, and a day after it, in Windows' 100 ns steps.
    constexpr std::uint64_t kWritten = 133'500'000'000'000'000ull;
    constexpr std::uint64_t kDay = 864'000'000'000ull;

    const bool set_first = set_time(kWritten);
    const std::uint64_t first = file_write_stamp(path);
    check(set_first && first == kWritten, "a file: the time it was last written, as Windows keeps it");

    FileWatch w(0);  // no gap: every call looks, so no clock is needed
    const bool taken = w.due(path);
    const bool unchanged = w.due(path);
    const bool set_later = set_time(kWritten + kDay);
    const bool edited = w.due(path);
    check(taken && !unchanged && set_later && edited,
          "watched by its path: read first, not again while it is as it was, read again once it is written");

    DeleteFileW(wide.c_str());
    const bool gone = w.due(path);
    check(gone && file_write_stamp(path) == 0, "and once more when it is gone, its time then 0");
}

void test_log_sequence() {
    section("Heavy tabs: the log's change count, which the Log tab copies by (Logger::ring_sequence)");

    // The runner keeps the log at Error so its output stays readable; a Warn
    // line would be dropped before the ring ever saw it. Let Warn through for
    // this test only, and put the level back at the end.
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_level(LogLevel::Warn);
    log.set_ring_capacity(50);
    log.clear_ring();
    const std::uint64_t before = log.ring_sequence();
    log.write(LogLevel::Warn, "gobot_test: a line for the log's change count");
    const std::uint64_t written = log.ring_sequence();
    bool kept = false;
    for (const LogLine& l : log.ring())
        if (l.text.find("gobot_test: a line for the log's change count") != std::string::npos) kept = true;
    check(written > before && kept, "a line kept moves it on, and the line is in ring() by then");

    log.clear_ring();
    const std::uint64_t cleared = log.ring_sequence();
    check(cleared > written, "the ring cleared moves it on");

    log.set_ring_capacity(0);
    const std::uint64_t cut = log.ring_sequence();
    log.write(LogLevel::Warn, "gobot_test: a line no ring keeps");
    const std::uint64_t unkept = log.ring_sequence();
    log.set_level(was);
    check(cut > cleared && unkept == cut,
          "the ring cut to nothing moves it on; with no ring kept, a line written leaves it where it was");
}

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// From `head` to the first closing brace at the start of a line after it:
/// the function's body, as the source has it.
std::string body(const std::string& text, const std::string& head) {
    const std::size_t at = text.find(head);
    if (at == std::string::npos) return {};
    const std::size_t end = text.find("\n}", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
#endif

void test_tabs_wiring() {
    section("Heavy tabs: Dọn rác, Tự đăng nhập, Log and the party and keeper panels (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    if (gui.empty() || view.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto npos = std::string::npos;

    const std::string region = body(gui, "GuiApp::FrameView GuiApp::draw_frame_region(");
    const std::size_t visible = region.find("const bool visible = ImGui::IsRectVisible(ImVec2(w * scale, h * scale));");
    const std::size_t plot = region.find("for (int y = 0; visible && y < h; y += step) {");
    const std::size_t button = region.find("ImGui::InvisibleButton(id, ImVec2(w * scale, h * scale));");
    check(visible != npos && plot != npos && button != npos && visible < plot && plot < button &&
              region.find("view.drawn = true;", button) != npos,
          "gui_app.cpp: a picture out of view is not plotted, and its button -- the same id and size -- is "
          "made all the same, so Dọn rác's clicks and tooltips are as they were");

    const std::string tab = body(view, "void GuiApp::simple_login_tab() {");
    const std::size_t watch = tab.find("if (login_tab_watch_.due(path)) {");
    const std::size_t parse = tab.find("fresh.load(path)");
    const std::size_t write = tab.find("IniFile::set_in_place(path, \"Login\", key, i);");
    const std::size_t forget = write == npos ? npos : tab.find("login_tab_watch_.forget();", write);
    check(watch != npos && parse != npos && watch < parse && tab.find("login.load(path)") == npos &&
              forget != npos && forget - write < 300 &&
              tab.find("login_tab_launcher_ok_ = login.launcher_found();") != npos &&
              tab.find("FileWatch::kGapMs") != npos,
          "simple_view.cpp: Tự đăng nhập reads configs/login.ini when it changes, forgets it after its own "
          "write, and asks where VTCZone is once a second");

    const std::string look = body(view, "void GuiApp::simple_login_look() {");
    const std::size_t minimised = look.find("IsIconic(static_cast<HWND>(main_hwnd_))");
    const std::size_t start = look.find("login_look_ = std::async(");
    check(minimised != npos && start != npos && minimised < start &&
              look.find("ScreenCapture& capture = login_capture_;") != npos &&
              look.find("ScreenCapture capture;") == npos,
          "simple_view.cpp: no login look starts while GoBot is minimised, and one capture of VTCZone is kept "
          "from look to look");

    const std::string panel = body(gui, "void GuiApp::draw_log_panel() {");
    const std::size_t seq = panel.find("logs.ring_sequence()");
    const std::size_t copy = panel.find("log_copy_ = logs.ring();");
    check(seq != npos && copy != npos && seq < copy &&
              panel.find("now - log_copied_ms_ >= kLogCopyGapMs") != npos &&
              panel.find("ImGuiListClipper clipper;") != npos && panel.find("Logger::instance().ring()") == npos,
          "gui_app.cpp: the Log tab copies the ring only when a line went in, at most every 250 ms, and draws "
          "only the rows in view");

    const std::string party = body(gui, "void GuiApp::draw_party_panel() {");
    const std::string keeper = body(gui, "void GuiApp::draw_keeper_panel() {");
    check(party.find("if (party_panel_watch_.due(path)) {") != npos && party.find("rules.load(path)") == npos &&
              party.find("party_panel_watch_.forget();") != npos &&
              keeper.find("if (keeper_panel_app_watch_.due(path)) {") != npos &&
              keeper.find("if (keeper_panel_login_watch_.due(login_path)) {") != npos,
          "gui_app.cpp: the full screen's party and keeper panels read party.ini, app.ini and login.ini when "
          "they change, not on every frame");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_tabs(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_file_watch_rule();
    test_file_write_stamp();
    test_log_sequence();
    test_tabs_wiring();
}
