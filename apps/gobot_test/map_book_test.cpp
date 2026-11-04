// gobot_test -- the game's own map files (GameMapBook: the drop plan's §6) and
// which map the character is on (MapTrust: §6.3 and the player's answer 9 of
// 2026-11-04): floors read planar with their attribute, gates out of the
// .prj, what a session learns per map, and a map trusted by its entry's
// spawn, by standing points on its floors, or named from standing points
// when no entry was seen -- and let go of at the first point off its floors.
//
// The parsers on small made-up buffers; GameMapBook and MapTrust on made-up map
// files written to this run's own temp folder. Then, when they are there,
// copies of the game's own files in build\handoff\drop\testdata -- never the
// game's install, never the registry -- for the numbers data.md measured
// live. Nothing here presses a key.

#include "gobot/common.hpp"
#include "gobot/map_book.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
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

using Why = MapTrust::Why;
using How = MapTrust::How;

/// In this run's own temp folder, which main.cpp makes and removes.
std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

bool write_bytes(const std::string& path, const Bytes& bytes) {
    std::FILE* f = open_file(path, "wb");
    if (f == nullptr) return false;
    const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

/// A data folder of made-up files: <temp>\<name>\Map and \Project.
std::string data_folder(const char* name) {
    const std::string dir = temp_path(name);
    make_folder(dir);
    make_folder(dir + "\\Map");
    make_folder(dir + "\\Project");
    return dir;
}

/// A planar .map of w x h cells, every record empty as the game's are: bytes
/// 0-34 0xFF -- no surface, attribute 0xFF -- and byte 35 0.
struct MapMaker {
    int   w;
    int   h;
    Bytes b;

    MapMaker(int w_cells, int h_cells)
        : w(w_cells), h(h_cells),
          b(8 + static_cast<std::size_t>(w_cells) * static_cast<std::size_t>(h_cells) * 72, 0xFF) {
        std::memcpy(b.data(), &w, 4);
        std::memcpy(b.data() + 4, &h, 4);
        for (std::size_t r = 8; r < b.size(); r += 36) b[r + 35] = 0;
    }
    std::size_t rec(int layer, int cx, int cy) const {
        return 8 + static_cast<std::size_t>(layer) * static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 36 +
               (static_cast<std::size_t>(cx) * static_cast<std::size_t>(h) + static_cast<std::size_t>(cy)) * 36;
    }
    /// A surface pixel at x, y in `layer`, its cell's attribute `attr`.
    void dot(int layer, int x, int y, std::uint8_t attr) {
        const std::size_t r = rec(layer, x / 32, y / 32);
        b[r + 2] = attr;
        b[r + 3 + static_cast<std::size_t>(x % 32)] = static_cast<std::uint8_t>(y % 32);
    }
    void floor(int layer, int x0, int x1, const std::function<int(int)>& y_at, std::uint8_t attr) {
        for (int x = x0; x <= x1; ++x) dot(layer, x, y_at(x), attr);
    }
    void flat(int layer, int x0, int x1, int y, std::uint8_t attr) {
        floor(layer, x0, x1, [y](int) { return y; }, attr);
    }
};

/// A .prj as the game's: kinds [u32 4][i32 id][u32 type] and the sprite's
/// path, then placings of 37 bytes -- a name of 20, then [i32 kind][u8][f32
/// 1.0][i32 x][i32 y] -- among bytes that mean nothing here.
struct PrjMaker {
    Bytes b;

    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float v) {
        std::uint32_t u = 0;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
    void text(const char* s) {
        for (const char* p = s; *p; ++p) b.push_back(static_cast<std::uint8_t>(*p));
        b.push_back(0);
    }
    void junk(std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(0x5A + i * 7));
    }
    void kind(std::int32_t id, std::uint32_t type, const char* path) {
        u32(4);
        i32(id);
        u32(type);
        text(path);
    }
    /// A placing, its name the bytes given (EUC-KR in the game's) padded to 20.
    void place(std::initializer_list<std::uint8_t> name, std::int32_t kind, std::uint8_t flag, float scale,
               std::int32_t x, std::int32_t y) {
        std::size_t n = 0;
        for (std::uint8_t c : name) {
            b.push_back(c);
            ++n;
        }
        for (; n < 20; ++n) b.push_back(0);
        i32(kind);
        u8(flag);
        f32(scale);
        i32(x);
        i32(y);
    }
};

const std::initializer_list<std::uint8_t> kJeontu = { 0xC0, 0xFC, 0xC5, 0xF5, 0xC1, 0xB8 };   // 전투존, EUC-KR
const std::initializer_list<std::uint8_t> kPiseu = { 0xC7, 0xC7, 0xBD, 0xBA, 0xC1, 0xB8 };    // 피스존

bool floor_is(const std::optional<MapFloor>& f, int y, int attr) {
    return f && f->y == y && f->attr == attr;
}

std::string floors_text(std::span<const MapFloor> fl) {
    std::string s;
    for (const MapFloor& f : fl)
        s += fmt("%s%d+%da%dL%d", s.empty() ? "" : " ", f.y, f.depth, f.attr, f.layer);
    return s.empty() ? std::string("none") : s;
}

// ------------------------------------------------------------- the parsers

