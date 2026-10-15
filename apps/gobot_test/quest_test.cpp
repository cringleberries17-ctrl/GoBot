// gobot_test -- quick quest.
//
// GoC's own pictures of the Nhiệm Vụ window pasted into frames, a pretend game
// whose screen is drawn afresh from whatever keys and clicks reached it, and a
// pretend clock for the schedule. No game, and nothing is pressed anywhere.

#include "gobot/quick_quest.hpp"
#include "gobot/alerts.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
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

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

bool starts_with(const std::string& s, const std::string& head) { return s.rfind(head, 0) == 0; }

std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    // In this run's own folder, which main.cpp makes and removes: see temp_file there.
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

/// Where GoC's quest pictures are: staged beside this test, else GoC's folder.
std::string picture_dir() {
    for (const std::string& d : { TemplateStore::default_dir(), TemplateStore::client_images() })
        if (!d.empty() && exists(d + "\\quest\\1\\Dlg_Quest_Neo.png") &&
            exists(d + "\\quest\\0\\Dlg_Quest_Neo.png") && exists(d + "\\inven\\0\\quest.png"))
            return d;
    return {};
}

Frame raw(const std::string& dir, const std::string& name) {
    std::string file = name;
    std::replace(file.begin(), file.end(), '/', '\\');
    Frame f;
    load_png(dir + "\\" + file + ".png", f);
    return f;
}

/// `src` over `dst` at `x`, `y`, by its transparency, as the game draws it.
void paste(Frame& dst, const Frame& src, int x, int y) {
    for (int sy = 0; sy < src.height; ++sy) {
        const int dy = y + sy;
        if (dy < 0 || dy >= dst.height) continue;
        for (int sx = 0; sx < src.width; ++sx) {
            const int dx = x + sx;
            if (dx < 0 || dx >= dst.width) continue;
            const std::uint8_t* s = &src.pixels[(static_cast<std::size_t>(sy) * src.width + sx) * 4];
            std::uint8_t* d = &dst.pixels[(static_cast<std::size_t>(dy) * dst.width + dx) * 4];
            const int a = s[3];
            for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>((s[c] * a + d[c] * (255 - a)) / 255);
            d[3] = 255;
        }
    }
}

/// Something like the game's scenery: a slow gradient with blocks of noise
/// over it, so that nothing in it is flat and nothing in it is a window.
Frame scene(int w, int h, std::uint32_t seed) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    std::mt19937 rng(seed);
    std::vector<int> block(static_cast<std::size_t>((w / 16 + 1) * (h / 16 + 1)));
    for (int& b : block) b = static_cast<int>(rng() % 90);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int b = block[static_cast<std::size_t>((y / 16) * (w / 16 + 1) + x / 16)];
            std::uint8_t* p = &f.pixels[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(40 + (x * 80) / w + b);
            p[1] = static_cast<std::uint8_t>(60 + (y * 70) / h + b / 2);
            p[2] = static_cast<std::uint8_t>(30 + b);
            p[3] = 255;
        }
    return f;
}

/// Noise over part of a frame.
void scribble(Frame& f, const ScreenRect& r, std::uint32_t seed) {
    std::mt19937 rng(seed);
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x) {
            std::uint8_t* p = &f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4];
            p[0] = p[1] = p[2] = static_cast<std::uint8_t>(rng() % 256);
        }
}