void test_parse_map() {
    section("GameMapBook: the .map read planar, its floors and their attribute");

    // 3 x 4 cells: 96 x 128 px.
    MapMaker m(3, 4);
    m.flat(0, 0, 63, 40, 1);                                    // a platform, layer 0
    m.flat(1, 48, 56, 41, 2);                                   // a solid row 1 px under it, layer 1
    m.flat(1, 5, 5, 43, 1);                                     // 3 px under it: a floor of its own
    m.flat(0, 0, 95, 120, 2);                                   // the bottom ground
    m.floor(1, 32, 63, [](int x) { return 70 + (x - 32) / 4; }, 1);   // a slope, layer 1
    m.b[m.rec(0, 2, 0) + 3 + 6] = 40;                           // x 70: a height past 31 is the cell's last row
    m.b[m.rec(0, 2, 0) + 2] = 1;

    const std::optional<MapFloors> f = GameMapBook::parse_map(m.b);
    check(f.has_value(), "a planar .map of 8 + 3 x 4 x 72 bytes reads");
    if (!f) return;
    check(f->w_cells == 3 && f->h_cells == 4 && f->w_px() == 96 && f->h_px() == 128 && f->start.size() == 97,
          fmt("3 x 4 cells, 96 x 128 px, a start per column and one past (got %dx%d, %zu)", f->w_cells,
              f->h_cells, f->start.size()));
    GameMapBook::Page page;
    page.stem = "made_up";
    page.floors = *f;
    const auto col = [&](int x) { return floors_text(page.floors_at(x)); };
    check(col(0) == "40+0a1L0 120+0a2L0", "x 0: the platform at 40 and the bottom at 120, top first: " + col(0));
    check(col(52) == "40+1a2L0 75+0a1L1 120+0a2L0",
          "x 52: a row 1 px under a floor's top is that floor -- 1 deep, and solid, as one of its rows is: " +
              col(52));
    check(col(5) == "40+0a1L0 43+0a1L1 120+0a2L0", "x 5: a row 3 px under the top is a floor of its own: " + col(5));
    check(col(40) == "40+0a1L0 72+0a1L1 120+0a2L0", "x 40: the slope in layer 1 at 72 between the two: " + col(40));
    check(col(70) == "31+0a1L0 120+0a2L0", "x 70: a height byte past 31 is row 31 of its cell: " + col(70));
    check(col(-1) == "none" && col(96) == "none", "no floors outside the map");

    check(floor_is(page.floor_near(0, 39), 40, 1), "standing 1 px above a floor is on it");
    check(floor_is(page.floor_near(0, 37), 40, 1) && floor_is(page.floor_near(0, 43), 40, 1),
          "3 px off a floor's row either way is on it");
    check(!page.floor_near(0, 36) && !page.floor_near(0, 44), "4 px off is on no floor");
    check(floor_is(page.floor_near(52, 44), 40, 2) && !page.floor_near(52, 45),
          "a floor 1 deep: on it to 3 px past its deepest row, not 4");
    check(floor_is(page.floor_near(5, 41), 40, 1), "near two floors 3 px apart, the top one is the one stood on");
    check(page.floor_near(0, 39)->platform() && !page.floor_near(52, 40)->platform() &&
              !page.floor_near(0, 119)->platform(),
          "attribute 1 is a platform; 2 -- the bottom, or merged in -- is not");
    check(floor_is(page.floor_below(0, 39, 8), 120, 2), "below the platform at x 0: the bottom");
    check(floor_is(page.floor_below(40, 39, 8), 72, 1), "below it at x 40: the slope first");
    check(!page.floor_below(0, 119, 8), "nothing below the bottom");
    check(floor_is(page.floor_below(5, 39, 3), 43, 1) && floor_is(page.floor_below(5, 39, 4), 120, 2),
          "below means more than `gap` under y");

    // Planar, not interleaved: one height byte in the layer-1 block's first
    // record is cell 0,0 -- read interleaved it would be another cell.
    MapMaker one(2, 2);
    one.b[8 + 4 * 36 + 2] = 1;
    one.b[8 + 4 * 36 + 3 + 0] = 9;
    const std::optional<MapFloors> p1 = GameMapBook::parse_map(one.b);
    check(p1 && floors_text(p1->at(0)) == "9+0a1L1" && p1->floors.size() == 1,
          "the layer-1 block's first record is cell 0,0 of layer 1: x 0, row 9 -- " +
              (p1 ? floors_text(p1->at(0)) : std::string("did not read")));

    MapMaker empty(1, 1);
    const std::optional<MapFloors> e = GameMapBook::parse_map(empty.b);
    check(e && e->floors.empty() && e->at(0).empty() && e->at(31).empty(), "a map with no surface has no floors");

    Bytes shorter(m.b.begin(), m.b.end() - 1);
    Bytes longer = m.b;
    longer.push_back(0);
    check(!GameMapBook::parse_map(shorter) && !GameMapBook::parse_map(longer),
          "a byte short or a byte over 8 + W x H x 72: not a map");
    Bytes head(m.b.begin(), m.b.begin() + 8);
    check(!GameMapBook::parse_map(head) && !GameMapBook::parse_map(Bytes{}) &&
              !GameMapBook::parse_map(Bytes{ 1, 2, 3 }),
          "a header alone, nothing, or 3 bytes: not a map");
    for (const std::pair<int, int>& wh : { std::pair<int, int>{ 0, 4 }, { 3, -1 }, { 4097, 1 }, { 1, 4097 } }) {
        Bytes bad = m.b;
        std::memcpy(bad.data(), &wh.first, 4);
        std::memcpy(bad.data() + 4, &wh.second, 4);
        check(!GameMapBook::parse_map(bad), fmt("%d x %d cells: not a map", wh.first, wh.second));
    }
}

void test_parse_prj() {
    section("GameMapBook: the gates of a .prj -- warp zones placed, nothing else");

    PrjMaker p;
    p.junk(40);
    // A list of the files the map uses: a pointer and a 4 before each path,
    // no id -- read as kinds, its "id" is the pointer, which nothing places.
    p.u32(0x02BC12B8);
    p.u32(4);
    p.text("\\Data\\OBJ\\Object\\warpzone.spr");
    p.junk(13);
    p.kind(1015201, 15, "\\Data\\OBJ\\Monster\\m015201.spr");
    p.kind(3900008, 2, "\\Data\\OBJ\\Object\\warpzone.spr");
    p.junk(5);
    p.kind(3900000, 2, "\\DATA\\obj\\OBJECT\\WarpZone.spr");
    p.kind(3900001, 2, "\\Data\\OBJ\\Object\\butterfly_1.spr");
    p.kind(3990001, 1, "\\Data\\OBJ\\Object\\ind_portal.spr");
    p.kind(3900009, 2, "\\Data\\OBJ\\Object\\warpzone.mot");     // a .mot is no kind
    p.junk(30);
    const std::size_t first = p.b.size();
    p.place(kJeontu, 3900008, 1, 1.0f, 2812, 622);
    p.place(kPiseu, 3900000, 1, 1.0f, 881, 267);
    p.place({ 'm' }, 1015201, 1, 1.0f, 500, 600);                 // a monster's spawn
    p.place({ 'b' }, 3900001, 1, 1.0f, 100, 100);                 // a butterfly
    p.place({ 'p' }, 3990001, 255, 1.0f, 50, 60);                 // a portal
    p.place({ 'w' }, 3900009, 1, 1.0f, 10, 10);                   // a kind only its .mot names
    p.place(kJeontu, 3900008, 1, 1.0f, 2812, 622);                // the same gate twice
    p.place(kPiseu, 3900000, 1, 2.0f, 999, 999);                  // not at scale 1
    p.junk(17);

    const std::vector<MapGate> g = GameMapBook::parse_prj(p.b);
    const auto text = [](const std::vector<MapGate>& v) {
        std::string s;
        for (const MapGate& x : v) s += fmt("%s%d@%d,%d", s.empty() ? "" : " ", x.kind, x.x, x.y);
        return s.empty() ? std::string("none") : s;
    };
    check(text(g) == "3900008@2812,622 3900000@881,267 3990001@50,60",
          "the two warp zones and the portal, in the file's order, each once -- not the monster, the butterfly, "
          "a kind only a .mot names, or a placing at another scale: " +
              text(g));

    Bytes cut(p.b.begin(), p.b.begin() + static_cast<std::ptrdiff_t>(first + 20 + 4 + 1 + 4 + 2));
    check(GameMapBook::parse_prj(cut).empty(), "cut in the middle of the first gate's x: no gates, no crash");
    Bytes kinds_only(p.b.begin(), p.b.begin() + static_cast<std::ptrdiff_t>(first));
    check(GameMapBook::parse_prj(kinds_only).empty(), "kinds but no placings: no gates");
    Bytes half(p.b.begin(), p.b.begin() + 70);
    check(GameMapBook::parse_prj(half).empty() && GameMapBook::parse_prj(Bytes{}).empty() &&
              GameMapBook::parse_prj(Bytes(5, 0x5C)).empty(),
          "cut inside the kinds, empty, or 5 backslashes: no gates");

    check(GameMapBook::stem_of(52, 2) == "t52_s2" && GameMapBook::stem_of(0, 0) == "t0_s0" &&
              GameMapBook::stem_of(1108, 3) == "t1108_s3",
          "a map entry's numbers 52/2 are the files' stem t52_s2");
}

// ------------------------------------------------------------ the files

void test_book_files() {
    section("GameMapBook: the files read once and kept, gone ones, and what a session learns");

    const std::string dir = data_folder("map_book_files");
    MapMaker a(4, 3);                                       // 128 x 96
    a.flat(0, 0, 127, 50, 1);
    a.floor(1, 0, 127, [](int x) { return 20 + x / 8; }, 1);
    a.flat(0, 0, 127, 90, 2);
    PrjMaker pa;
    pa.junk(9);
    pa.kind(3900008, 2, "\\Data\\OBJ\\Object\\warpzone.spr");
    pa.place(kJeontu, 3900008, 1, 1.0f, 64, 44);
    MapMaker b(2, 2);
    b.flat(0, 0, 63, 30, 1);
    Bytes garbled = a.b;
    garbled.resize(garbled.size() - 36);
    const bool wrote = write_bytes(dir + "\\Map\\a1_s1.map", a.b) && write_bytes(dir + "\\Project\\a1_s1.prj", pa.b) &&
                       write_bytes(dir + "\\Map\\a1_s2.map", b.b) && write_bytes(dir + "\\Map\\bad_s1.map", garbled) &&
                       write_bytes(dir + "\\Map\\Up_S1.MAP", b.b) && write_bytes(dir + "\\Map\\a1_s1.map_", b.b) &&
                       write_bytes(dir + "\\Map\\t9.s0.map", b.b) &&
                       write_bytes(dir + "\\Map\\notes.txt", Bytes{ 'x' });
    check(wrote, "made-up map files written to this run's temp folder");
    if (!wrote) return;

    GameMapBook book(dir);
    check(book.data_dir() == dir, "the folder given is the one used, never looked up");
    const std::vector<std::string> stems = book.stems();
    std::string listed;
    for (const std::string& s : stems) listed += (listed.empty() ? "" : " ") + s;
    check(listed == "Up_S1 a1_s1 a1_s2 bad_s1 t9.s0",
          "every .map by its stem, sorted, whatever the case, a dot in it too (the game has t58.s0.map) -- not a "
          ".map_ or a .txt: " +
              listed);
    check(book.map_files() == 5, "5 map files");

    const std::shared_ptr<const GameMapBook::Page> p1 = book.get("a1_s1");
    check(p1 && p1->stem == "a1_s1" && p1->floors.w_px() == 128 && p1->prj_read && p1->gates.size() == 1 &&
              p1->gates[0].x == 64 && p1->gates[0].y == 44 && p1->gates[0].kind == 3900008,
          "a1_s1: its floors, and the one gate its .prj places");
    check(book.get("a1_s1") == p1, "asked again: the same page, not read again");
    const std::shared_ptr<const GameMapBook::Page> p2 = book.get("a1_s2");
    check(p2 && !p2->prj_read && p2->gates.empty(), "a1_s2 with no .prj: its floors, no gates");
    check(!book.get("bad_s1") && !book.get("bad_s1"), "a .map 36 bytes short: no data, twice");
    check(!book.get("t9_s9") && !book.get("..\\Map\\a1_s1") && !book.get("") && !book.get("a1_s1.map"),
          "a map with no file, or a stem that is a path: no data");

    // The file read a few bytes a point says what the page says, everywhere.
    GameMapBook fresh(dir);
    std::vector<MapPoint> pts;
    for (int x = -2; x <= 130; x += 5)
        for (int y = -5; y <= 100; ++y) pts.push_back({ x, y });
    int differ = 0, on = 0;
    for (const MapPoint& q : pts) {
        const int by_file = fresh.misses("a1_s1", std::span<const MapPoint>(&q, 1));
        const bool by_page = p1 && p1->on_floor(q);
        differ += by_file == (by_page ? 0 : 1) ? 0 : 1;
        on += by_page ? 1 : 0;
    }
    check(differ == 0 && on > 100,
          fmt("on a floor or not, read from the file a few bytes at a time and from the whole page agree at all "
              "%zu points (%d on a floor; %d differ)",
              pts.size(), on, differ));
    const std::vector<MapPoint> some = { { 10, 49 }, { 10, 70 }, { 200, 49 }, { -1, 49 }, { 64, 27 }, { 64, 89 } };
    check(fresh.misses("a1_s1", some) == 3 && book.misses("a1_s1", some) == 3,
          "of 6 points, the 3 off the map or off its floors miss -- from the file and from the page");
    check(fresh.misses("bad_s1", some) == -1 && fresh.misses("a\\b", some) == -1,
          "a file that is no map, or a stem that is no file name: -1");
    check(book.misses("bad_s1", some) == -1,
          "the same asked of the book that tried it as a page (kept as none): the file says which -- -1");
    check(fresh.misses("t9_s9", some) == -2, "a file that cannot be opened: -2, whether it fits not known");
    {
        // Held open by another with no sharing, as an antivirus scan or the
        // game's patcher may: opened by nobody else meanwhile.
        const std::wstring locked = utf8_to_wide(dir + "\\Map\\a1_s1.map");
        const HANDLE h = CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
        GameMapBook other(dir);
        const int while_held = other.misses("a1_s1", some);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        check(h != INVALID_HANDLE_VALUE && while_held == -2 && other.misses("a1_s1", some) == 3,
              fmt("a1_s1.map held open with no sharing: -2 (%d); let go, 3 again", while_held));
    }
    check(fresh.misses("a1_s1", std::span<const MapPoint>()) == 0, "no points: none missed");

    // What a session learns, per map.
    book.learn_no_drop("a1_s1", { 100, 200 });
    check(book.no_drop_near("a1_s1", { 164, 224 }) && book.no_drop_near("a1_s1", { 36, 176 }),
          "a no-drop point covers 64 px either way in x and 24 in y");
    check(!book.no_drop_near("a1_s1", { 165, 200 }) && !book.no_drop_near("a1_s1", { 100, 225 }) &&
              !book.no_drop_near("a1_s2", { 100, 200 }),
          "not 65 px or 25 px off, nor on another map");
    check(book.unlearn_no_drop("a1_s1", { 100, 200 }) && !book.no_drop_near("a1_s1", { 100, 200 }) &&
              !book.unlearn_no_drop("a1_s1", { 100, 200 }),
          "taken back once, by its exact point");
    book.learn_gate("a1_s1", { 5, 6 });
    book.learn_gate("a1_s1", { 5, 6 });
    const std::vector<MapGate> gates = book.gates("a1_s1");
    check(gates.size() == 2 && gates[0].kind == 3900008 && gates[1].x == 5 && gates[1].y == 6 && gates[1].kind == 0,
          "a map's gates: its file's, then the one learned (kind 0), learned twice and listed once");
    check(book.gates("t9_s9").empty() && book.learned_gates("a1_s2").empty(), "no gates for a map with none");
    for (int i = 0; i <= static_cast<int>(GameMapBook::kLearnedMax); ++i) book.learn_no_drop("a1_s2", { i * 1000, 0 });
    check(!book.no_drop_near("a1_s2", { 0, 0 }) && book.no_drop_near("a1_s2", { 1000, 0 }),
          "past kLearnedMax points, the oldest goes");
    check(!book.distrusted("a1_s1"), "a map is not distrusted until a drop finds its file wrong");
    book.distrust("a1_s1");
    check(book.distrusted("a1_s1") && !book.distrusted("a1_s2"), "distrusted for the session, that map only");

    // No game installed: no files, never a crash.
    GameMapBook none(dir + "\\not_there");
    check(none.stems().empty() && none.map_files() == 0 && !none.get("a1_s1") && none.gates("a1_s1").empty() &&
              none.misses("a1_s1", some) == -2,
          "a data folder that is not there: no maps, no pages, no gates -- and no file to open");
    GameMapBook nothing{ std::string() };
    check(nothing.data_dir().empty() && nothing.stems().empty() && !nothing.get("t52_s2"),
          "no data folder at all: no maps -- and none looked up");
}