/// The frames saved from the live client beside this test -- where the package
/// puts it, beside gobot_gui: frame.png and the trash cleaner's debug folder.
std::vector<std::string> saved_frames(bool trash_debug) {
    std::vector<std::string> out;
    const std::string dir = exe_relative("");
    if (!trash_debug) {
        if (exists(dir + "frame.png")) out.push_back(dir + "frame.png");
        return out;
    }
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "trash_debug\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(dir + "trash_debug\\" + fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

/// FNV-1a over a file's bytes.
std::uint64_t file_hash(const std::string& path) {
    const std::string bytes = read_all(path);
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

/// Whether `path` is one of the live frames this test was written against --
/// frame.png and the trash cleaner's seventeen, as they were saved -- each
/// looked at and showing no quest window. Only these are counted on: any
/// other frame beside the test, the player's own after quick quest has run,
/// may well show the window, so it is looked at and reported, not counted.
bool known_frame(const std::string& path) {
    static const std::uint64_t kKnown[] = {
        0x55f4d65f3cc529c7ull,      // frame.png
        0x4a4b30a67d3a4c04ull, 0xe8be9a147ccf183aull, 0x00b1e5b2447e43a3ull, 0xb9c07a06947287faull,
        0x2b3daeec725beb4aull, 0x34e0c99b37961fa5ull, 0x2bb38db54d5417baull, 0xaf08d5f618f0f6feull,
        0xf26e1a095d3b2214ull, 0x4dc52622d875ebd3ull, 0x9b5aa22a75664aedull, 0xc711011d0e74439bull,
        0x47f2f607802468b7ull, 0xdd5798f68bc6261eull, 0xe29edc96c0df073full, 0x091afd231122f428ull,
        0xc8f5b8e97bc14b87ull,      // the trash cleaner's, 9 to 13 October
    };
    const std::uint64_t h = file_hash(path);
    return std::find(std::begin(kKnown), std::end(kKnown), h) != std::end(kKnown);
}

/// Everything a pretend game needs to draw the window, from GoC's pictures.
struct Art {
    std::string   dir;
    QuestSettings settings;
    QuestPictures pictures;
    Frame dialog, dialog_en, radio, box_on, box_off, title_bar;
    std::array<Frame, 4> picked;        ///< each tab in its other look
    bool ok = false;
};

const Art& art() {
    static Art a = [] {
        Art x;
        x.dir = picture_dir();
        if (x.dir.empty()) return x;
        x.pictures.load_dir(x.dir, x.settings);
        x.dialog = raw(x.dir, "quest/1/Dlg_Quest_Neo");
        x.dialog_en = raw(x.dir, "quest/0/Dlg_Quest_Neo");
        x.radio = raw(x.dir, "quest/1/radio_LetterQuest_Auto.spr/0");
        x.box_on = raw(x.dir, "quest/1/btn_QuestNeo_Radio.spr/0");
        x.box_off = raw(x.dir, "quest/1/btn_QuestNeo_Radio.spr/1");
        x.title_bar = raw(x.dir, "inven/0/quest");
        for (int i = 0; i < 4; ++i)
            x.picked[static_cast<std::size_t>(i)] =
                raw(x.dir, "quest/1/" + x.settings.tabs[static_cast<std::size_t>(i)] + "/1");
        x.ok = x.pictures.ready() && x.dialog.valid() && x.dialog_en.valid() && x.radio.valid() &&
               x.box_on.valid() && x.box_off.valid() && x.title_bar.valid() &&
               std::all_of(x.picked.begin(), x.picked.end(), [](const Frame& f) { return f.valid(); });
        return x;
    }();
    return a;
}

bool have_art() {
    if (art().ok) return true;
    std::printf("  [skip] GoC's quest pictures are not installed here\n");
    return false;
}

/// A pretend game with the Nhiệm Vụ window in it, for a round to drive. Its
/// screen is drawn afresh at every look from what the keys and clicks did.
struct PretendQuest {
    Frame base = scene(1280, 720, 11);
    bool english = false;
    int  wx = 300, wy = 120;            ///< where the window opens
    int  ax = 450, ay = 380;            ///< Tự Động, from the window
    bool open = false;
    int  tab = 0;                       ///< 0 NV Chính .. 3 NV Phụ
    bool on = false;                    ///< Tự Động's box
    bool key_opens = true;
    bool key_closes = true;             ///< false: the key opens the window but never shuts it
    bool has_auto = true;
    bool click_takes = true;
    bool smudged_box = false;           ///< half the box hidden under noise
    std::uint32_t key = 85;

    std::vector<std::uint32_t> keys;
    std::vector<std::pair<int, int>> clicks, moves;
    std::uint64_t clock = 5'000'000;
    /// Asked before every key, click and move; true stops the round.
    std::function<bool(const PretendQuest&)> stop_when;

    int inputs() const { return static_cast<int>(keys.size() + clicks.size()); }

    Frame screen() const {
        Frame f = base;
        if (!open) return f;
        const Art& a = art();
        paste(f, english ? a.dialog_en : a.dialog, wx, wy);
        // The tab that is picked, in its other look; the rest as the
        // window's picture draws them.
        if (!english) paste(f, a.picked[static_cast<std::size_t>(tab)], wx + 10 + 76 * tab, wy + 32);
        if (tab == 3 && has_auto && !english) {
            paste(f, a.radio, wx + ax, wy + ay);
            if (on) paste(f, a.box_on, wx + ax + 4, wy + ay + 8);
            if (smudged_box) scribble(f, ScreenRect{ wx + ax + 4, wy + ay + 8, 7, 14 }, 5);
        }
        return f;
    }

    QuestRound::Hands hands() {
        QuestRound::Hands h;
        h.grab = [this](Frame& f) {
            f = screen();
            return true;
        };
        h.key = [this](std::uint32_t vk, int) {
            keys.push_back(vk);
            if (vk == key && key_opens && (!open || key_closes)) {
                open = !open;
                if (open) tab = 0;
            }
            return true;
        };
        h.click = [this](int x, int y) {
            clicks.emplace_back(x, y);
            if (!open) return true;
            if (x >= wx + 238 && x < wx + 312 && y >= wy + 32 && y < wy + 60) tab = 3;
            else if (tab == 3 && has_auto && click_takes && x >= wx + ax + 4 && x < wx + ax + 18 &&
                     y >= wy + ay + 8 && y < wy + ay + 22)
                on = !on;
            return true;
        };
        h.move = [this](int x, int y) {
            moves.emplace_back(x, y);
            return true;
        };
        h.now = [this] { return clock; };
        h.sleep = [this](int ms) { clock += static_cast<std::uint64_t>(ms); };
        h.stop = [this] { return stop_when && stop_when(*this); };
        return h;
    }

    QuestResult run(QuestSettings s = QuestSettings{}) {
        QuestRound round(art().pictures, s, hands());
        return round.run();
    }
};

bool near_point(const std::pair<int, int>& p, int x, int y, int slack) {
    return std::abs(p.first - x) <= slack && std::abs(p.second - y) <= slack;
}

// ------------------------------------------------------------------ settings

bool same(const ScreenRect& a, const ScreenRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool same_settings(const QuestSettings& a, const QuestSettings& b) {
    return a.enabled == b.enabled && a.interval_min == b.interval_min &&
           a.first_delay_s == b.first_delay_s && a.quest_key == b.quest_key &&
           a.max_failures == b.max_failures && a.back_off == b.back_off && a.skin == b.skin &&
           a.title_rows == b.title_rows && a.title_inset == b.title_inset &&
           same(a.tabs_area, b.tabs_area) && a.tab_pitch == b.tab_pitch &&
           a.tab_click_x == b.tab_click_x && a.tab_click_y == b.tab_click_y &&
           same(a.auto_area, b.auto_area) && a.auto_click_x == b.auto_click_x &&
           a.auto_click_y == b.auto_click_y && same(a.auto_box, b.auto_box) &&
           a.hover_dx == b.hover_dx && same(a.forbid_abandon, b.forbid_abandon) &&
           same(a.forbid_hide_low, b.forbid_hide_low) && same(a.forbid_buttons, b.forbid_buttons) &&
           a.window == b.window &&
           a.window_en == b.window_en && a.auto_option == b.auto_option && a.box_on == b.box_on &&
           a.box_off == b.box_off && a.tabs == b.tabs && a.window_score == b.window_score &&
           a.tab_score == b.tab_score && a.auto_score == b.auto_score &&
           a.state_score == b.state_score && a.state_margin == b.state_margin &&
           a.open_hold_ms == b.open_hold_ms && a.close_hold_ms == b.close_hold_ms &&
           a.open_wait_ms == b.open_wait_ms && a.open_look_ms == b.open_look_ms &&
           a.tab_wait_ms == b.tab_wait_ms && a.click_wait_ms == b.click_wait_ms &&
           a.hover_wait_ms == b.hover_wait_ms && a.close_wait_ms == b.close_wait_ms &&
           a.round_limit_ms == b.round_limit_ms;
}

void test_settings() {
    section("Quick quest: configs/quest.ini");

    const QuestSettings d;
    check(!d.enabled && d.interval_min == 5 && d.first_delay_s == 40 && d.quest_key == 85 &&
              d.max_failures == 3 && d.back_off && d.skin == QuestSettings::Skin::Auto,
          "defaults: off, every 5 min, first after 40 s, key U (85), 3 failures, back-off, skin auto");
    check(d.tab_click_x == 274 && d.tab_click_y == 47 && d.auto_click_x == 11 && d.auto_click_y == 15 &&
              same(d.auto_area, ScreenRect{ 0, 360, 672, 100 }) &&
              same(d.auto_box, ScreenRect{ 4, 8, 14, 14 }) && d.hover_dx == 14,
          "defaults: GoC's points -- NV Phu at +274,47, Tu Dong at +11,15, its box at +4,8, 14 px hover");
    check(same(d.forbid_abandon, ScreenRect{ 296, 417, 80, 30 }) &&
              same(d.forbid_hide_low, ScreenRect{ 523, 37, 14, 14 }),
          "defaults: Huy Nhiem Vu and the hide-low-level checkbox are where GoC's picture has them");
    check(same(d.forbid_buttons, ScreenRect{ 283, 412, 103, 41 }),
          "defaults: the quick quest's buttons kept out of where GoC reads them, window+283,412 103x41");
    check(d.window_score == 0.85f && d.auto_score == 0.80f && d.state_score == 0.80f &&
              d.state_margin == 0.10f && d.open_wait_ms == 500 && d.open_look_ms == 1500 &&
              d.tab_wait_ms == 500 && d.click_wait_ms == 300 && d.hover_wait_ms == 200 &&
              d.close_wait_ms == 1000 && d.open_hold_ms == 50 && d.close_hold_ms == 100,
          "defaults: the BA's scores and GoC's waits");

    // The file that ships -- from the source tree, which is only where this
    // was built.
    const std::string shipped = std::string(GOBOT_SOURCE_DIR) + "/configs/quest.ini";
    const std::string bytes = read_all(shipped);
    if (bytes.empty()) {
        std::printf("  [skip] the source tree's configs/quest.ini is not here\n");
    } else {
        QuestSettings s;
        check(s.load(shipped), "the shipped configs/quest.ini loads");
        check(!s.enabled, "... with quick quest off");
        check(same_settings(s, d), "... and every value in it is the code's default");
        check(bytes.compare(0, 3, "\xEF\xBB\xBF") != 0 && bytes.find('\r') == std::string::npos,
              "... in UTF-8 without a BOM, with LF line ends");
    }

    // No file.
    {
        QuestSettings none;
        none.interval_min = 7;
        const std::string path = temp_path("gobot_test_quest_none.ini");
        DeleteFileA(path.c_str());
        check(!none.load(path) && none.interval_min == 7, "no file: not loaded, and nothing changed");
    }

    // Key by key, with the comments kept, and back again to the same bytes.
    {
        const std::string path = temp_path("gobot_test_quest_roundtrip.ini");
        write_all(path, bytes);
        const struct { const char* sec; const char* key; const char* value; } kChanges[] = {
            { "Quest", "enabled", "true" },        { "Quest", "interval_min", "12" },
            { "Quest", "first_delay_s", "25" },    { "Quest", "quest_key", "81" },
            { "Quest", "max_failures", "4" },      { "Quest", "back_off", "false" },
            { "Quest", "skin", "vi" },             { "Layout", "tab_click", "270,45" },
            { "Scores", "window", "0.9" },         { "Timing", "open_wait_ms", "600" },
            { "Layout", "forbid_buttons", "280,410,110,45" },
        };
        bool all = true;
        for (const auto& c : kChanges) all = QuestSettings::save_key(path, c.sec, c.key, c.value) && all;
        QuestSettings back;
        check(all && back.load(path), "every key written by save_key, and the file read back");
        check(back.enabled && back.interval_min == 12 && back.first_delay_s == 25 &&
                  back.quest_key == 81 && back.max_failures == 4 && !back.back_off &&
                  back.skin == QuestSettings::Skin::Vi && back.tab_click_x == 270 &&
                  back.tab_click_y == 45 && back.window_score == 0.9f && back.open_wait_ms == 600 &&
                  same(back.forbid_buttons, ScreenRect{ 280, 410, 110, 45 }),
              "... with every value as written");
        const std::string after = read_all(path);
        std::size_t comments_before = 0, comments_after = 0;
        for (std::size_t at = 0; (at = bytes.find("\n#", at)) != std::string::npos; ++at) ++comments_before;
        for (std::size_t at = 0; (at = after.find("\n#", at)) != std::string::npos; ++at) ++comments_after;
        check(comments_before > 20 && comments_after == comments_before &&
                  after.find('\r') == std::string::npos && after.compare(0, 3, "\xEF\xBB\xBF") != 0,
              "... every comment kept, still LF and without a BOM");
        const struct { const char* sec; const char* key; const char* value; } kBack[] = {
            { "Quest", "enabled", "false" },       { "Quest", "interval_min", "5" },
            { "Quest", "first_delay_s", "40" },    { "Quest", "quest_key", "85" },
            { "Quest", "max_failures", "3" },      { "Quest", "back_off", "true" },
            { "Quest", "skin", "auto" },           { "Layout", "tab_click", "274,47" },
            { "Scores", "window", "0.85" },        { "Timing", "open_wait_ms", "500" },
            { "Layout", "forbid_buttons", "283,412,103,41" },
        };
        for (const auto& c : kBack) QuestSettings::save_key(path, c.sec, c.key, c.value);
        check(read_all(path) == bytes, "set back key by key, the file is the shipped one, byte for byte");
        DeleteFileA(path.c_str());
    }

    // A missing file is made, off; an existing one is never emptied first.
    {
        const std::string path = temp_path("gobot_test_quest_new.ini");
        DeleteFileA(path.c_str());
        QuestSettings made;
        check(QuestSettings::save_key(path, "Quest", "interval_min", "9") && made.load(path) &&
                  !made.enabled && made.interval_min == 9,
              "save_key on a missing file makes one, with quick quest off");
        write_all(path, "# mine\n[Quest]\nenabled=true\nquest_key=81\n");
        QuestSettings::save_key(path, "Quest", "interval_min", "7");
        const std::string kept = read_all(path);
        check(starts_with(kept, "# mine\n[Quest]\nenabled=true\n") && contains(kept, "interval_min=7") &&
                  contains(kept, "quest_key=81"),
              "save_key on an existing file changes one key and keeps the rest");
        DeleteFileA(path.c_str());
    }

    // Edited by hand: a BOM, any case, CRLF, numbers out of range, nonsense.
    {
        const std::string path = temp_path("gobot_test_quest_hand.ini");
        write_all(path, "\xEF\xBB\xBF[QUEST]\r\nEnabled = TRUE\r\nInterval_Min=600\r\nquest_key=13\r\n"
                        "first_delay_s=-5\r\n[Scores]\r\nwindow=0,5\r\nstate=abc\r\n[Layout]\r\n"
                        "tab_click=abc\r\nauto_area=1,2,3\r\n");
        QuestSettings h;
        check(h.load(path), "a hand-edited file with a BOM, upper case and CRLF is read");
        check(h.enabled && h.interval_min == 60 && h.first_delay_s == 0,
              "... TRUE is on, 600 minutes is brought down to 60, -5 s up to 0");
        check(h.quest_key == 85, "... Enter (13) is refused as the quest key: it answers dialogs");
        check(h.window_score == 0.60f && h.state_score == 0.80f,
              "... a score of 0,5 is read and brought up to 0.60; nonsense keeps the default");
        check(h.tab_click_x == 274 && same(h.auto_area, ScreenRect{ 0, 360, 672, 100 }),
              "... a point or area that does not read keeps GoC's");
        write_all(path, "[Quest]\nquest_key=U\n");
        QuestSettings u;
        check(u.load(path) && u.quest_key == 85, "a key may be written by its name: U is 85");
        write_all(path, "[Quest]\nquest_key=Esc\n");
        QuestSettings esc;
        check(esc.load(path) && esc.quest_key == 85, "Esc is refused as the quest key");
        write_all(path, "# nothing\n[Other]\nenabled=true\n");
        QuestSettings other;
        check(!other.load(path), "a file without [Quest] is not taken");
        DeleteFileA(path.c_str());
    }
    check(QuestSettings::clamp_interval(0) == 1 && QuestSettings::clamp_interval(61) == 60 &&
              QuestSettings::clamp_interval(5) == 5,
          "the interval is kept within 1 to 60 minutes");
}

// ------------------------------------------------------------------ pictures

void test_pictures() {
    section("Quick quest: pictures");
    if (!have_art()) return;
    const Art& a = art();
    const QuestPictures& p = a.pictures;
    check(p.ready(), "the Vietnamese client's pictures load from " + a.dir);
    check(p.knows_en(), "... and the English client's, to tell its window apart");
    check(p.window_w == 672 && p.window_h == 460, "the window is 672 x 460");
    bool opaque = p.title.valid();
    for (std::size_t i = 3; i < p.title.pixels.size(); i += 4)
        opaque = opaque && p.title.pixels[i] >= TemplateMatcher::kOpaque;
    check(p.title.width == 652 && p.title.height == 32 && opaque,
          "the title bar is cut 652 x 32, with nothing see-through left in it");
    bool tabs_ok = p.tabs.size() == 4 && p.tabs_en.size() == 4;
    for (const auto& looks : p.tabs)
        tabs_ok = tabs_ok && looks[0].width == 74 && looks[0].height == 28 && looks[1].width == 74;
    check(tabs_ok, "four tabs, each in its two looks, for both clients");
    bool hole = p.auto_option.width == 80 && p.auto_option.height == 30;
    for (int y = 8; y < 22 && hole; ++y)
        for (int x = 4; x < 18; ++x)
            hole = hole && p.auto_option.pixels[(static_cast<std::size_t>(y) * 80 + x) * 4 + 3] == 0;
    check(hole, "Tu Dong is looked for without its box, which is read on its own");
    check(p.box_on.width == 14 && p.box_off.width == 14, "the box in both its looks, 14 x 14");

    // From a store, as a test or another caller has them.
    {
        TemplateStore store;
        const std::vector<std::string> names = {
            a.settings.window, a.settings.auto_option, a.settings.box_on, a.settings.box_off };
        for (const std::string& n : names) store.put(n, raw(a.dir, n));
        for (const std::string& t : a.settings.tabs)
            for (const char* look : { "/0", "/1" })
                store.put("quest/1/" + t + look, raw(a.dir, "quest/1/" + t + look));
        QuestPictures q;
        q.load(store, a.settings);
        check(q.ready() && !q.knows_en(), "from a store: ready, and without the English pictures it "
                                          "just cannot tell that client apart");
    }

    // Missing ones are named, and the feature says which.
    {
        QuestSettings odd = a.settings;
        odd.auto_option = "quest/1/no_such_picture";
        odd.box_on = "quest/9/nothing";
        QuestPictures q;
        check(!q.load_dir(a.dir, odd) && !q.ready() && q.missing.size() == 2 &&
                  q.missing[0] == odd.auto_option && q.missing[1] == odd.box_on,
              "missing pictures are named");
        check(quest_missing_vi(q) == "Thiếu hình templates/quest/1/no_such_picture.png, "
                                     "templates/quest/9/nothing.png — cài lại GoBot.",
              "... and said in Vietnamese: " + quest_missing_vi(q));
        QuestPictures none;
        check(!none.load_dir(temp_path("gobot_test_no_pictures_here"), a.settings) &&
                  none.missing.size() == 12,
              "an empty folder: all twelve needed pictures named");
    }
    {
        QuestPictures best;
        std::string from;
        const bool loaded = best.load_best(a.settings, &from);
        check(loaded && best.ready() && !from.empty(), "load_best finds them, in " + from);
    }
}

// -------------------------------------------------------------------- window

void test_window() {
    section("Quick quest: finding the Nhiệm Vụ window (Q-A1)");
    if (!have_art()) return;
    const Art& a = art();
    const QuestSettings& s = a.settings;
    const QuestPictures& p = a.pictures;

    std::vector<std::pair<std::string, Frame>> bases;
    bases.emplace_back("a pretend scene", scene(1280, 720, 3));
    const auto frames = saved_frames(false);
    Frame shot;
    bool shot_known = false;
    if (!frames.empty() && load_png(frames.front(), shot) && shot.width == 1280 && shot.height == 720) {
        shot_known = known_frame(frames.front());
        // Looked at alone first: one of the player's own may show the window
        // itself, which is no failure of anything, and no base to paste
        // another into either.
        const QuestWindow alone = find_quest_window(shot, p, s);
        if (shot_known) {
            check(!alone.found, fmt("frame.png alone: no window (best title bar %.3f)",
                                    static_cast<double>(alone.best_title)));
        } else {
            std::printf("  [info] frame.png is not one this test knows: %s (best title bar %.3f)\n",
                        alone.found ? "a quest window is in it" : "no quest window in it",
                        static_cast<double>(alone.best_title));
        }
        if (!alone.found) bases.emplace_back("frame.png", shot);
    } else {
        std::printf("  [skip] no frame.png from the live client here; a pretend scene only\n");
    }

    for (const auto& [name, base] : bases) {
        for (const auto& [x, y] : { std::pair<int, int>{ 300, 120 }, std::pair<int, int>{ 600, 250 } }) {
            Frame f = base;
            paste(f, a.dialog, x, y);
            const QuestWindow w = find_quest_window(f, p, s);
            check(w.found && std::abs(w.at.x - x) <= 1 && std::abs(w.at.y - y) <= 1 && !w.english,
                  fmt("quest/1's window pasted into %s at %d,%d: found at %d,%d (title %.3f, tabs "
                      "%.3f), Vietnamese",
                      name.c_str(), x, y, w.at.x, w.at.y, static_cast<double>(w.score),
                      static_cast<double>(w.tabs)));
            Frame g = base;
            paste(g, a.dialog_en, x, y);
            const QuestWindow e = find_quest_window(g, p, s);
            check(e.found && e.english && std::abs(e.at.x - x) <= 1 && std::abs(e.at.y - y) <= 1,
                  fmt("quest/0's window at %d,%d in %s: found at %d,%d, English (vi tabs %.3f, en "
                      "tabs %.3f)",
                      x, y, name.c_str(), e.at.x, e.at.y, static_cast<double>(e.tabs),
                      static_cast<double>(e.tabs_en)));
        }
    }

    // Whichever tab is picked is drawn in its other look -- and the one under
    // the pointer may be too. Each tab is matched in either look.
    {
        Frame f = bases.front().second;
        paste(f, a.dialog, 300, 120);
        paste(f, a.picked[1], 300 + 10 + 76, 120 + 32);
        const QuestWindow one = find_quest_window(f, p, s);
        paste(f, a.picked[3], 300 + 10 + 76 * 3, 120 + 32);
        const QuestWindow two = find_quest_window(f, p, s);
        check(one.found && two.found && !one.english && !two.english,
              fmt("found with Đang Tiến Hành picked (tabs %.3f), and with NV Phụ lit as well (%.3f)",
                  static_cast<double>(one.tabs), static_cast<double>(two.tabs)));
    }

    // Not the window.
    if (bases.size() > 1) {
        Frame f = bases[1].second;
        paste(f, a.title_bar, 300, 120);
        const QuestWindow bar = find_quest_window(f, p, s);
        const std::string what = fmt("GoC's inven/0/quest.png title bar pasted alone into frame.png: not "
                                     "taken for the window (title %.3f)",
                                     static_cast<double>(bar.best_title));
        if (shot_known) check(!bar.found, what);
        else std::printf("  [info] %s: %s\n", what.c_str(), bar.found ? "taken for one" : "so");
    }
    {
        Frame f = bases.front().second;
        paste(f, a.title_bar, 300, 120);
        check(!find_quest_window(f, p, s).found, "the same title bar on the pretend scene: not the window");
        Frame g = bases.front().second;
        Frame top = a.dialog;
        top.height = 32;
        top.pixels.resize(static_cast<std::size_t>(top.width) * 32 * 4);
        paste(g, top, 300, 120);
        check(!find_quest_window(g, p, s).found,
              "the window's own title bar with no tabs under it: not the window");
    }
    const auto debug = saved_frames(true);
    if (debug.empty()) {
        std::printf("  [skip] no trash_debug frames from the live client here\n");
    } else {
        // Counted on for the frames this test knows; any others reported.
        int found = 0, read = 0, other = 0, other_found = 0;
        float best = 0.0f;
        for (const std::string& path : debug) {
            Frame f;
            if (!load_png(path, f)) continue;
            const QuestWindow w = find_quest_window(f, p, s);
            if (!known_frame(path)) {
                ++other;
                if (w.found) ++other_found;
                continue;
            }
            ++read;
            if (w.found) ++found;
            best = std::max(best, w.best_title);
        }
        if (read > 0)
            check(found == 0, fmt("%d trash_debug frames from the live client: no window in any (best title "
                                  "bar %.3f)",
                                  read, static_cast<double>(best)));
        if (other > 0)
            std::printf("  [info] %d other trash_debug frames, not ones this test knows: a quest window in "
                        "%d\n",
                        other, other_found);
    }
}

// ------------------------------------------------------------------ Tự Động

void test_auto() {
    section("Quick quest: reading Tự Động (Q-A2)");
    if (!have_art()) return;
    const Art& a = art();
    const QuestSettings& s = a.settings;
    const QuestPictures& p = a.pictures;
    const ScreenRect win{ 300, 120, 672, 460 };
    Frame base = scene(1280, 720, 4);
    paste(base, a.dialog, win.x, win.y);
    paste(base, a.picked[3], win.x + 10 + 76 * 3, win.y + 32);

    Frame f = base;
    paste(f, a.radio, win.x + 450, win.y + 380);
    const AutoOption off = find_auto_option(f, win, p, s);
    check(off.found && std::abs(off.at.x - 750) <= 1 && std::abs(off.at.y - 500) <= 1,
          fmt("Tu Dong pasted at window+450,380: found at %d,%d (%.3f)", off.at.x, off.at.y,
              static_cast<double>(off.score)));
    check(off.state == AutoState::Off, fmt("... its box read as off (on %.3f / off %.3f)",
                                           static_cast<double>(off.on), static_cast<double>(off.off)));
    check(off.click_x == off.at.x + 11 && off.click_y == off.at.y + 15, "... the click point is match+11,15");

    Frame g = f;
    paste(g, a.box_on, 750 + 4, 500 + 8);
    const AutoOption on = find_auto_option(g, win, p, s);
    check(on.found && on.state == AutoState::On && std::abs(on.at.x - 750) <= 1,
          fmt("with the ticked box over it: found (%.3f) and read as on (on %.3f / off %.3f)",
              static_cast<double>(on.score), static_cast<double>(on.on), static_cast<double>(on.off)));

    Frame h = f;
    scribble(h, ScreenRect{ 750 + 4, 500 + 8, 7, 14 }, 9);
    const AutoOption smudged = find_auto_option(h, win, p, s);
    check(smudged.found && smudged.state == AutoState::Unknown,
          fmt("with half the box under noise: found, and read as unknown (on %.3f / off %.3f)",
              static_cast<double>(smudged.on), static_cast<double>(smudged.off)));

    // The hide-low-level checkbox is the very same picture as the box, off.
    check(!find_auto_option(base, win, p, s).found,
          "only the hide-low-level checkbox (the off box at window+523,37): no Tu Dong found");
    Frame lone = base;
    paste(lone, a.box_off, 750 + 4, 500 + 8);
    check(!find_auto_option(lone, win, p, s).found,
          "the bare box pasted where Tu Dong's would be, without its words: no Tu Dong found");
    float on_score = 0.0f, off_score = 0.0f;
    check(read_quest_box(base, win.x + 523, win.y + 37, p, s, on_score, off_score) == AutoState::Off,
          "(and that checkbox does read as an unticked box: why it is never read for the state)");
    Frame low = f;
    paste(low, a.radio, win.x + 450, win.y + 300);
    check(find_auto_option(low, win, p, s).at.y == 500,
          "a Tu Dong above the bottom hundred pixels is not looked at");
}

// --------------------------------------------------------------------- round

void test_round() {
    section("Quick quest: a round on a pretend game (Q-A3 to Q-A5)");
    if (!have_art()) return;

    {
        PretendQuest g;
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::TurnedOn, std::string("closed, off: outcome ") +
                                                       quest_outcome_name(r.outcome));
        check(r.keys_pressed == 2 && g.keys.size() == 2 && g.keys[0] == 85 && g.keys[1] == 85 &&
                  g.clicks.size() == 2 && r.clicks == 2,
              "... exactly 2 presses of U and 2 clicks");
        check(g.clicks.size() == 2 && near_point(g.clicks[0], 300 + 274, 120 + 47, 2) &&
                  near_point(g.clicks[1], 300 + 450 + 11, 120 + 380 + 15, 2),
              "... the clicks at window+274,47 (NV Phụ) and at Tự Động+11,15");
        check(!g.open && g.on && r.opened_by_us && !r.left_open, "... Tự Động on, and the window closed again");
        check(g.moves.size() == 1 && g.clicks.size() == 2 &&
                  g.moves[0].first == g.clicks[1].first + 14 && g.moves[0].second == g.clicks[1].second,
              "... the pointer moved 14 px right of the click before the box was read again");
        check(contains(r.log, "NV Phu tab clicked 574,167") && contains(r.log, "-> on") &&
                  contains(r.log, "closed"),
              "the log line: " + r.log);
        check(quest_result_vi(r, "U") == "đã bật ô Tự Động", "for the tab: " + quest_result_vi(r, "U"));
    }
    {
        PretendQuest g;
        g.on = true;
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::AlreadyOn && g.keys.size() == 2 && g.clicks.size() == 1 &&
                  g.on && !g.open,
              "already on: outcome already_on, 2 key presses, 1 click (the tab), the box left alone");
        check(quest_result_vi(r, "U") == "ô Tự Động đã bật sẵn", "for the tab: " + quest_result_vi(r, "U"));
    }
    {
        PretendQuest g;
        g.open = true;
        g.tab = 1;
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::TurnedOn && g.keys.empty() && g.clicks.size() == 2 && g.open &&
                  !r.opened_by_us,
              "the window already open on another tab: no key at all, NV Phụ and the box clicked, and "
              "the window left open");
        PretendQuest h;
        h.open = true;
        h.tab = 3;
        h.on = true;
        const QuestResult r2 = h.run();
        check(r2.outcome == QuestOutcome::AlreadyOn && h.keys.empty() && h.clicks.size() == 1 && h.open,
              "the player's window on NV Phụ with Tự Động on: no key, the tab click only, left open");
    }
    {
        PretendQuest g;
        g.key_opens = false;
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::NoWindow && g.keys.size() == 1 && g.clicks.empty(),
              "the window never appears: 1 key press, 0 clicks, outcome no_window");
        // The key went in, and whatever it opened -- another window, or this
        // one drawn so that it is not known -- is still there.
        check(r.maybe_open && !r.left_open && r.opened_by_us,
              "... and said to have maybe opened something GoBot does not know, not this window");
        check(quest_result_vi(r, "U") == "không mở được cửa sổ Nhiệm Vụ (đã bấm U); có thể đã mở một "
                                         "cửa sổ mà GoBot không nhận ra — xem trong game",
              "for the tab: " + quest_result_vi(r, "U"));
        check(r.failed() && r.last.valid(), "... a failure, with the last frame kept for quest_debug");
        check(quest_trouble_why(r) == "không mở được cửa sổ Nhiệm Vụ", "... and the alert's words as before");
    }
    {
        PretendQuest g;
        g.has_auto = false;
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::NoAuto && g.keys.size() == 2 && g.clicks.size() == 1 && !g.open,
              "no Tự Động in NV Phụ: the tab click and the closing key only, outcome no_auto");
        check(quest_result_vi(r, "U") == "không thấy ô Tự Động trong thẻ NV Phụ",
              "for the tab: " + quest_result_vi(r, "U"));
    }
    {
        PretendQuest g;
        g.click_takes = false;
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::ClickFailed && g.clicks.size() == 2 && g.keys.size() == 2 &&
                  !g.open && !g.on,
              "a click that does not take: exactly one click on the box, no second, outcome click_failed");
        check(quest_result_vi(r, "U") == "đã bấm ô Tự Động nhưng không thấy nó bật",
              "for the tab: " + quest_result_vi(r, "U"));
    }
    {
        PretendQuest g;
        g.smudged_box = true;
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::UnknownState && g.clicks.size() == 1 && !g.open,
              "a box that reads as neither: not clicked, outcome unknown_state, window closed");
    }
    {
        PretendQuest g;
        g.english = true;
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::English && g.clicks.empty() && g.keys.size() == 2 && !g.open,
              "the English client's window: nothing clicked, closed again, outcome english");
        check(quest_result_vi(r, "U") == "Bản game tiếng Anh: chưa hỗ trợ (cần chọn nhiệm vụ như GoC)",
              "for the tab: " + quest_result_vi(r, "U"));
        PretendQuest h;
        h.english = true;
        h.open = true;
        const QuestResult r2 = h.run();
        check(r2.outcome == QuestOutcome::English && h.clicks.empty() && h.keys.empty() && h.open,
              "the English window open already: nothing pressed at all");
    }
    {
        // A window this round opened that will not close.
        PretendQuest g;
        int presses = 0;
        QuestRound::Hands hands = g.hands();
        hands.key = [&](std::uint32_t vk, int) {
            g.keys.push_back(vk);
            if (++presses == 1) g.open = true;     // opens, and then sticks
            return true;
        };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::TurnedOn && r.left_open && g.keys.size() == 2 && g.open &&
                  r.failed() && !r.done(),
              "the window will not close: one closing key, no more, and said: " +
                  quest_result_vi(r, "U"));
        check(quest_trouble_why(r) == "không đóng được cửa sổ Nhiệm Vụ", "... the alert's words for it");
    }
    {
        // The key does not reach the game (it is not in front).
        PretendQuest g;
        QuestRound::Hands hands = g.hands();
        hands.key = [](std::uint32_t, int) { return false; };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::Refused && g.clicks.empty() && r.keys_pressed == 0,
              "a key that does not go: refused, nothing more done");
    }
    {
        // A click that does not go (the game covered at that point): refused,
        // the window this round opened closed again, and the tab says why.
        PretendQuest g;
        QuestRound::Hands hands = g.hands();
        hands.click = [](int, int) { return false; };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::Refused && r.clicks == 0 && g.keys.size() == 2 && !g.open &&
                  quest_result_vi(r, "U") == "không bấm: game bị che hoặc không ở trên cùng",
              "a click that does not go: refused, the window closed again -- " + quest_result_vi(r, "U"));
    }
    {
        // The real waits, added up on the pretend clock: a round holds
        // everything off for a few seconds, not more.
        PretendQuest g;
        const std::uint64_t start = g.clock;
        g.run();
        const std::uint64_t took = g.clock - start;
        check(took >= 1500 && took <= 4000, fmt("a whole round waits %llu ms in all",
                                                static_cast<unsigned long long>(took)));
    }
}

// ------------------------------------------------------- never clicked there

void test_forbidden() {
    section("Quick quest: never Hủy Nhiệm Vụ, the checkbox, another tab or outside (Q-A6)");
    const QuestSettings s;
    std::mt19937 rng(20261015);
    int planned = 0, allowed = 0, bad_allowed = 0, outside = 0, outside_refused = 0;
    const auto in = [](const ScreenRect& r, int x, int y) { return r.contains(x, y); };
    for (int i = 0; i < 200; ++i) {
        // Some windows partly off the client area, as a dragged window can be.
        const int wx = static_cast<int>(rng() % 1500) - 300;
        const int wy = static_cast<int>(rng() % 900) - 200;
        const ScreenRect win{ wx, wy, 672, 460 };
        std::vector<std::pair<int, int>> points;
        points.emplace_back(wx + s.tab_click_x, wy + s.tab_click_y);
        // Tự Động anywhere in the bottom hundred pixels.
        const int rx = static_cast<int>(rng() % 593), ry = 360 + static_cast<int>(rng() % 71);
        points.emplace_back(wx + rx + s.auto_click_x, wy + ry + s.auto_click_y);
        // And points aimed squarely at what must never be clicked.
        points.emplace_back(wx + 336, wy + 432);            // Hủy Nhiệm Vụ
        points.emplace_back(wx + 530, wy + 44);             // the checkbox
        points.emplace_back(wx + 47, wy + 46);              // NV Chính
        points.emplace_back(wx + 199, wy + 46);             // Hoàn Thành
        points.emplace_back(wx + 286, wy + 450);            // the quick quest's buttons, by Hủy Nhiệm Vụ
        for (const auto& [x, y] : points) {
            ++planned;
            const bool out = x < 0 || y < 0 || x >= 1280 || y >= 720;
            const bool ok = quest_click_ok(win, x, y, 1280, 720, s);
            if (out) {
                ++outside;
                if (!ok) ++outside_refused;
            }
            if (!ok) continue;
            ++allowed;
            const bool forbidden = in(ScreenRect{ wx + 296, wy + 417, 80, 30 }, x, y) ||
                                   in(ScreenRect{ wx + 283, wy + 412, 103, 41 }, x, y) ||
                                   in(ScreenRect{ wx + 523, wy + 37, 14, 14 }, x, y) ||
                                   in(ScreenRect{ wx + 10, wy + 32, 228, 28 }, x, y) || out ||
                                   !win.contains(x, y);
            if (forbidden) ++bad_allowed;
        }
    }
    check(planned == 1400 && allowed > 100 && bad_allowed == 0,
          fmt("200 windows, %d points: %d allowed, none on Hủy Nhiệm Vụ, the quest buttons, the checkbox, "
              "another tab or outside",
              planned, allowed));
    {
        // Every pixel where GoC reads the quick quest's Accept, Give up and
        // Complete -- Hủy Nhiệm Vụ's place and round it -- refused, and said.
        const ScreenRect win{ 300, 120, 672, 460 };
        int pixels = 0, passed = 0;
        for (int y = win.y + 412; y < win.y + 412 + 41; ++y)
            for (int x = win.x + 283; x < win.x + 283 + 103; ++x) {
                ++pixels;
                if (quest_click_ok(win, x, y, 1280, 720, s)) ++passed;
            }
        std::string why, why_vi;
        quest_click_ok(win, win.x + 285, win.y + 414, 1280, 720, s, &why, &why_vi);
        check(pixels == 4223 && passed == 0 && contains(why, "Accept") &&
                  why_vi == "trúng chỗ các nút nhận, bỏ và trả nhiệm vụ",
              fmt("all %d pixels of the quest buttons' area refused (%d let through): %s", pixels, passed,
                  why_vi.c_str()));
        quest_click_ok(win, win.x + 336, win.y + 432, 1280, 720, s, &why, &why_vi);
        check(why_vi == "trúng nút Hủy Nhiệm Vụ", "... Hủy Nhiệm Vụ inside it said as itself: " + why_vi);
    }
    check(outside > 50 && outside_refused == outside,
          fmt("all %d points outside the client area are refused", outside));
    std::string why;
    const bool past_edge = quest_click_ok(ScreenRect{ 900, 300, 672, 460 }, 1361, 695, 1280, 720, s, &why);
    check(!past_edge && contains(why, "outside"),
          "a point past the right edge is refused -- not moved to the edge: " + why);
    check(quest_click_ok(ScreenRect{ 300, 120, 672, 460 }, 574, 167, 1280, 720, s),
          "GoC's point in NV Phụ is allowed");

    if (!have_art()) return;
    {
        // Tự Động drawn where its click would land on Hủy Nhiệm Vụ: refused.
        PretendQuest g;
        g.ax = 290;
        g.ay = 405;
        const QuestResult r = g.run();
        bool on_abandon = false;
        for (const auto& c : g.clicks)
            on_abandon = on_abandon || ScreenRect{ 300 + 296, 120 + 417, 80, 30 }.contains(c.first, c.second);
        check(r.outcome == QuestOutcome::Refused && g.clicks.size() == 1 && !on_abandon && !g.open,
              "Tự Động whose click would land on Hủy Nhiệm Vụ: refused, only the tab clicked, closed: " +
                  r.log);
        check(quest_result_vi(r, "U") == "không bấm: chỗ cần bấm trúng nút Hủy Nhiệm Vụ",
              "... and the tab says which place: " + quest_result_vi(r, "U"));

        // The probe says so before any round is run: everything found is not
        // enough when a round would refuse the click (the live test's first
        // step must not pass on that).
        PretendQuest h;
        h.ax = 290;
        h.ay = 405;
        h.open = true;
        h.tab = 3;
        const QuestProbe probe = probe_quest(h.screen(), art().pictures, art().settings);
        check(probe.window.found && probe.option.found && probe.tab_ok && !probe.click_ok &&
                  contains(probe.vi, "GoBot sẽ không bấm vào ô Tự Động") &&
                  contains(probe.vi, "trúng nút Hủy Nhiệm Vụ") && contains(probe.log, "refused") &&
                  h.keys.empty() && h.clicks.empty(),
              "the probe on that window: Tự Động found, its click refused and said -- " + probe.vi);
    }
    {
        // A window half below the client area: no Tự Động is looked for
        // outside it, and nothing is clicked there.
        PretendQuest g;
        g.wy = 400;
        const QuestResult r = g.run();
        bool outside_click = false;
        for (const auto& c : g.clicks)
            outside_click = outside_click || c.second >= 720 || c.first >= 1280;
        check(r.outcome == QuestOutcome::NoAuto && !outside_click && g.clicks.size() == 1 && !g.open,
              "a window hanging off the bottom: the tab clicked, no Tự Động, nothing clicked off screen");
    }
    {
        // A window too far right for its title bar to be on screen.
        PretendQuest g;
        g.wx = 900;
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::NoWindow && g.clicks.empty() && g.keys.size() == 1,
              "a window off the right edge: not taken for found, nothing clicked");
    }
}