// --------------------------------------------------------------- trust

/// One pass after another of what the hunter sees: you at a new report each
/// stand(), a monster at a new frame each.
struct Feed {
    MapTrust::Look look;
    std::uint32_t  slot = 0;
    std::uint64_t  frames = 0;

    Feed(bool known, std::uint16_t map, std::uint16_t area, std::uint32_t epoch, int ex, int ey) {
        look.now_ms = 10000;
        look.capture = true;
        look.map_known = known;
        look.map = map;
        look.area = area;
        look.epoch = epoch;
        look.entry_x = ex;
        look.entry_y = ey;
    }
    void pass(MapTrust& t, std::uint32_t work = 60000) {
        look.now_ms += 60;
        t.update(look, work);
        look.mobs.clear();
    }
    /// You standing at x, y: a report of yours not seen before.
    void stand(MapTrust& t, int x, int y) {
        look.settled = true;
        look.rx = x;
        look.ry = y;
        look.report_ms = look.now_ms + 1;
        pass(t);
    }
    /// Monsters stopped at these points, each a new frame; you as you were.
    void mobs(MapTrust& t, std::initializer_list<MapPoint> at, std::uint32_t work = 60000) {
        for (const MapPoint& p : at) {
            MapTrust::Mob m;
            m.at = p;
            m.slot = ++slot;
            m.at_ms = ++frames;
            look.mobs.push_back(m);
        }
        pass(t, work);
    }
};

/// The made-up maps of the trust tests: t5_s1 a platform at 200 and the
/// bottom at 600, a gate at 300,192; t5_s2 the same with no .prj; t5_s3 the
/// same with a .prj that places no gate.
std::string trust_folder() {
    const std::string dir = data_folder("map_book_trust");
    MapMaker m(32, 20);                                     // 1024 x 640
    m.flat(0, 0, 1023, 200, 1);
    m.flat(0, 0, 1023, 600, 2);
    PrjMaker gate;
    gate.junk(3);
    gate.kind(3900000, 2, "\\Data\\OBJ\\Object\\warpzone.spr");
    gate.place(kPiseu, 3900000, 1, 1.0f, 300, 192);
    PrjMaker no_gate;
    no_gate.kind(3900000, 2, "\\Data\\OBJ\\Object\\warpzone.spr");
    no_gate.junk(40);
    const bool ok = write_bytes(dir + "\\Map\\t5_s1.map", m.b) && write_bytes(dir + "\\Project\\t5_s1.prj", gate.b) &&
                    write_bytes(dir + "\\Map\\t5_s2.map", m.b) && write_bytes(dir + "\\Map\\t5_s3.map", m.b) &&
                    write_bytes(dir + "\\Project\\t5_s3.prj", no_gate.b);
    return ok ? dir : std::string();
}