// --------------------------------------------------------------------- cancel

void test_cancel() {
    section("Quick quest: a stop between any two steps (Q-A7)");
    if (!have_art()) return;
    const char* const kSteps[] = { "the opening key", "the NV Phụ click", "the click on the box",
                                   "the pointer's move" };
    for (int k = 0; k < 4; ++k) {
        PretendQuest g;
        if (k < 3) {
            g.stop_when = [k](const PretendQuest& p) { return p.inputs() >= k; };
        } else {
            g.stop_when = [](const PretendQuest& p) { return p.inputs() >= 3; };
        }
        const QuestResult r = g.run();
        check(r.outcome == QuestOutcome::Cancelled && g.inputs() == k && g.moves.empty(),
              fmt("stopped before %s: %d sent, nothing after, outcome cancelled", kSteps[k], k));
    }
    {
        // Before the closing key: the box is ticked, the window stays open --
        // and a stop is a stop, not a round that failed to close its window.
        PretendQuest g;
        g.stop_when = [](const PretendQuest& p) { return !p.moves.empty(); };
        const QuestResult r = g.run();
        check(g.keys.size() == 1 && g.clicks.size() == 2 && g.open && r.left_open,
              "stopped before the closing key: no closing key -- the window may stay open");
        check(r.outcome == QuestOutcome::Cancelled && !r.failed() && !r.done(),
              std::string("... outcome cancelled, not counted as a failure (") + quest_outcome_name(r.outcome) +
                  ")");
    }
    {
        // After the closing key, while it is looked at to see it shut: the
        // same. A window that never shuts, and a stop meanwhile.
        PretendQuest g;
        QuestRound::Hands hands = g.hands();
        int presses = 0;
        hands.key = [&](std::uint32_t vk, int) {
            g.keys.push_back(vk);
            if (++presses == 1) g.open = true;
            return true;
        };
        hands.stop = [&] { return presses >= 2; };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::Cancelled && r.left_open && !r.failed() && g.keys.size() == 2,
              std::string("stopped while the closing key's effect is looked for: cancelled, not a failure (") +
                  quest_outcome_name(r.outcome) + ")");
    }
    {
        // A failure that is closing its window when the stop comes: the stop
        // is what became of it. The fourth look is the one before the closing
        // key: closed, opened, on NV Phụ, then that.
        PretendQuest g;
        g.has_auto = false;
        QuestRound::Hands hands = g.hands();
        int looks = 0;
        hands.grab = [&](Frame& f) {
            ++looks;
            f = g.screen();
            return true;
        };
        hands.stop = [&] { return looks >= 4; };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::Cancelled && !r.failed() && g.keys.size() == 1 && g.open &&
                  r.left_open,
              std::string("no Tự Động, and a stop as its window is about to be closed: cancelled, nothing "
                          "more pressed, not counted (") +
                  quest_outcome_name(r.outcome) + ")");
    }
    {
        // cancel() from elsewhere, as stop_quest does it.
        PretendQuest g;
        QuestRound::Hands hands = g.hands();
        QuestRound* self = nullptr;
        const auto click = hands.click;
        hands.click = [&](int x, int y) {
            const bool ok = click(x, y);
            if (self) self->cancel();
            return ok;
        };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        self = &round;
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::Cancelled && g.clicks.size() == 1 && g.keys.size() == 1,
              "cancel() during the tab click: nothing more sent");
    }
}