void test_trust_entry() {
    section("MapTrust: a map entry's file, trusted by its spawn or by standing points, and let go of");

    const std::string dir = trust_folder();
    check(!dir.empty(), "made-up maps for the trust written");
    if (dir.empty()) return;
    GameMapBook book(dir);

    {
        MapTrust t(&book);
        Feed f(true, 5, 1, 1, 302, 180);
        f.pass(t);
        check(t.trusted() && t.how() == How::Entry && t.stem() == "t5_s1" && t.page() && t.page()->stem == "t5_s1" &&
                  t.report().why == Why::Trusted,
              "map entry 302,180, 2 px and 12 px from the gate at 300,192: t5_s1 trusted at once");
        const std::vector<MapGate> learned = book.learned_gates("t5_s1");
        check(learned.size() == 1 && learned[0].x == 302 && learned[0].y == 180,
              "the entry's spawn is learned as a gate of the map");
        f.stand(t, 302, 180);
        check(t.trusted(), "you still at the entry's spawn -- maybe in the air -- is no standing point to miss");
        f.stand(t, 500, 199);
        f.mobs(t, { { 700, 198 }, { 40, 202 } });
        check(t.trusted(), "you and two monsters on its floors: still trusted");
        f.stand(t, 520, 330);
        check(!t.trusted() && !t.page() && t.how() == How::None && t.report().why == Why::Mismatch &&
                  t.report().miss.x == 520 && t.report().miss.y == 330,
              "you standing at 520,330, on no floor of its file: not trusted, and the point said");
        f.stand(t, 500, 199);
        f.stand(t, 600, 199);
        f.stand(t, 700, 199);
        check(!t.trusted() && t.report().why == Why::Mismatch, "points on its floors after that do not bring it back");
        t.lose("Tìm lại");
        f.stand(t, 800, 199);
        check(!t.trusted() && t.report().why == Why::Mismatch, "nor does lose(): it stays wrong for this entry");
        f.look.epoch = 2;
        f.pass(t);
        check(t.trusted() && t.how() == How::Entry, "a new map entry to the same map, by the gate: trusted again");
    }
    {
        MapTrust t(&book);
        Feed f(true, 5, 1, 7, 345, 192);
        f.pass(t);
        check(!t.trusted() && t.report().why == Why::Confirming && t.report().points == 0 && t.report().needed == 3,
              "map entry 345,192 -- 45 px from the gate: not by it -- standing points wanted, 0 of 3");
        f.stand(t, 100, 199);
        f.stand(t, 130, 199);
        f.stand(t, 150, 198);
        check(t.report().points == 1, "points within 64 px in x and 24 in y of one taken do not count");
        f.mobs(t, { { 164, 199 } });
        check(t.report().points == 2, "a monster's stop on its floors counts: 2 of 3");
        f.stand(t, 345, 192);
        check(t.report().points == 2, "you at the entry's spawn is not a standing point");
        f.stand(t, 228, 199);
        check(t.trusted() && t.how() == How::Points && t.report().why == Why::Trusted,
              "the third, 64 px from the others: trusted by standing points");
        t.lose("the plates' view changed");
        f.pass(t);
        check(!t.trusted() && t.report().why == Why::Confirming && t.report().points == 0,
              "lose(): no longer trusted, standing points wanted again");
        f.look.entry_x = 302;
        f.look.entry_y = 180;
        f.pass(t);
        check(!t.trusted(), "and the spawn by a gate is not had again for the same entry");
        f.stand(t, 100, 199);
        f.stand(t, 200, 199);
        f.stand(t, 300, 199);
        check(t.trusted() && t.how() == How::Points, "3 standing points: trusted again");
    }
    {
        MapTrust t(&book);
        Feed f(true, 5, 1, 3, 302, 180);
        f.pass(t);
        check(t.trusted(), "trusted by the entry");
        f.look.capture = false;
        f.pass(t);
        check(!t.trusted() && t.report().why == Why::NoCapture, "the capture stops: nothing trusted");
        f.look.capture = true;
        f.pass(t);
        check(!t.trusted() && t.report().why == Why::Confirming,
              "back in the same entry's stay: only standing points, not the spawn again");
    }
    {
        // Found wrong, then the capture lost and back within the same entry's
        // stay (a re-link keeps the entry): still wrong, as after lose().
        MapTrust t(&book);
        Feed f(true, 5, 1, 4, 302, 180);
        f.pass(t);
        f.stand(t, 520, 330);
        const bool wrong = !t.trusted() && t.report().why == Why::Mismatch;
        f.look.capture = false;
        f.pass(t);
        f.look.capture = true;
        f.pass(t);
        f.stand(t, 100, 199);
        f.stand(t, 200, 199);
        f.stand(t, 300, 199);
        check(wrong && !t.trusted() && t.report().why == Why::Mismatch && t.report().miss.x == 520 &&
                  t.report().miss.y == 330,
              fmt("a file found wrong, the capture lost and back in the same entry's stay: 3 standing points on its "
                  "floors do not bring it back (%s)",
                  MapTrust::why_text(t.report().why)));
        f.look.epoch = 5;
        f.pass(t);
        check(t.trusted() && t.how() == How::Entry, "a new map entry by the gate: trusted again");
    }
    {
        MapTrust t(&book);
        Feed f(true, 5, 2, 1, 302, 180);
        f.pass(t);
        check(!t.trusted() && t.report().why == Why::NoGates && t.stem() == "t5_s2",
              "t5_s2 with no .prj: no gates, not trusted");
        Feed g(true, 5, 3, 2, 302, 180);
        g.pass(t);
        check(!t.trusted() && t.report().why == Why::NoGates, "t5_s3 whose .prj places no gate: not trusted");
        Feed h(true, 5, 9, 3, 302, 180);
        h.pass(t);
        check(!t.trusted() && t.report().why == Why::NoFile && !t.page(), "t5_s9 with no .map: no file");
        Feed i(true, 5, 1, 4, 302, 180);
        i.pass(t);
        check(t.trusted(), "t5_s1 again: trusted");
        book.distrust("t5_s1");
        i.pass(t);
        check(!t.trusted() && t.report().why == Why::Distrusted, "a drop found its file wrong: no longer trusted");
        i.look.epoch = 5;
        i.pass(t);
        check(!t.trusted() && t.report().why == Why::Distrusted, "nor after a new entry, this session");
    }
    MapTrust unwired;
    Feed f(true, 5, 1, 1, 302, 180);
    f.pass(unwired);
    check(!unwired.trusted() && unwired.report().why == Why::NoBook, "no map book: nothing trusted");
}

/// The made-up maps of the naming tests, every stem t7_s<n>:
///   s1  a slope, y 300 + x/4 over x 0-1023, a gate, and the bottom at 700;
///   s2  the same slope to x 511, then 20 px lower, a gate;
///   s3  flat at 300; s4 128 px wide; s5 cut short.
std::string naming_folder() {
    const std::string dir = data_folder("map_book_naming");
    const auto slope = [](int x) { return 300 + x / 4; };
    MapMaker s1(32, 24);                                    // 1024 x 768
    s1.floor(0, 0, 1023, slope, 1);
    s1.flat(1, 0, 1023, 700, 2);
    MapMaker s2(32, 24);
    s2.floor(0, 0, 511, slope, 1);
    s2.floor(0, 512, 1023, [&](int x) { return slope(x) + 20; }, 1);
    MapMaker s3(32, 24);
    s3.flat(0, 0, 1023, 300, 1);
    MapMaker s4(4, 24);
    s4.floor(0, 0, 127, slope, 1);
    Bytes s5 = s1.b;
    s5.resize(100);
    PrjMaker gate;
    gate.kind(3900008, 2, "\\Data\\OBJ\\Object\\warpzone.spr");
    gate.place(kJeontu, 3900008, 1, 1.0f, 40, 300);
    const bool ok = write_bytes(dir + "\\Map\\t7_s1.map", s1.b) && write_bytes(dir + "\\Map\\t7_s2.map", s2.b) &&
                    write_bytes(dir + "\\Map\\t7_s3.map", s3.b) && write_bytes(dir + "\\Map\\t7_s4.map", s4.b) &&
                    write_bytes(dir + "\\Map\\t7_s5.map", s5) && write_bytes(dir + "\\Project\\t7_s1.prj", gate.b) &&
                    write_bytes(dir + "\\Project\\t7_s2.prj", gate.b);
    return ok ? dir : std::string();
}

/// On t7_s1's slope, 1 px above it as the packets put you.
MapPoint on_slope(int x) { return { x, 300 + x / 4 - 1 }; }

void test_trust_naming() {
    section("MapTrust: no map entry seen -- the map named from standing points, strictly");

    const std::string dir = naming_folder();
    check(!dir.empty(), "made-up maps for the naming written");
    if (dir.empty()) return;

    {
        GameMapBook book(dir);
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        for (int i = 0; i < 11; ++i) f.stand(t, on_slope(20 + i * 40).x, on_slope(20 + i * 40).y);
        MapTrust::Report r = t.report();
        check(!t.trusted() && r.why == Why::Naming && r.points == 11 && r.needed == 12 && r.files == 0,
              fmt("11 standing points: naming, 11 of 12, no file read yet (got %s, %d of %d, %zu files)",
                  MapTrust::why_text(r.why), r.points, r.needed, r.files));
        f.stand(t, on_slope(460).x, on_slope(460).y);
        r = t.report();
        check(!t.trusted() && r.why == Why::Ambiguous && r.fit == 2 && r.files == 5 && r.read == 5,
              fmt("12 points, all left of x 512, lie on t7_s1 and t7_s2 alike: not named (%s, %d fit, %zu of %zu "
                  "files read)",
                  MapTrust::why_text(r.why), r.fit, r.read, r.files));
        for (int i = 0; i < 5; ++i) f.mobs(t, { on_slope(540 + i * 60) });
        check(!t.trusted() && t.report().why == Why::Ambiguous && t.report().next_misses == 5,
              "5 monsters' stops right of 512: t7_s2 misses 5 -- still under the margin of 6");
        f.mobs(t, { on_slope(900) });
        r = t.report();
        check(t.trusted() && t.how() == How::Named && t.stem() == "t7_s1" && t.page() && r.why == Why::Trusted &&
                  r.next_best == "t7_s2" && r.next_misses == 6,
              fmt("the 6th: t7_s1 named, the next best t7_s2 missing 6 of 18 (got %s, next %s missing %d)",
                  t.stem().c_str(), r.next_best.c_str(), r.next_misses));

        f.mobs(t, { on_slope(1000) });
        f.stand(t, on_slope(10).x, on_slope(10).y);
        check(t.trusted() && t.stem() == "t7_s1", "more points on its floors: still named");
        f.mobs(t, { { 600, 500 } });
        r = t.report();
        check(!t.trusted() && t.stem().empty() && !t.page() && r.why == Why::Naming && r.points == 1,
              "a monster stopped at 600,500, on no floor of t7_s1: let go of, naming again from that point");
        for (int i = 0; i < 12; ++i) f.stand(t, on_slope(15 + i * 70).x, on_slope(15 + i * 70).y);
        r = t.report();
        check(!t.trusted() && r.why == Why::Naming && r.files == 0 && r.points == 13,
              "13 points within 30 s of the last full read: every file is not read again yet");
        f.look.now_ms += MapTrust::kNameRescanMs;
        f.pass(t);
        check(!t.trusted() && t.report().why == Why::Naming && t.report().points == 0,
              "30 s on, read again: the point off t7_s1's floors is on no file's either -- naming starts again");
    }
    {
        // t7_s1's file held open with no sharing as the 12 points come: the
        // one other file they fit, t7_s2, is NOT named -- t7_s1 might fit too.
        // Let go of, it is read with all the points, and both fit.
        GameMapBook book(dir);
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        const std::wstring locked = utf8_to_wide(dir + "\\Map\\t7_s1.map");
        const HANDLE h = CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
        for (int i = 0; i < 11; ++i) f.stand(t, on_slope(20 + i * 40).x, on_slope(20 + i * 40).y);
        f.stand(t, on_slope(460).x, on_slope(460).y);
        f.pass(t);
        const MapTrust::Report held = t.report();
        const bool named = t.trusted() || !t.stem().empty();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        f.pass(t);
        const MapTrust::Report r = t.report();
        check(h != INVALID_HANDLE_VALUE && !named && held.why == Why::Naming && held.read == 4 &&
                  r.why == Why::Ambiguous && r.fit == 2 && !t.trusted(),
              fmt("t7_s1.map not readable: t7_s2 alone fits, and is not named (%s, %zu of %zu read); readable "
                  "again, both fit (%s, %d fit)",
                  MapTrust::why_text(held.why), held.read, held.files, MapTrust::why_text(r.why), r.fit));
    }
    {
        // Every file read at its own update: one at least each time.
        GameMapBook book(dir);
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        for (int i = 0; i < 11; ++i) f.stand(t, on_slope(20 + i * 90).x, on_slope(20 + i * 90).y);
        f.look.settled = true;
        f.look.rx = on_slope(1010).x;
        f.look.ry = on_slope(1010).y;
        f.look.report_ms = f.look.now_ms + 1;
        int updates = 0;
        while (!t.trusted() && updates < 20) {
            f.pass(t, 0);
            ++updates;
        }
        check(t.trusted() && t.stem() == "t7_s1" && updates == 5,
              fmt("with no time to spend, one file an update: named at the 5th of 5 files (%d updates)", updates));
    }
    {
        GameMapBook book(dir);
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        for (int i = 0; i < 20; ++i) f.stand(t, 100, 324);
        f.stand(t, 104, 326);
        f.stand(t, 100, 327);
        check(t.report().points == 1, "the same point 22 times, or 4 px and 3 px off it: one point");
        f.mobs(t, { { 108, 326 } });
        f.mobs(t, { { 100, 328 } });
        check(t.report().points == 3, "8 px off in x, or 4 in y: points of their own");
        MapTrust::Mob same;
        same.at = { 500, 424 };
        same.slot = 77;
        same.at_ms = 5;
        f.look.mobs = { same };
        f.pass(t);
        f.look.mobs = { same };
        f.pass(t);
        check(t.report().points == 4, "a monster's frame seen twice is one point");
        f.look.settled = true;
        f.look.rx = 700;
        f.look.ry = 474;
        f.look.report_ms = 99;
        f.pass(t);
        f.look.rx = 800;
        f.pass(t);
        check(t.report().points == 5, "a report of yours seen twice is one point");
    }
    {
        // A real map entry always wins over a name.
        GameMapBook book(dir);
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        for (int i = 0; i < 12; ++i) f.stand(t, on_slope(30 + i * 85).x, on_slope(30 + i * 85).y);
        check(t.trusted() && t.stem() == "t7_s1" && t.how() == How::Named, "12 points across the slope: t7_s1 named");
        f.look.map_known = true;
        f.look.map = 7;
        f.look.area = 2;
        f.look.epoch = 1;
        f.look.entry_x = 42;
        f.look.entry_y = 290;
        f.pass(t);
        check(t.trusted() && t.stem() == "t7_s2" && t.how() == How::Entry,
              "then a map entry to t7_s2 by its gate: t7_s2, trusted by the entry -- the name gone");
        f.look.capture = false;
        f.pass(t);
        f.look.capture = true;
        f.look.map_known = false;
        f.pass(t);
        check(!t.trusted() && t.stem().empty() && t.report().why == Why::Naming,
              "the capture started again -- no entry since: naming from nothing");
    }
    {
        // A point no file has: naming starts again.
        GameMapBook book(dir);
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        for (int i = 0; i < 11; ++i) f.stand(t, on_slope(30 + i * 85).x, on_slope(30 + i * 85).y);
        f.mobs(t, { { 990, 100 } });
        check(!t.trusted() && t.report().why == Why::Naming && t.report().points == 0,
              "12 points, one of them on no file's floors: no file fits them all -- naming starts again");
    }
    {
        // Named, but its .prj places no gate, or the session distrusts it.
        GameMapBook book(dir);
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        for (int i = 0; i < 12; ++i) f.stand(t, 3 + i * 10, 300);
        check(!t.trusted() && t.stem() == "t7_s3" && t.report().why == Why::NoGates,
              fmt("12 points on t7_s3's flat floor at 300: named, but with no .prj it has no gate (got %s, %s)",
                  t.stem().c_str(), MapTrust::why_text(t.report().why)));
        book.distrust("t7_s1");
        MapTrust u(&book);
        Feed g(false, 0, 0, 0, 0, 0);
        for (int i = 0; i < 12; ++i) g.stand(u, on_slope(30 + i * 85).x, on_slope(30 + i * 85).y);
        check(!u.trusted() && u.stem() == "t7_s1" && u.report().why == Why::Distrusted,
              "t7_s1 named, but distrusted this session: not trusted");
    }
    {
        GameMapBook book(dir + "\\not_there");
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        for (int i = 0; i < 12; ++i) f.stand(t, i * 50, 300);
        check(!t.trusted() && t.report().why == Why::NoFiles, "no map files at all: nothing named");
    }
}