// ------------------------------------------------------------------- schedule

/// A run on a pretend clock, its gates and rounds scripted.
struct PretendRun {
    QuestRun run{ 7 };
    QuestGateInput gates;
    std::vector<QuestOutcome> script;       ///< what each round gives; the last repeats
    bool maybe_open = false;                ///< and whether its key may have opened something else
    int rounds = 0;
    std::vector<std::uint64_t> round_at;
    std::vector<std::pair<std::string, std::string>> alerts;
    std::vector<std::string> said;
    std::vector<QuestCall> calls;           ///< what each round was told
    std::uint64_t now = 1'000'000;

    PretendRun() {
        gates.window = true;
        gates.focused = true;
        gates.hp_pct = 100.0f;
        gates.hp_floor = 60;
        QuestRun::Hooks h;
        h.gates = [this] { return gates; };
        h.round = [this](const QuestCall& call) {
            QuestResult r;
            const std::size_t i = std::min<std::size_t>(static_cast<std::size_t>(rounds),
                                                        script.empty() ? 0 : script.size() - 1);
            r.outcome = script.empty() ? QuestOutcome::TurnedOn : script[i];
            r.maybe_open = maybe_open;
            ++rounds;
            round_at.push_back(now);
            calls.push_back(call);
            return r;
        };
        h.alert = [this](const std::string& e, const std::string& t) { alerts.emplace_back(e, t); };
        h.say = [this](const std::string& s) { said.push_back(s); };
        run.set_hooks(h);
    }
    /// Step every 100 ms for `ms`.
    void go(std::uint64_t ms) {
        for (std::uint64_t t = 0; t < ms; t += 100) {
            now += 100;
            run.step(now);
        }
    }
};

void test_schedule() {
    section("Quick quest: when rounds run, and what holds them (Q-A8)");
    QuestSettings s;
    s.enabled = true;

    {
        PretendRun p;
        p.run.start(p.now, s);
        const std::uint64_t t0 = p.now;
        p.go(39'000);
        check(p.rounds == 0, "nothing in the first 39 s");
        p.go(1'100);
        check(p.rounds == 1 && p.round_at[0] - t0 >= 40'000 && p.round_at[0] - t0 <= 40'100,
              "the first round at 40 s");
        p.go(10 * 60'000);
        bool every = p.rounds >= 2;
        for (std::size_t i = 1; i < p.round_at.size(); ++i) {
            const std::uint64_t gap = p.round_at[i] - p.round_at[i - 1];
            every = every && gap >= 270'000 && gap <= 330'100;
        }
        check(every, fmt("then every 5 min, give or take a tenth (%d rounds in 10 min)", p.rounds - 1));
    }

    // Each gate puts a due round off by 30 s, and nothing is run.
    const struct { const char* what; std::function<void(QuestGateInput&)> set; const char* why; } kGates[] = {
        { "the HUD missing", [](QuestGateInput& g) { g.hp_pct = -1.0f; }, "không thấy thanh HP/MP" },
        { "somebody's hold", [](QuestGateInput& g) { g.held_by = "trash cleaning"; }, "bot đang tạm dừng" },
        { "the chat line in doubt", [](QuestGateInput& g) { g.chat = true; }, "ô chat" },
        { "a trash pass", [](QuestGateInput& g) { g.trash = true; }, "dọn rác" },
        { "recent input", [](QuestGateInput& g) { g.user_active = true; }, "chuột hoặc bàn phím" },
        { "low HP", [](QuestGateInput& g) { g.hp_pct = 55.0f; }, "máu thấp" },
        { "the keeper crossing", [](QuestGateInput& g) { g.keeper = true; }, "vào lại game" },
        { "no game window", [](QuestGateInput& g) { g.window = false; }, "cửa sổ game" },
        // The player busy in another window: a timed round never pulls the
        // game forward over it.
        { "the game not in front", [](QuestGateInput& g) { g.focused = false; }, "game không ở trên cùng" },
    };
    for (const auto& gate : kGates) {
        PretendRun p;
        p.run.start(p.now, s);
        gate.set(p.gates);
        p.go(40'100);
        const std::uint64_t next = p.run.next_at();
        check(p.rounds == 0 && next >= p.now + 29'000 && next <= p.now + 30'100 && !p.said.empty() &&
                  contains(p.said.back(), gate.why),
              std::string("held by ") + gate.what + ": put off 30 s, nothing run -- \"" +
                  (p.said.empty() ? std::string() : p.said.back()) + "\"");
        p.gates = QuestGateInput{};
        p.gates.window = true;
        p.gates.focused = true;
        p.gates.hp_pct = 100.0f;
        p.go(30'100);
        check(p.rounds == 1, std::string("... and run once ") + gate.what + " is over");
    }
    {
        QuestGateInput in;
        in.window = true;
        in.focused = true;
        in.hp_pct = 65.0f;
        in.hp_floor = 60;
        check(quest_gate(in).go, "HP over the floor: may go");
        in.resting = true;
        check(!quest_gate(in).go && quest_gate(in).why == "vừa làm xong một lần chưa đầy 1 phút",
              "a round ended under a minute ago: not yet");
        in.resting = false;
        in.focused = false;
        check(!quest_gate(in).go, "the game behind another window: a timed round may not go");
        in.asked = true;
        check(quest_gate(in).go, "... a round asked for with Làm ngay 1 lần may: it brings the game forward");
    }
    {
        // The game behind another window all along: the timed rounds wait,
        // and the one asked for goes -- told that it was asked for.
        PretendRun p;
        p.gates.focused = false;
        p.run.start(p.now, s);
        p.go(5 * 60'000);
        check(p.rounds == 0, "the game never in front for 5 min: no timed round at all");
        p.run.ask_once(p.now);
        p.go(500);
        check(p.rounds == 1 && p.calls.size() == 1 && p.calls[0].asked,
              "Làm ngay 1 lần meanwhile: runs, and the round is told a person asked for it");
        PretendRun q;
        q.run.start(q.now, s);
        q.go(40'100);
        check(q.rounds == 1 && !q.calls[0].asked, "a timed round is told it was not asked for");
    }
    {
        // Asked for while something else was in the way, and the game behind
        // another window when that clears 55 s later -- where the person who
        // asked may be typing by then. Not pulled forward over them: it waits
        // for the game to be in front, as a timed round does (QQ-R3-1).
        PretendRun p;
        p.gates.user_active = true;
        p.gates.focused = false;
        p.run.ask_once(p.now);
        p.go(55'000);
        check(p.rounds == 0 && p.run.asked(), "asked while somebody is at the keyboard: nothing yet");
        p.gates.user_active = false;
        p.go(5'000);
        check(p.rounds == 0 && !p.said.empty() && contains(p.said.back(), "game không ở trên cùng"),
              "55 s on, the game behind another window: no round -- \"" +
                  (p.said.empty() ? std::string() : p.said.back()) + "\"");
        p.gates.focused = true;
        p.go(1'500);
        check(p.rounds == 1 && !p.calls.back().asked,
              "... and once the game is in front it runs, told it may not bring the game forward");
        PretendRun q;
        q.gates.focused = false;
        q.run.ask_once(q.now);
        q.go(4'000);
        check(q.rounds == 1 && q.calls[0].asked,
              "straight after the click, the game behind: it runs, and may bring the game forward");
    }

    // The box found ticked again and again: looked at less often.
    {
        PretendRun p;
        p.script = { QuestOutcome::AlreadyOn };
        QuestSettings quick = s;
        quick.first_delay_s = 0;
        p.run.start(p.now, quick);
        std::vector<std::uint64_t> intervals;
        for (int i = 0; i < 6; ++i) {
            const int before = p.rounds;
            while (p.rounds == before) p.go(1000);
            intervals.push_back(p.run.schedule().interval_ms() / 60'000);
        }
        check(intervals == std::vector<std::uint64_t>{ 5, 5, 10, 20, 30, 30 },
              fmt("already on, round after round: the interval goes 5, 5, 10, 20, 30, 30 min (%llu, %llu, "
                  "%llu, %llu, %llu, %llu)",
                  static_cast<unsigned long long>(intervals[0]), static_cast<unsigned long long>(intervals[1]),
                  static_cast<unsigned long long>(intervals[2]), static_cast<unsigned long long>(intervals[3]),
                  static_cast<unsigned long long>(intervals[4]), static_cast<unsigned long long>(intervals[5])));
        p.script.push_back(QuestOutcome::TurnedOn);     // the next one finds it off
        const int before = p.rounds;
        while (p.rounds == before) p.go(1000);
        check(p.run.schedule().interval_ms() == 5 * 60'000, "a round that finds it off: back to 5 min");
        QuestSettings no_back = quick;
        no_back.back_off = false;
        QuestSchedule plain(1);
        plain.start(0, no_back);
        QuestResult on;
        on.outcome = QuestOutcome::AlreadyOn;
        for (int i = 0; i < 5; ++i) plain.done(1000ull * static_cast<unsigned>(i), on);
        check(plain.interval_ms() == 5 * 60'000, "with back_off=false it stays at 5 min");
    }

    // A key that may have opened some other window, three times: the phone
    // is told that too, not only the tab.
    {
        PretendRun p;
        p.script = { QuestOutcome::NoWindow };
        p.maybe_open = true;
        p.run.start(p.now, s);
        p.go(40 * 60'000);
        check(p.alerts.size() == 1 &&
                  p.alerts[0].second == "Làm nhiệm vụ nhanh đã tự tắt: 3 lần liền không mở được cửa sổ "
                                        "Nhiệm Vụ; có thể còn một cửa sổ khác đang mở trong game.",
              "a key that may have opened something else: the alert says so -- " +
                  (p.alerts.empty() ? std::string() : p.alerts[0].second));
    }

    // Three failures in a row stop it, and the alert is raised once.
    {
        PretendRun p;
        p.script = { QuestOutcome::NoWindow };
        p.run.start(p.now, s);
        p.go(40 * 60'000);
        check(p.rounds == 3 && p.run.stopped_itself() && p.alerts.size() == 1 &&
                  p.alerts[0].first == "quest_trouble",
              fmt("three rounds that fail: it stops itself after the third (%d rounds), one alert", p.rounds));
        check(!p.alerts.empty() &&
                  p.alerts[0].second == "Làm nhiệm vụ nhanh đã tự tắt: 3 lần liền không mở được cửa sổ "
                                        "Nhiệm Vụ.",
              "... the alert: " + (p.alerts.empty() ? std::string() : p.alerts[0].second));
        check(p.run.stopped_why() == "Đã tự tắt: 3 lần liền không mở được cửa sổ Nhiệm Vụ. Xem ảnh trong "
                                     "thư mục quest_debug.",
              "... the tab, in red: " + p.run.stopped_why());
        check(p.run.stopped_log() == "3 failed rounds in a row, the last no_window",
              "... and the log, in English: " + p.run.stopped_log());
        check(p.run.next_at() == 0, "... and no round is due after that");
        PretendRun q;
        q.script = { QuestOutcome::NoAuto, QuestOutcome::NoAuto, QuestOutcome::TurnedOn,
                     QuestOutcome::NoAuto, QuestOutcome::NoAuto, QuestOutcome::AlreadyOn };
        q.run.start(q.now, s);
        q.go(40 * 60'000);
        check(!q.run.stopped_itself() && q.alerts.empty() && q.rounds >= 6,
              "two failures, a success, two failures: not stopped -- a success resets the count");
        PretendRun e;
        e.script = { QuestOutcome::English };
        e.run.start(e.now, s);
        e.go(60'000);
        check(e.run.stopped_itself() && e.alerts.empty() && e.rounds == 1 &&
                  e.run.stopped_why() == "Bản game tiếng Anh: chưa hỗ trợ (cần chọn nhiệm vụ như GoC).",
              "the English client: stops at once with the reason, and no alert");
        PretendRun en;
        QuestSettings english = s;
        english.skin = QuestSettings::Skin::En;
        en.run.start(en.now, english);
        en.go(60'000);
        check(en.run.stopped_itself() && en.rounds == 0, "skin=en in the file: refused before any round");
    }

    // Asked for with a button: runs when nothing is in the way, a minute and
    // a half at most, and the rest gate counts for it too.
    {
        PretendRun p;
        p.run.ask_once(p.now);
        p.go(500);
        check(p.rounds == 1 && !p.run.asked(), "Làm ngay 1 lần with nothing in the way: one round at once");
        p.run.ask_once(p.now);
        p.go(30'000);
        check(p.rounds == 1 && p.run.asked(), "asked again 30 s later: waits for the minute's rest");
        p.go(31'000);
        check(p.rounds == 2 && !p.run.asked(), "... and runs once the minute is up");
        p.gates.user_active = true;
        p.go(100'000);
        p.run.ask_once(p.now);
        p.go(89'000);
        check(p.rounds == 2 && p.run.asked(), "asked while somebody is at the keyboard: keeps waiting");
        p.go(2'000);
        check(p.rounds == 2 && !p.run.asked() && !p.said.empty() &&
                  contains(p.said.back(), "Không làm được lần này"),
              "... and gives up after a minute and a half: " + (p.said.empty() ? std::string() : p.said.back()));
    }

    // Back in after a crossing, or revived: a round 30 s on.
    {
        PretendRun p;
        p.run.start(p.now, s);
        p.go(41'000);
        p.go(120'000);
        const int before = p.rounds;
        p.run.soon(p.now);
        p.go(31'000);
        check(p.rounds == before + 1, "the keeper back in, or a revive: a round 30 s later");
    }
    {
        // A real round takes a few seconds: the next is counted from its end.
        PretendRun p;
        QuestRun::Hooks h;
        h.gates = [&p] { return p.gates; };
        h.round = [&p](const QuestCall&) {
            ++p.rounds;
            p.round_at.push_back(p.now);
            QuestResult r;
            r.outcome = QuestOutcome::TurnedOn;
            return r;
        };
        h.now = [&p] { return p.now + 4'000; };
        p.run.set_hooks(h);
        p.run.start(p.now, s);
        p.go(40'100);
        check(p.rounds == 1 && p.run.schedule().last_end() == p.round_at[0] + 4'000,
              "a round that ends 4 s after it began: the schedule counts from its end");
    }
    {
        // A round that could not start after all -- the hold taken, somebody
        // at the keys -- waits like a gate.
        PretendRun p;
        p.script = { QuestOutcome::NotNow };
        p.run.start(p.now, s);
        p.go(41'000);
        check(p.rounds == 1 && p.run.next_at() >= p.now + 28'000 && !p.run.has_last(),
              "a round that says not now: put off 30 s, not counted");
    }
}

// ------------------------------------------------------------- HP mid-round

void test_low_hp() {
    section("Quick quest: HP falling mid-round lets the potion go");
    if (!have_art()) return;
    {
        // After the tab click: nothing more -- not the box, not the closing key.
        PretendQuest g;
        QuestRound::Hands hands = g.hands();
        hands.low_hp = [&g] { return !g.clicks.empty(); };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::NotNow && g.keys.size() == 1 && g.clicks.size() == 1 &&
                  g.moves.empty() && g.open && !g.on,
              std::string("HP under the potion's mark after the tab click: nothing more pressed (") +
                  quest_outcome_name(r.outcome) + ")");
        check(!r.failed() && !r.done() && r.why == "máu thấp" && r.opened_by_us && r.left_open,
              "... not now and not a failure; the window it opened is left, said to be open");
        check(quest_result_vi(r, "U") == "dừng giữa chừng: máu thấp; cửa sổ Nhiệm Vụ vẫn mở",
              "for the tab: " + quest_result_vi(r, "U"));
        check(contains(r.log, "HP fell under the potion's mark"), "the log: " + r.log);
    }
    {
        // After the box is ticked: the closing key waits for the next round.
        PretendQuest g;
        QuestRound::Hands hands = g.hands();
        hands.low_hp = [&g] { return !g.moves.empty(); };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::NotNow && g.keys.size() == 1 && g.clicks.size() == 2 && g.on &&
                  g.open && r.left_open,
              "HP falling once the box is ticked: no closing key either -- not now, window left open");
    }
    {
        // HP never high enough: nothing at all.
        PretendQuest g;
        QuestRound::Hands hands = g.hands();
        hands.low_hp = [] { return true; };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::NotNow && g.inputs() == 0 && !r.left_open && !r.looked,
              "HP low from the start: nothing pressed, nothing looked at");
        check(quest_result_vi(r, "U") == "chưa làm: máu thấp", "for the tab: " + quest_result_vi(r, "U"));
    }
    {
        // A stop and low HP together: the stop is what is said.
        PretendQuest g;
        QuestRound::Hands hands = g.hands();
        hands.low_hp = [] { return true; };
        hands.stop = [] { return true; };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        check(round.run().outcome == QuestOutcome::Cancelled && g.inputs() == 0,
              "a stop and low HP at once: cancelled, as a stop");
    }
    {
        // The player goes to another window after the tab click: nothing
        // more, and not a failure -- the same as HP falling. Counted as one,
        // a player who switches windows often had quick quest stop itself,
        // with an alert, over nothing wrong in the game.
        PretendQuest g;
        QuestRound::Hands hands = g.hands();
        hands.behind = [&g] { return !g.clicks.empty(); };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::NotNow && !r.failed() && g.keys.size() == 1 &&
                  g.clicks.size() == 1 && r.why == "game không ở trên cùng" && r.left_open,
              std::string("the game gone behind another window part way: nothing more pressed, not "
                          "now and not a failure (") +
                  quest_outcome_name(r.outcome) + "), the window it opened said to be open");
    }
    {
        // A stop that comes after the round has asked, as the key is about to
        // go down: the hand asks once more and holds it back, as the GUI's
        // does. A stop -- not a refusal, which counts as a failure and could
        // raise an alert over the player's own F12.
        PretendQuest g;
        bool stop = false;
        QuestRound::Hands hands = g.hands();
        hands.stop = [&stop] { return stop; };
        hands.key = [&stop](std::uint32_t, int) {
            stop = true;        // F12, between the round's asking and the key
            return false;       // and so held back
        };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::Cancelled && !r.failed() && g.inputs() == 0,
              std::string("a stop as the opening key was to go: held back, stopped, not a failure (") +
                  quest_outcome_name(r.outcome) + ")");
    }
    {
        // The same as the tab's click was to go, in the window this round
        // opened: not even the closing key after it.
        PretendQuest g;
        bool stop = false;
        QuestRound::Hands hands = g.hands();
        hands.stop = [&stop] { return stop; };
        hands.click = [&stop](int, int) {
            stop = true;
            return false;
        };
        QuestRound round(art().pictures, QuestSettings{}, hands);
        const QuestResult r = round.run();
        check(r.outcome == QuestOutcome::Cancelled && !r.failed() && g.keys.size() == 1 &&
                  g.clicks.empty() && g.open && r.left_open,
              std::string("... and as a click was to go: nothing more, not even the closing key, the "
                          "window said to be open (") +
                  quest_outcome_name(r.outcome) + ")");
    }
}

// ------------------------------------------------------ a window left open

/// A run on a pretend clock whose rounds are real ones, on a pretend game:
/// what one round leaves, the next one finds.
struct PretendLiveRun {
    PretendQuest game;
    QuestRun run{ 5 };
    QuestGateInput gates;
    QuestSettings settings;
    std::function<bool()> low_hp;           ///< for the rounds' hands, when set
    std::vector<QuestCall> calls;
    std::vector<QuestOutcome> outcomes;
    std::vector<int> keys;                  ///< each round's key presses
    std::vector<std::pair<std::string, std::string>> alerts;

    PretendLiveRun() {
        gates.window = true;
        gates.focused = true;
        gates.hp_pct = 100.0f;
        gates.hp_floor = 60;
        QuestRun::Hooks h;
        h.gates = [this] { return gates; };
        h.round = [this](const QuestCall& call) {
            calls.push_back(call);
            QuestRound::Hands hands = game.hands();
            if (low_hp) hands.low_hp = low_hp;
            QuestRound round(art().pictures, settings, hands);
            round.adopt_open_window(call.ours_open);
            QuestResult r = round.run();
            outcomes.push_back(r.outcome);
            keys.push_back(r.keys_pressed);
            r.last = Frame{};                   // not kept: a few megabytes a round
            return r;
        };
        h.alert = [this](const std::string& e, const std::string& t) { alerts.emplace_back(e, t); };
        h.now = [this] { return game.clock; };
        run.set_hooks(h);
    }
    /// Step every 100 ms for `ms` of the game's clock, which a round moves on
    /// by its own waits too.
    void go(std::uint64_t ms) {
        const std::uint64_t until = game.clock + ms;
        while (game.clock < until) {
            game.clock += 100;
            run.step(game.clock);
        }
    }
};

void test_left_open() {
    section("Quick quest: a window GoBot left open is GoBot's to close");
    if (!have_art()) return;
    QuestSettings s;
    s.enabled = true;
    {
        // The quest key opens the window but never shuts it. Each round after
        // the first finds the window it left, takes it as its own, presses the
        // one closing key -- and, the window still open, counts a failure: the
        // third stops it, with the alert, rather than every round after the
        // first taking it for the player's and leaving it there all night.
        PretendLiveRun p;
        p.game.key_closes = false;
        p.settings = s;
        p.run.start(p.game.clock, s);
        p.go(40 * 60'000);
        const bool adopted = p.calls.size() == 3 && !p.calls[0].ours_open && p.calls[1].ours_open &&
                             p.calls[2].ours_open;
        check(p.outcomes.size() == 3 && p.run.stopped_itself() && adopted,
              fmt("a key that never shuts the window: %d rounds, the 2nd and 3rd taking the window left open "
                  "as GoBot's, then stopped",
                  static_cast<int>(p.outcomes.size())));
        check(p.keys == std::vector<int>{ 2, 1, 1 },
              "... the first round two keys, the next ones just the closing key");
        check(p.alerts.size() == 1 && p.alerts[0].first == "quest_trouble" &&
                  p.alerts[0].second == "Làm nhiệm vụ nhanh đã tự tắt: 3 lần liền không đóng được cửa sổ "
                                        "Nhiệm Vụ.",
              "... one alert: " + (p.alerts.empty() ? std::string() : p.alerts[0].second));
        check(p.run.stopped_log() == "3 failed rounds in a row, the last already_on with the window left open",
              "... and the log line: " + p.run.stopped_log());
    }
    {
        // HP falls in the middle of a round, with the window open: put off like
        // a gate, not counted -- and the next round, once HP is back, finds the
        // window, finishes the job and closes it.
        PretendLiveRun p;
        p.settings = s;
        bool low = true;
        p.low_hp = [&p, &low] { return low && !p.game.clicks.empty(); };
        p.run.start(p.game.clock, s);
        p.go(41'000);
        check(p.outcomes.size() == 1 && p.outcomes[0] == QuestOutcome::NotNow && p.game.open &&
                  p.run.ours_open() && !p.run.has_last() && p.run.schedule().failures() == 0,
              "HP falling after the tab click: not now, not counted, the window remembered as GoBot's");
        const std::uint64_t next = p.run.next_at();
        check(next >= p.game.clock + 28'000 && next <= p.game.clock + 30'100,
              "... and the round put off 30 s, like any gate");
        low = false;
        p.go(31'000);
        check(p.outcomes.size() == 2 && p.outcomes[1] == QuestOutcome::TurnedOn && p.calls[1].ours_open &&
                  p.keys[1] == 1 && !p.game.open && p.game.on && !p.run.ours_open(),
              std::string("the next round: the window taken as GoBot's, Tự Động ticked, closed with one key (") +
                  quest_outcome_name(p.outcomes.back()) + ")");
    }
    {
        // A stop mid-round ends the run with the window open. The next run is
        // told, as the GUI carries it from one worker to the next, and closes
        // it; told nothing, it would take it for the player's.
        PretendLiveRun first;
        first.settings = s;
        first.game.stop_when = [](const PretendQuest& g) { return !g.clicks.empty(); };
        first.run.start(first.game.clock, s);
        first.go(41'000);
        check(first.outcomes.size() == 1 && first.outcomes[0] == QuestOutcome::Cancelled &&
                  first.game.open && first.run.ours_open(),
              "a stop after the tab click: cancelled, the window open and remembered");
        PretendLiveRun next;
        next.settings = s;
        next.game = first.game;
        next.game.stop_when = nullptr;
        next.game.keys.clear();
        next.game.clicks.clear();
        next.run.set_ours_open(first.run.ours_open());
        next.run.start(next.game.clock, s);
        next.go(41'000);
        check(next.outcomes.size() == 1 && next.outcomes[0] == QuestOutcome::TurnedOn && !next.game.open &&
                  next.keys[0] == 1,
              "... the next run's first round finishes the job and closes it with one key");
    }
    {
        // The window GoBot left was shut meanwhile -- by the player, or a
        // crossing back in. The first look shows it shut: the memory goes,
        // and a window the player opens after that is theirs again.
        PretendLiveRun p;
        p.settings = s;
        p.run.set_ours_open(true);
        p.run.start(p.game.clock, s);
        p.go(41'000);
        check(p.outcomes.size() == 1 && p.outcomes[0] == QuestOutcome::TurnedOn && !p.run.ours_open() &&
                  p.keys[0] == 2 && !p.game.open,
              "remembered open, but seen shut: an ordinary round, and the memory gone");
        p.game.open = true;     // the player's own
        p.game.tab = 1;
        p.go(5 * 60'000 + 31'000);
        check(p.outcomes.size() == 2 && !p.calls[1].ours_open && p.keys[1] == 0 && p.game.open,
              "... the player's window afterwards: no key, left open");
    }
    {
        // A round that never looked -- the game not captured -- tells nothing
        // about the window: what was known is kept.
        QuestRun run(1);
        QuestRun::Hooks h;
        h.gates = [] {
            QuestGateInput in;
            in.window = in.focused = true;
            in.hp_pct = 100.0f;
            return in;
        };
        h.round = [](const QuestCall&) {
            QuestResult r;
            r.outcome = QuestOutcome::NoFrame;
            return r;
        };
        run.set_hooks(h);
        run.set_ours_open(true);
        QuestSettings quick = s;
        quick.first_delay_s = 0;
        run.start(1000, quick);
        run.step(1100);
        check(run.ours_open(), "a round that never saw the game: the window is still taken as maybe open");
    }
}

// --------------------------------------------------------------------- probe

void test_probe_and_frames() {
    section("Quick quest: the look-only probe, and the frames kept");
    if (have_art()) {
        const Art& a = art();
        PretendQuest g;
        const QuestProbe closed = probe_quest(g.screen(), a.pictures, a.settings);
        check(!closed.window.found &&
                  starts_with(closed.vi, "Không thấy cửa sổ Nhiệm Vụ trên màn hình — mở nó bằng tay "
                                         "(phím U) ở thẻ NV Phụ rồi thử lại."),
              "the window closed: " + closed.vi);
        g.open = true;
        const QuestProbe main = probe_quest(g.screen(), a.pictures, a.settings);
        check(main.window.found && !main.option.found && contains(main.vi, "Không thấy ô Tự Động") &&
                  contains(main.vi, "NV Phụ"),
              "open on NV Chính: " + main.vi);
        g.tab = 3;
        const QuestProbe off = probe_quest(g.screen(), a.pictures, a.settings);
        check(off.option.found && off.option.state == AutoState::Off && contains(off.vi, "đang tắt") &&
                  contains(off.vi, "bản tiếng Việt") && contains(off.log, "Tu Dong at"),
              "open on NV Phụ: " + off.vi);
        check(off.tab_ok && off.click_ok && !contains(off.vi, "sẽ không bấm") &&
                  contains(off.log, "tab click 574,167 ok") && contains(off.log, "click 761,515 ok"),
              "... both points a round would click are allowed, and the log says so: " + off.log);
        check(g.keys.empty() && g.clicks.empty(), "a probe presses nothing");

        // quest.ini's tab point edited onto another tab: the probe says a
        // round would refuse it, before any round does.
        QuestSettings moved = a.settings;
        moved.tab_click_x = 120;
        const QuestProbe wrong = probe_quest(g.screen(), a.pictures, moved);
        check(wrong.window.found && !wrong.tab_ok && wrong.click_ok &&
                  contains(wrong.vi, "GoBot sẽ không bấm vào thẻ NV Phụ ở 420,167: trúng một thẻ khác"),
              "a tab point edited onto another tab: refused, and said -- " + wrong.vi);
    }

    const std::string dir = temp_path("gobot_test_quest_debug");
    CreateDirectoryA(dir.c_str(), nullptr);
    Frame tiny;
    tiny.width = tiny.height = 4;
    tiny.pixels.assign(64, 128);
    int kept = 0;
    for (int i = 0; i < 25; ++i) kept += keep_quest_frame(dir, tiny, "no_window", false).empty() ? 0 : 1;
    for (int i = 0; i < 22; ++i) kept += keep_quest_frame(dir, tiny, "", true).empty() ? 0 : 1;
    int rounds = 0, probes = 0;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::string(fd.cFileName).rfind("probe_", 0) == 0) ++probes;
            else ++rounds;
            DeleteFileA((dir + "\\" + fd.cFileName).c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
    check(kept == 47 && rounds == 20 && probes == 20,
          fmt("quest_debug keeps the newest 20 of a round's frames and 20 of a probe's (%d and %d)", rounds,
              probes));
}

// --------------------------------------------------------------------- texts

void test_texts() {
    section("Quick quest: what the tab and the alert say");
    QuestResult r;
    const std::pair<QuestOutcome, const char*> kTab[] = {
        { QuestOutcome::TurnedOn, "đã bật ô Tự Động" },
        { QuestOutcome::AlreadyOn, "ô Tự Động đã bật sẵn" },
        { QuestOutcome::NoWindow, "không mở được cửa sổ Nhiệm Vụ (đã bấm U)" },
        { QuestOutcome::NoAuto, "không thấy ô Tự Động trong thẻ NV Phụ" },
        { QuestOutcome::ClickFailed, "đã bấm ô Tự Động nhưng không thấy nó bật" },
    };
    bool all = true;
    for (const auto& [o, text] : kTab) {
        r.outcome = o;
        all = all && quest_result_vi(r, "U") == text;
    }
    check(all, "the last result, as the BA wrote it: đã bật ô Tự Động / ô Tự Động đã bật sẵn / ...");
    r.outcome = QuestOutcome::TurnedOn;
    r.left_open = true;
    check(quest_result_vi(r, "U") == "đã bật ô Tự Động; cửa sổ Nhiệm Vụ vẫn mở",
          "a window left open is said: " + quest_result_vi(r, "U"));
    const std::pair<QuestOutcome, const char*> kWhy[] = {
        { QuestOutcome::NoWindow, "không mở được cửa sổ Nhiệm Vụ" },
        { QuestOutcome::NoAuto, "không thấy ô Tự Động trong thẻ NV Phụ" },
        { QuestOutcome::UnknownState, "không đọc được ô Tự Động đang bật hay tắt" },
        { QuestOutcome::ClickFailed, "bấm ô Tự Động mà không thấy nó bật" },
    };
    all = true;
    for (const auto& [o, text] : kWhy) {
        QuestResult x;
        x.outcome = o;
        all = all && quest_trouble_why(x) == text && x.failed() && !x.done();
    }
    check(all, "each failure has fixed words for the alert, and counts as a failure");
    QuestResult ok;
    ok.outcome = QuestOutcome::AlreadyOn;
    QuestResult stop;
    stop.outcome = QuestOutcome::Cancelled;
    check(ok.done() && !ok.failed() && !stop.done() && !stop.failed(),
          "already on is done; a stop is neither done nor a failure");
    check(std::string(quest_outcome_name(QuestOutcome::TurnedOn)) == "turned_on" &&
              std::string(quest_outcome_name(QuestOutcome::NoWindow)) == "no_window" &&
              std::string(auto_state_name(AutoState::Unknown)) == "unknown",
          "the outcomes' names for the log and the command line");

    // The quest key may not be one of GoBot's own switches -- F10 start/stop,
    // F12 panic, F11 hunting by default -- which GoBot would take its own
    // press of for the player's.
    const std::vector<std::uint32_t> own = { 121, 123, 122 };
    const std::string clash = quest_key_clash_vi(123, own);
    check(clash == "phím mở cửa sổ Nhiệm Vụ (" + InputManager::key_name(123) +
                       ") trùng phím bật/tắt của GoBot — chọn phím khác ở thẻ Nhiệm vụ" &&
              !quest_key_clash_vi(121, own).empty() && !quest_key_clash_vi(122, own).empty(),
          "a quest key that is GoBot's panic, start/stop or hunting key: refused -- " + clash);
    check(quest_key_clash_vi(85, own).empty() && quest_key_clash_vi(85, { 0, 0, 0 }).empty(),
          "U, or any key with no switches set: allowed");

    // The source never goes near GoC's dialog-closing driver or the buttons
    // that must never be clicked.
    const std::string src = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/quick_quest.cpp");
    if (src.empty()) {
        // Only where this was built: the source tree is not on a tester's machine.
        std::printf("  [skip] the source tree's src/quick_quest.cpp is not here\n");
    } else {
        check(!contains(src, "close_popups") && !contains(src, "DialogDriver") &&
                  !contains(src, "QuestCancel") && !contains(src, "btn_LQuest") &&
                  !contains(src, "type_text"),
              "quick_quest.cpp uses no DialogDriver::close_popups, loads no Hủy Nhiệm Vụ or "
              "Accept/Complete picture, and types nothing");
    }
}

} // namespace

void test_quest(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_settings();
    test_pictures();
    test_window();
    test_auto();
    test_round();
    test_forbidden();
    test_cancel();
    test_schedule();
    test_low_hp();
    test_left_open();
    test_probe_and_frames();
    test_texts();
}