// --------------------------------------------------- the game's own files

/// The game's files copied for the tests, or "" -- never the game's install.
std::string game_data() {
#ifdef GOBOT_SOURCE_DIR
    const std::string dir = std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data";
    if (path_exists(dir + "\\Map\\t52_s2.map") && path_exists(dir + "\\Project\\t52_s2.prj") &&
        path_exists(dir + "\\Map\\t52_s1.map") && path_exists(dir + "\\Project\\t52_s1.prj"))
        return dir;
#endif
    return {};
}

std::string gates_text(const std::vector<MapGate>& g) {
    std::string s;
    for (const MapGate& x : g) s += fmt("%s%d,%d", s.empty() ? "" : " ", x.x, x.y);
    return s;
}

/// Where data.md had the character and the monsters standing, live.
const std::vector<MapPoint> kT52s2Points = {
    // L06, you, 06:44:20-06:49:40 (06:49:50's 2579,315 was in the air)
    { 822, 608 }, { 990, 681 }, { 893, 640 }, { 1035, 560 }, { 1299, 390 }, { 1821, 360 }, { 1803, 360 },
    { 1794, 360 }, { 2336, 239 }, { 2180, 357 }, { 2171, 358 }, { 2297, 234 }, { 2567, 367 }, { 2513, 237 },
    { 2937, 417 }, { 2929, 415 }, { 2675, 371 }, { 2691, 374 }, { 2500, 237 }, { 2482, 237 }, { 2563, 367 },
    // L06, monsters' stops
    { 571, 578 }, { 557, 576 }, { 375, 533 }, { 847, 618 },
    // L23, you
    { 567, 577 }, { 558, 577 }, { 515, 569 }, { 506, 567 }, { 521, 570 }, { 1024, 691 }, { 1046, 698 },
};
/// The 12 data.md named t52_s1 by: you 9 times (gobot_gui_20261103_2224.log
/// :71-338), then three monsters' stops (:55, :70, :81).
const std::vector<MapPoint> kT52s1Own = { { 434, 2266 }, { 440, 2268 }, { 431, 2265 }, { 415, 2260 }, { 421, 2262 },
                                          { 413, 2260 }, { 402, 2259 }, { 427, 2264 }, { 460, 2269 } };
const std::initializer_list<MapPoint> kT52s1Mobs = { { 501, 2259 }, { 288, 2246 }, { 779, 2271 } };

void test_game_files() {
    section("GameMapBook and MapTrust on the game's own files (copies in build\\handoff\\drop\\testdata)");

    const std::string dir = game_data();
    if (dir.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata)\n");
        return;
    }
    GameMapBook book(dir);
    const std::size_t files = book.map_files();
    const std::vector<std::string> stems = book.stems();
    check(files >= 781 && std::find(stems.begin(), stems.end(), "t53_s4") != stems.end(),
          fmt("the game's .map files listed: %zu (781 shipped)", files));

    const std::shared_ptr<const GameMapBook::Page> s2 = book.get("t52_s2");
    check(s2 && s2->floors.w_cells == 100 && s2->floors.h_cells == 40 && s2->floors.w_px() == 3200 &&
              s2->floors.h_px() == 1280,
          "t52_s2: 100 x 40 cells, 3200 x 1280 px");
    if (!s2) return;
    check(gates_text(s2->gates) == "2812,622 881,267" && s2->gates[0].kind == 3900008 &&
              s2->gates[1].kind == 3900000,
          "t52_s2's gates: 전투존 2812,622 and 피스존 881,267 -- " + gates_text(s2->gates));
    const struct {
        const char* stem;
        const char* gates;
    } kGates[] = {
        { "t52_s1", "68,1706 1200,2298 243,729 1050,1710" },
        { "t53_s2", "1075,1055 2631,1182 4544,859 2020,1185" },
        { "t1_s4", "4650,1420 175,1070 240,1420" },
    };
    for (const auto& g : kGates) {
        const std::shared_ptr<const GameMapBook::Page> p = book.get(g.stem);
        check(p && gates_text(p->gates) == g.gates,
              fmt("%s's gates as data.md listed them: %s", g.stem, p ? gates_text(p->gates).c_str() : "no page"));
    }

    // 06:48:49: you at 2675,371, the monster at about 2673,638.
    check(floor_is(s2->floor_near(2675, 371), 372, 1) && s2->floor_near(2675, 371)->platform(),
          "06:48:49: you at 2675,371 stood on floor 372, a platform");
    const std::optional<MapFloor> land = s2->floor_below(2675, 371, 8);
    const std::optional<MapFloor> land_l = s2->floor_below(2667, 371, 8);
    const std::optional<MapFloor> land_r = s2->floor_below(2683, 373, 8);
    check(floor_is(land, 633, 1) && floor_is(land_l, 635, 1) && floor_is(land_r, 631, 1),
          fmt("a drop there lands on 633 -- 635 and 631 at 8 px either side, the monster's floor (got %d, %d, %d)",
              land ? land->y : -1, land_l ? land_l->y : -1, land_r ? land_r->y : -1));
    check(std::abs(s2->gates[0].x - 2675) == 137, "and the 전투존 gate stands 137 px to the side");
    // 06:48:26: you at 2513,237, the monster at about 2556,633.
    check(floor_is(s2->floor_near(2513, 237), 238, 1) && floor_is(s2->floor_below(2513, 237, 8), 375, 1),
          "06:48:26: from floor 238 at 2513 a drop lands on 375 first");
    check(floor_is(s2->floor_below(2513, 374, 8), 628, 1), "and from 375 on 628, by the monster");
    check(floor_is(s2->floor_near(2545, 370), 371, 1) && floor_is(s2->floor_below(2545, 370, 8), 984, 1),
          "at x 2545 the floor below 371 is 984: past the monster");
    // The 전투존 gate's own floor, and the bottom.
    const std::optional<MapFloor> gate_floor = s2->floor_near(2812, 622 + 3);
    check(floor_is(gate_floor, 626, 1) && gate_floor->layer == 0 && floor_is(s2->floor_below(2812, 625, 8), 937, 1),
          "the 전투존 gate at 2812,622 stands on floor 626 (layer 0, a platform), 937 below it");
    const std::optional<MapFloor> bottom = s2->floor_below(2812, 936, 8);
    check(floor_is(bottom, 1120, 2) && !bottom->platform() && !s2->floor_below(2812, 1119, 8),
          "under that the bottom ground, 1120, solid, nothing under it");

    int on = 0, exact = 0;
    std::string off;
    for (const MapPoint& p : kT52s2Points) {
        const std::optional<MapFloor> fl = s2->floor_near(p.x, p.y);
        on += fl ? 1 : 0;
        exact += fl && fl->y - p.y >= 0 && fl->y - p.y <= 1 ? 1 : 0;
        if (!fl) off += fmt(" %d,%d", p.x, p.y);
    }
    check(on == static_cast<int>(kT52s2Points.size()) && exact == on,
          fmt("all %zu live standing points of t52_s2 lie on a floor, 0-1 px above it (%d on, %d exact;%s)",
              kT52s2Points.size(), on, exact, off.empty() ? " none off" : off.c_str()));
    check(!s2->floor_near(2579, 315), "06:49:50's 2579,315, in the air, is on none");

    const std::shared_ptr<const GameMapBook::Page> s1 = book.get("t52_s1");
    if (!s1) return;
    int on1 = 0;
    for (const MapPoint& p : kT52s1Own) {
        const std::optional<MapFloor> fl = s1->floor_near(p.x, p.y);
        on1 += fl && fl->y - p.y == 1 && fl->attr == 2 ? 1 : 0;
    }
    for (const MapPoint& p : kT52s1Mobs) {
        const std::optional<MapFloor> fl = s1->floor_near(p.x, p.y);
        on1 += fl && fl->y - p.y == 1 && fl->attr == 2 ? 1 : 0;
    }
    check(on1 == 12, fmt("t52_s1's 12 live points lie 1 px above its bottom ground, solid (%d of 12)", on1));
    check(!s1->floor_below(430, 2265, 8), "nothing below the bottom ground the hunt of 22:36 was on");

    // data.md's 12 points against every one of the game's map files, as
    // map_find.py matched them: on t52_s1's floors alone.
    {
        std::vector<MapPoint> all = kT52s1Own;
        all.insert(all.end(), kT52s1Mobs.begin(), kT52s1Mobs.end());
        const auto t0 = std::chrono::steady_clock::now();
        std::string fit, next;
        int fits = 0, next_misses = 99, unread = 0;
        for (const std::string& s : stems) {
            const int m = book.misses(s, all);
            if (m < 0) {
                ++unread;
            } else if (m == 0) {
                ++fits;
                fit = s;
            } else if (m < next_misses) {
                next_misses = m;
                next = s;
            }
        }
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("         data.md's 12 points: %d file(s) fit (%s); the next best %s misses %d; %d unread; "
                    "%zu files read in %lld ms\n",
                    fits, fit.c_str(), next.c_str(), next_misses, unread, stems.size(), ms);
        check(fits == 1 && fit == "t52_s1" && next_misses == 8 && unread == 0,
              fmt("data.md's 12 points lie on t52_s1 alone of %zu files, the next best fitting 4 of them (%d fit, "
                  "next %s missing %d)",
                  stems.size(), fits, next.c_str(), next_misses));

        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        for (const MapPoint& p : kT52s1Own) f.stand(t, p.x, p.y);
        f.mobs(t, kT52s1Mobs);
        check(!t.trusted() && t.report().why == Why::Naming && t.report().points == 7,
              fmt("but 9 reports of yours within 60 px are 4 points 8 px or 4 px apart: with the 3 monsters, %d of "
                  "12 -- not named yet",
                  t.report().points));
    }
    // The first 12 points apart in the log of 2026-11-03 22:36, in the order
    // they came (gobot_gui_20261103_2224.log :55-:407).
    {
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        f.mobs(t, { { 501, 2259 }, { 413, 2260 } });
        f.mobs(t, { { 288, 2246 } });
        f.stand(t, 434, 2266);
        f.mobs(t, { { 779, 2271 } });
        for (const MapPoint& p : { MapPoint{ 440, 2268 }, MapPoint{ 431, 2265 }, MapPoint{ 415, 2260 },
                                   MapPoint{ 421, 2262 }, MapPoint{ 413, 2260 }, MapPoint{ 420, 2261 },
                                   MapPoint{ 402, 2259 }, MapPoint{ 442, 2269 }, MapPoint{ 460, 2269 },
                                   MapPoint{ 469, 2269 }, MapPoint{ 451, 2269 } })
            f.stand(t, p.x, p.y);
        check(!t.trusted() && t.report().points == 11, fmt("22:43:59: 11 points apart (%d)", t.report().points));
        const auto t0 = std::chrono::steady_clock::now();
        f.stand(t, 485, 2266);
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        const MapTrust::Report r = t.report();
        std::printf("         22:44:49: %s named, the next best %s missing %d; every file read in %lld ms\n",
                    t.stem().c_str(), r.next_best.c_str(), r.next_misses, ms);
        check(t.trusted() && t.how() == How::Named && t.stem() == "t52_s1" && r.next_best == "t23_s13" &&
                  r.next_misses == 6,
              fmt("22:44:49, the 12th: t52_s1 named, the only one of %zu files they all lie on; the next best, "
                  "t23_s13, misses 6 of them -- the margin (got %s, %s missing %d)",
                  files, t.stem().c_str(), r.next_best.c_str(), r.next_misses));
        f.stand(t, 1821, 360);
        check(!t.trusted() && t.stem().empty() && t.report().why == Why::Naming,
              "then you at 1821,360 -- a point of t52_s2, on no floor of t52_s1: let go of");
    }
    {
        // t52_s2 named by its live points the same way.
        MapTrust t(&book);
        Feed f(false, 0, 0, 0, 0, 0);
        for (const MapPoint& p : kT52s2Points) f.stand(t, p.x, p.y);
        const MapTrust::Report r = t.report();
        check(t.trusted() && t.stem() == "t52_s2" && r.next_misses >= MapTrust::kNameMarginMisses,
              fmt("t52_s2's live points name t52_s2 (next best %s missing %d)", r.next_best.c_str(), r.next_misses));
    }
    {
        // A map entry's numbers naming the wrong file: the first point off it.
        MapTrust t(&book);
        Feed f(true, 52, 2, 1, 880, 274);
        f.pass(t);
        check(t.trusted() && t.how() == How::Entry && t.stem() == "t52_s2",
              "map entry 52/2 at 880,274 -- 1 px and 7 px from the 피스존 gate: t52_s2 trusted");
        f.stand(t, 434, 2266);
        check(!t.trusted() && t.report().why == Why::Mismatch,
              "you at 434,2266 -- t52_s1's ground, not t52_s2's: wrong file");
        const struct {
            std::uint16_t map, area;
            int           x, y;
        } kEntries[] = { { 53, 2, 2635, 1188 }, { 1, 4, 4650, 1400 } };
        std::uint32_t epoch = 2;
        for (const auto& e : kEntries) {
            Feed g(true, e.map, e.area, epoch++, e.x, e.y);
            g.pass(t);
            check(t.trusted() && t.how() == How::Entry,
                  fmt("map entry %u/%u at %d,%d, as live: trusted by the gate it came in by",
                      static_cast<unsigned>(e.map), static_cast<unsigned>(e.area), e.x, e.y));
        }
    }
}

} // namespace

void test_map_book(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_parse_map();
    test_parse_prj();
    test_book_files();
    test_trust_entry();
    test_trust_naming();
    test_game_files();
}
