// gobot_test -- the "Bản đồ" tab's core (2026.11.04c): the floors drawn as
// runs (map_sketch), the gate's no-drop marks against the drop rule itself,
// the canvas's camera, the hunting zone -- its grips, a drag through a frozen
// camera, the home snapped to a floor, the checks and warnings, spots.ini and
// its save queue, and the owner's three answers each in one place -- which map
// the tab knows (map_identity, usable_identity) and the worker's job
// (MapViewJob::run). And Phase 0: MapTrust's log label, the named stem of a
// map with no gate, and packet_stands. And the package: spots.ini never in
// it, and the notes saying the owner's answers as the code has them. And
// 2026.11.05's dots (mob_dot, you_dot): a walker on its floor, a spawn set
// down, the dead hidden, you on your floor -- on t52_s2's own ramps too; none
// with nothing to place them by, and the grey on the hunter's own heights;
// and the notes saying so with the code's own numbers. And 2026.11.05b's
// notes -- the jump up and the seek -- against the code and t52_s2's own
// take-offs.
//
// Maps made up in memory, or written to this run's own temp folder; then,
// when they are there, copies of the game's files in build\handoff\drop\
// testdata -- never the game's install, never the registry. Nothing here
// presses a key or touches the desktop.

#include "gobot/alerts.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/hunt_seek.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_book.hpp"
#include "gobot/map_sketch.hpp"
#include "gobot/map_view.hpp"
#include "gobot/mob_place.hpp"
#include "gobot/packet_eyes.hpp"
#include "gobot/packet_world.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
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
using By = KnownMap::By;
using P = MapCamera::P;

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

bool write_text(const std::string& path, const std::string& text) {
    return write_bytes(path, Bytes(text.begin(), text.end()));
}

std::string read_text(const std::string& path) {
    std::FILE* f = open_file(path, "rb");
    if (f == nullptr) return {};
    std::string s;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

/// The lines `run` logs at INFO and above.
std::vector<std::string> info_lines(const std::function<void()>& run) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(4000);
    log.clear_ring();
    log.set_level(LogLevel::Info);
    run();
    std::vector<std::string> out;
    for (const LogLine& l : log.ring()) out.push_back(l.text);
    log.set_level(was);
    log.set_ring_capacity(0);
    return out;
}

int lines_with(const std::vector<std::string>& all, const char* a) {
    int n = 0;
    for (const std::string& l : all)
        if (l.find(a) != std::string::npos) ++n;
    return n;
}

// ------------------------------------------------------- made-up maps

/// A map's floors made column by column, as parse_map would leave them.
struct Floors {
    int w_cells;
    int h_cells;
    std::vector<std::vector<MapFloor>> cols;

    Floors(int wc, int hc) : w_cells(wc), h_cells(hc), cols(static_cast<std::size_t>(wc) * 32) {}
    void run(int x0, int x1, const std::function<int(int)>& y_at, std::uint8_t attr) {
        for (int x = x0; x <= x1; ++x) {
            MapFloor f;
            f.y = y_at(x);
            f.attr = attr;
            cols[static_cast<std::size_t>(x)].push_back(f);
        }
    }
    void flat(int x0, int x1, int y, std::uint8_t attr) {
        run(x0, x1, [y](int) { return y; }, attr);
    }
    MapFloors make() const {
        MapFloors m;
        m.w_cells = w_cells;
        m.h_cells = h_cells;
        for (std::vector<MapFloor> c : cols) {
            m.start.push_back(static_cast<std::uint32_t>(m.floors.size()));
            std::sort(c.begin(), c.end(), [](const MapFloor& a, const MapFloor& b) { return a.y < b.y; });
            m.floors.insert(m.floors.end(), c.begin(), c.end());
        }
        m.start.push_back(static_cast<std::uint32_t>(m.floors.size()));
        return m;
    }
    GameMapBook::Page page(const char* stem, std::vector<MapGate> gates = {}) const {
        GameMapBook::Page p;
        p.stem = stem;
        p.floors = make();
        p.prj_read = true;
        p.gates = std::move(gates);
        return p;
    }
};

MapGate gate_at(int x, int y) {
    MapGate g;
    g.x = x;
    g.y = y;
    g.kind = 3900000;
    return g;
}

/// A planar .map of w x h cells, every record empty as the game's are.
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
    void flat(int layer, int x0, int x1, int y, std::uint8_t attr) {
        for (int x = x0; x <= x1; ++x) {
            const std::size_t r = rec(layer, x / 32, y / 32);
            b[r + 2] = attr;
            b[r + 3 + static_cast<std::size_t>(x % 32)] = static_cast<std::uint8_t>(y % 32);
        }
    }
};

/// A .prj placing one gate, as the game's: its kind, then the placing.
Bytes prj_with_gate(std::int32_t x, std::int32_t y) {
    Bytes b;
    const auto u32 = [&b](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const auto text = [&b](const char* s) {
        for (const char* p = s; *p; ++p) b.push_back(static_cast<std::uint8_t>(*p));
        b.push_back(0);
    };
    u32(4);
    u32(3900000);
    u32(2);
    text("\\Data\\OBJ\\Object\\warpzone.spr");
    for (int i = 0; i < 20; ++i) b.push_back(0);
    u32(3900000);
    b.push_back(1);
    float one = 1.0f;
    std::uint32_t u = 0;
    std::memcpy(&u, &one, 4);
    u32(u);
    u32(static_cast<std::uint32_t>(x));
    u32(static_cast<std::uint32_t>(y));
    return b;
}

/// A tb_Map.tbl as MapTable reads it: "Ghost" and a header of 32 bytes, the
/// columns' types and names, then the rows -- each text masked and backwards.
Bytes table_bytes(std::initializer_list<MapTable::Entry> rows) {
    Bytes b = { 'G', 'h', 'o', 's', 't' };
    b.resize(32, 0);
    const auto u32 = [&b](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const auto text = [&b](const std::string& s) {
        b.push_back(static_cast<std::uint8_t>(s.size() & 0xFF));
        b.push_back(static_cast<std::uint8_t>(s.size() >> 8));
        for (std::size_t i = 0; i < s.size(); ++i)
            b.push_back(static_cast<std::uint8_t>(static_cast<std::uint8_t>(s[s.size() - 1 - i]) ^ MapTable::kMask));
    };
    u32(3);
    u32(6);
    u32(6);
    u32(7);
    u32(static_cast<std::uint32_t>(rows.size()));
    text("nTheme");
    text("nStage");
    text("szStageName");
    for (const MapTable::Entry& e : rows) {
        u32(static_cast<std::uint32_t>(e.theme));
        u32(static_cast<std::uint32_t>(e.stage));
        text(e.name);
    }
    return b;
}

/// The made-up data folder of the identity and job tests: t8_s1 a platform at
/// 200 with a gate at 300,192; t8_s2 one at 300 with a gate at 300,292; t8_s3
/// one at 400 and no .prj -- each with the bottom at 600; 1024 x 640.
std::string tab_maps() {
    const std::string dir = temp_path("map_tab_maps");
    make_folder(dir);
    make_folder(dir + "\\Map");
    make_folder(dir + "\\Project");
    bool ok = true;
    const int ys[] = { 200, 300, 400 };
    for (int i = 0; i < 3; ++i) {
        MapMaker m(32, 20);
        m.flat(0, 0, 1023, ys[i], 1);
        m.flat(0, 0, 1023, 600, 2);
        ok = ok && write_bytes(dir + "\\Map\\t8_s" + std::to_string(i + 1) + ".map", m.b);
    }
    ok = ok && write_bytes(dir + "\\Project\\t8_s1.prj", prj_with_gate(300, 192)) &&
         write_bytes(dir + "\\Project\\t8_s2.prj", prj_with_gate(300, 292));
    return ok ? dir : std::string();
}

/// One pass after another of what the hunter sees, for a MapTrust of the
/// tests' own: you at a new settled report each stand().
struct Feed {
    MapTrust::Look look;

    Feed() {
        look.now_ms = 10000;
        look.capture = true;
    }
    void stand(MapTrust& t, int x, int y) {
        look.now_ms += 60;
        look.settled = true;
        look.rx = x;
        look.ry = y;
        look.report_ms = look.now_ms;
        t.update(look, 60000);
    }
};

/// The game's files copied for the tests, or "" -- never the game's install.
std::string game_data() {
#ifdef GOBOT_SOURCE_DIR
    const std::string dir = std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data";
    if (path_exists(dir + "\\Map\\t52_s2.map") && path_exists(dir + "\\Map\\t52_s1.map")) return dir;
#endif
    return {};
}

// ------------------------------------------------------------- Phase 0

void test_phase0() {
    section("Map tab, Phase 0: packet_stands, MapTrust's log label, a map with no gate named all the same");

    {
        PacketWorld::View v;
        const auto mob = [&v](std::uint32_t slot, float x, float y) -> PacketWorld::Mob& {
            PacketWorld::Mob m;
            m.slot = slot;
            m.x = x;
            m.y = y;
            m.hp = m.max_hp = 100;
            m.event_ms = 5000 + slot;
            m.gen = slot * 10;
            v.mobs.push_back(m);
            return v.mobs.back();
        };
        mob(1, 100.4f, 200.6f);
        mob(2, 110.0f, 200.0f).dead = true;
        mob(3, 120.0f, 200.0f).moving = true;
        mob(4, 130.0f, 200.0f).speed = 7.0f;
        mob(5, 140.0f, 199.0f);
        const std::vector<MapTrust::Mob> s = packet_stands(v);
        check(s.size() == 2 && s[0].at.x == 100 && s[0].at.y == 201 && s[0].slot == 1 && s[0].at_ms == 5001 &&
                  s[0].gen == 10 && s[1].slot == 5,
              fmt("packet_stands: the living monsters standing, rounded, with the frame's time and generation -- "
                  "never one dead, walking or with a blow's speed (%zu)",
                  s.size()));
        for (std::uint32_t i = 0; i < 80; ++i) mob(100 + i, 300.0f, 200.0f);
        check(packet_stands(v).size() == PacketEyes::kStandsMax && packet_stands(v, 3).size() == 3,
              "at most kStandsMax of them, or the max asked for");
    }

    const std::string dir = tab_maps();
    check(!dir.empty(), "made-up maps for the map tab written");
    if (dir.empty()) return;
    {
        GameMapBook book(dir);
        MapTrust hunter(&book);
        MapTrust tab(&book, "Map view");
        Feed a, b;
        const std::vector<std::string> log = info_lines([&] {
            for (int i = 0; i < 12; ++i) {
                a.stand(hunter, 40 + i * 70, 199);
                b.stand(tab, 40 + i * 70, 199);
            }
        });
        check(hunter.trusted() && tab.trusted() && tab.stem() == "t8_s1" &&
                  lines_with(log, "Hunter: map named from 12 standing points: t8_s1") == 1 &&
                  lines_with(log, "Map view: map named from 12 standing points: t8_s1") == 1,
              "MapTrust's lines begin with whose it is: \"Hunter:\" by default, \"Map view:\" for the tab's own");
        const MapTrust::Report r = tab.report();
        check(r.named && r.how == MapTrust::How::Named && r.why == Why::Trusted, "named and trusted: Report::named");
    }
    {
        GameMapBook book(dir);
        MapTrust t(&book, "Map view");
        Feed f;
        for (int i = 0; i < 11; ++i) f.stand(t, 40 + i * 70, 399);
        check(!t.report().named && t.report().why == Why::Naming, "11 points: naming -- not named");
        f.stand(t, 900, 399);
        const MapTrust::Report r = t.report();
        check(!t.trusted() && r.why == Why::NoGates && r.how == MapTrust::How::None && r.named && r.stem == "t8_s3",
              fmt("12 points on t8_s3, which has no .prj: how() None as before, but Report::named and its stem kept "
                  "(%s, %s)",
                  MapTrust::why_text(r.why), r.stem.c_str()));
    }
    {
        GameMapBook book(dir);
        book.distrust("t8_s1");
        MapTrust t(&book);
        Feed f;
        for (int i = 0; i < 12; ++i) f.stand(t, 40 + i * 70, 199);
        check(t.report().named && t.report().why == Why::Distrusted, "named but distrusted: named, why Distrusted");
        Feed e;
        e.look.map_known = true;
        e.look.map = 8;
        e.look.area = 2;
        e.look.epoch = 1;
        e.look.entry_x = 300;
        e.look.entry_y = 290;
        e.stand(t, 500, 299);
        check(t.trusted() && !t.report().named && t.report().stem == "t8_s2", "a map entry's: never named");
    }
}

// --------------------------------------------------------------- sketch

void test_sketch() {
    section("Map tab: the floors as runs (build_sketch) -- linked column to column, simplified");

    const auto sketch_of = [](const Floors& fl) { return build_sketch("made_up", fl.make()); };
    {
        Floors fl(3, 4);
        fl.flat(0, 95, 40, 1);
        const MapSketch s = sketch_of(fl);
        check(s.runs.size() == 1 && s.runs[0].pts.size() == 2 && s.runs[0].pts[0].x == 0 &&
                  s.runs[0].pts[1].x == 95 && s.runs[0].pts[1].y == 40 && s.runs[0].x0 == 0 && s.runs[0].x1 == 95 &&
                  s.runs[0].y0 == 40 && s.runs[0].y1 == 40 && s.runs[0].attr == 1 && s.w_px == 96 && s.h_px == 128 &&
                  s.columns == 96 && s.points() == 2,
              "a flat floor: one run of its two ends, its box, 96 columns");
    }
    {
        Floors fl(4, 8);
        fl.run(0, 127, [](int x) { return 300 + x / 4; }, 1);
        fl.run(0, 100, [](int x) { return 100 + x; }, 1);
        const MapSketch s = sketch_of(fl);
        bool ends = s.runs.size() == 2;
        for (const MapRun& r : s.runs)
            ends = ends && r.pts.size() == 2 && r.pts.front().x == 0 && r.pts.back().x == r.x1 &&
                   (r.x1 == 127 ? r.pts.back().y == 331 && r.y0 == 300 && r.y1 == 331 : r.pts.back().y == 200);
        check(ends, "a slope a pixel every 4 columns, and one a pixel every column: their ends kept, nothing between");
    }
    {
        Floors fl(3, 8);
        fl.flat(0, 95, 100, 1);
        fl.flat(0, 95, 200, 1);
        fl.flat(0, 47, 150, 1);
        fl.flat(48, 95, 150, 2);
        const MapSketch s = sketch_of(fl);
        int attr2 = 0;
        for (const MapRun& r : s.runs) attr2 += r.attr == 2 ? 1 : 0;
        check(s.runs.size() == 4 && attr2 == 1,
              fmt("two floors a column: two runs; a platform beside the bottom ground at one height: two (%zu runs)",
                  s.runs.size()));
    }
    {
        Floors five(3, 8), four(3, 8);
        five.flat(0, 47, 100, 1);
        five.flat(48, 95, 105, 1);
        four.flat(0, 47, 100, 1);
        four.flat(48, 95, 104, 1);
        const MapSketch a = sketch_of(five), b = sketch_of(four);
        check(a.runs.size() == 2 && b.runs.size() == 1 && b.runs[0].y0 == 100 && b.runs[0].y1 == 104,
              "a step of 5 px: two runs; of 4 px: one");
    }
    {
        // Two floors 3 px apart coming in, one going on between them: it
        // goes on with the nearer, and the other ends.
        Floors fl(3, 8);
        fl.flat(0, 47, 100, 1);
        fl.flat(0, 47, 103, 1);
        fl.flat(48, 95, 101, 1);
        fl.flat(50, 50, 300, 1);
        const MapSketch s = sketch_of(fl);
        const MapRun* to_end = nullptr;
        const MapRun* ended = nullptr;
        const MapRun* one = nullptr;
        for (const MapRun& r : s.runs) {
            if (r.x0 == 0 && r.x1 == 95) to_end = &r;
            if (r.x0 == 0 && r.x1 == 47) ended = &r;
            if (r.x0 == 50 && r.x1 == 50) one = &r;
        }
        check(s.runs.size() == 3 && to_end && to_end->y0 == 100 && to_end->y1 == 101 && ended && ended->y0 == 103,
              "one to one: two floors converging within 4 px do not both join the one going on -- the nearer does");
        check(one && one->pts.size() == 1 && one->pts[0].y == 300, "a floor one column wide: a run of one point");
    }
    {
        Floors fl(2, 2);
        const MapSketch s = sketch_of(fl);
        check(s.runs.empty() && s.columns == 0 && s.w_px == 64 && s.points() == 0, "a map with no floor: no runs");
        MapFloors none;
        check(build_sketch("none", none).runs.empty(), "no map at all: no runs");
    }
    {
        // The largest map's size: 8,960 x 3,840.
        Floors fl(280, 120);
        for (int i = 0; i < 10; ++i) fl.flat(0, 8959, 300 + i * 300, 1);
        fl.flat(0, 8959, 3800, 2);
        for (int i = 0; i < 140; ++i) fl.flat(i * 64, i * 64 + 31, 150, 1);
        const MapSketch s = sketch_of(fl);
        check(s.runs.size() == 151 && s.points() == 302 && s.columns == 8960u * 11 + 140 * 32 && s.w_px == 8960 &&
                  s.h_px == 3840,
              fmt("8960 x 3840 with 11 floors across and 140 short ones: 151 runs, 302 points (%zu, %zu, %zu columns)",
                  s.runs.size(), s.points(), s.columns));
    }

    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): the sketch of t52_s1/s2\n");
        return;
    }
    GameMapBook book(data);
    const struct {
        const char* stem;
        std::size_t runs, points;
    } kMeasured[] = { { "t52_s1", 29, 322 }, { "t52_s2", 31, 387 } };
    for (const auto& m : kMeasured) {
        const std::shared_ptr<const GameMapBook::Page> p = book.get(m.stem);
        const MapSketch s = p ? build_sketch(m.stem, p->floors) : MapSketch{};
        check(p && s.runs.size() == m.runs && s.points() == m.points,
              fmt("%s from the game's files: %zu runs, %zu points, as floor_segments.py measured (%zu, %zu)", m.stem,
                  m.runs, m.points, s.runs.size(), s.points()));
    }
}

// ------------------------------------------------------------- no-drop marks

/// Whether a run of `marks` holds the floor at x, y (flat floors).
bool marked(const std::vector<MapRun>& marks, int x, int y) {
    for (const MapRun& r : marks)
        if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) return true;
    return false;
}

void test_no_drop_marks() {
    section("Map tab: the gate's no-drop marks, as Hunter::drop_rule refuses for a gate");

    // Platforms at 200 and 400, the bottom at 600; 1024 x 640.
    Floors fl(32, 20);
    fl.flat(0, 1023, 200, 1);
    fl.flat(0, 1023, 400, 1);
    fl.flat(0, 1023, 600, 2);
    const MapGate low = gate_at(500, 392);
    const GameMapBook::Page page = fl.page("t9_s1", { low });
    const std::vector<MapGate> one = { low };
    const std::vector<MapRun> marks = no_drop_marks(page, one, 150);
    check(marks.size() == 2 && marked(marks, 350, 200) && marked(marks, 650, 200) && marked(marks, 350, 400) &&
              marked(marks, 650, 400),
          fmt("a gate at 500,392: both platforms marked 150 px either side -- it lies in both falls (%zu runs)",
              marks.size()));
    check(!marked(marks, 349, 200) && !marked(marks, 651, 400) && !marked(marks, 500, 600),
          "not 151 px across, and never the bottom ground: nothing is dropped through it");
    const std::vector<MapGate> bottom = { gate_at(500, 590) };
    const std::vector<MapRun> deep = no_drop_marks(page, bottom, 150);
    check(deep.size() == 1 && marked(deep, 500, 400) && !marked(deep, 500, 200),
          "a gate on the bottom at 500,590: from 400 the fall reaches it; from 200 it lands on 400, more than 60 px "
          "above it -- not marked");
    check(no_drop_marks(page, {}, 150).empty(), "no gate: no marks");

    // drop_rule itself on the same floors, a target straight below each
    // standing point: Gate exactly where marked.
    HunterConfig cfg;
    cfg.drop_down = true;
    cfg.packet_positions = PacketPositions::On;
    cfg.spot = false;
    cfg.reach = 450;
    cfg.jump_key = 38;
    cfg.drop_gate_px = 150;
    const auto page_ptr = std::make_shared<const GameMapBook::Page>(page);
    int agree = 0, cases = 0, gates_seen = 0;
    std::string off;
    for (const std::vector<MapGate>* gates : { &one, &bottom }) {
        const std::vector<MapRun> m = no_drop_marks(page, *gates, cfg.drop_gate_px);
        for (const int x : { 340, 349, 350, 420, 500, 650, 651, 700 }) {
            for (const int floor_y : { 200, 400 }) {
                const int land = floor_y + 200;
                Hunter::DropFacts f;
                f.press = true;
                f.now = 100'000;
                f.wired = true;
                f.packet_target = true;
                f.dx = 0;
                f.dy = land - floor_y;
                f.target_wx = static_cast<float>(x);
                f.target_wy = static_cast<float>(land - 1);
                f.fix.state = WorldFix::State::Known;
                f.fix.x = f.fix.rx = static_cast<float>(x);
                f.fix.y = f.fix.ry = static_cast<float>(floor_y - 1);
                f.fix.report_ms = 99'960;
                f.page = page_ptr;
                f.gates = *gates;
                const Hunter::DropCall c = Hunter::drop_rule(cfg, f);
                const bool gate = c.why == Hunter::DropWhy::Gate;
                gates_seen += gate ? 1 : 0;
                ++cases;
                if (gate == marked(m, x, floor_y) && (gate || c.ok())) {
                    ++agree;
                } else {
                    off += fmt(" %d,%d:%s", x, floor_y, Hunter::drop_why_name(c.why));
                }
            }
        }
    }
    check(agree == cases && gates_seen > 0,
          fmt("drop_rule says Gate where the marks are, and goes elsewhere: %d of %d (%d Gate)%s", agree, cases,
              gates_seen, off.c_str()));
}

// ------------------------------------------------------------------ camera

bool near_eq(double a, double b, double eps) { return std::abs(a - b) <= eps; }

void test_camera() {
    section("Map tab: the canvas's camera -- world to screen and back, zoom about the cursor, its limits");

    const P origin{ 10, 20 }, size{ 700, 460 };
    MapCamera cam;
    cam.cx = 1000;
    cam.cy = 500;
    cam.zoom = 0.37;
    const P w{ 1234.5, 678.25 };
    const P back = cam.to_world(cam.to_screen(w, origin, size), origin, size);
    const P mid = cam.to_screen({ 1000, 500 }, origin, size);
    check(near_eq(back.x, w.x, 1e-9) && near_eq(back.y, w.y, 1e-9) && near_eq(mid.x, 360, 1e-9) &&
              near_eq(mid.y, 250, 1e-9),
          "to_world(to_screen(p)) is p, and the centre is the canvas's middle");

    const P cursor{ 123, 77 };
    const P under = cam.to_world(cursor, origin, size);
    cam.zoom_about(cursor, MapCamera::kZoomStep, origin, size, 0.1, 2.0);
    const P after = cam.to_world(cursor, origin, size);
    bool fixed = near_eq(after.x, under.x, 0.01) && near_eq(after.y, under.y, 0.01) && near_eq(cam.zoom, 0.444, 1e-9);
    for (int i = 0; i < 30; ++i) cam.zoom_about(cursor, MapCamera::kZoomStep, origin, size, 0.1, 2.0);
    const P far_in = cam.to_world(cursor, origin, size);
    fixed = fixed && near_eq(cam.zoom, 2.0, 1e-12) && near_eq(far_in.x, under.x, 0.01) && near_eq(far_in.y, under.y, 0.01);
    check(fixed, "the wheel zooms about the cursor: its world point stays put to 0.01 px, up to the most zoom");

    const struct {
        int    w, h;
        double zmin, follow;
    } kMaps[] = { { 800, 608, 460.0 / 608 * 0.9, 460.0 / 608 * 0.9 },
                  { 3200, 1280, 700.0 / 3200 * 0.9, 0.5 },
                  { 1280, 2560, 460.0 / 2560 * 0.9, 0.5 } };
    bool limits = true;
    std::string got;
    for (const auto& m : kMaps) {
        const double zmin = MapCamera::zmin_for(m.w, m.h, 700, 460);
        const double zmax = MapCamera::zmax_for(zmin);
        const double follow = MapCamera::follow_zoom(zmin, zmax);
        limits = limits && near_eq(zmin, m.zmin, 1e-9) && zmax == 2.0 && near_eq(follow, m.follow, 1e-9);
        got += fmt(" %dx%d: %.3f..%.1f follow %.3f;", m.w, m.h, zmin, zmax, follow);
    }
    check(limits, "into 700 x 460: zmin 0.9 of the fit, zmax 2, Follow at 0.5 within them --" + got);
    const double tiny = MapCamera::zmin_for(100, 100, 700, 460);
    check(near_eq(tiny, 4.14, 1e-9) && MapCamera::zmax_for(tiny) == tiny &&
              MapCamera::follow_zoom(tiny, MapCamera::zmax_for(tiny)) == tiny,
          "a map smaller than the canvas: zmax is zmin, and Follow is there too");

    MapCamera c;
    c.cx = -50;
    c.cy = 5000;
    c.clamp_centre(3200, 1280);
    MapCamera d;
    d.cx = 1600.5;
    d.cy = 7;
    d.clamp_centre(3200, 1280);
    c.fit(3200, 1280, size);
    check(d.cx == 1600.5 && d.cy == 7 && c.cx == 1600 && c.cy == 640 && near_eq(c.zoom, 700.0 / 3200, 1e-12),
          "the centre kept on the map; Cả bản đồ: the middle, at the fit");
    MapCamera e;
    e.cx = -50;
    e.cy = 5000;
    e.clamp_centre(3200, 1280);
    check(e.cx == 0 && e.cy == 1280, "a centre off the map: back on its edge");
}

// --------------------------------------------------------------- grips

HuntZone owner_zone() {
    HuntZone z;
    z.home_x = 1821;
    z.home_y = 359;
    z.left = 1521;
    z.right = 2121;
    z.top = 200;
    z.bottom = 520;
    return z;
}

void test_grips() {
    section("Map tab: the zone's grips -- pin, corners, edges, the strip; never its inside");

    const HuntZone z = owner_zone();
    const P origin{ 0, 0 }, size{ 700, 460 };
    MapCamera cam;
    cam.cx = 1821;
    cam.cy = 360;
    cam.zoom = 1.0;
    // On screen: box 50..650 x 70..390, the home at 350,229, the pin's head 10 above.
    const auto at = [&](double x, double y, bool pin = true) { return zone_grip(z, cam, origin, size, { x, y }, pin); };
    check(at(350, 219) == ZoneGrip::Home && at(352, 225) == ZoneGrip::Home && at(350, 232) != ZoneGrip::Home,
          "at zoom 1: the pin's head and the pin");
    check(at(50, 70) == ZoneGrip::TL && at(650, 70) == ZoneGrip::TR && at(50, 390) == ZoneGrip::BL &&
              at(650, 390) == ZoneGrip::BR && at(57, 77) == ZoneGrip::TL && at(643, 383) == ZoneGrip::BR,
          "the corners, 8 px of both edges");
    check(at(50, 200) == ZoneGrip::L && at(46, 200) == ZoneGrip::L && at(654, 200) == ZoneGrip::R &&
              at(300, 66) == ZoneGrip::T && at(300, 394) == ZoneGrip::B,
          "the edges, 5 px either side");
    check(at(300, 80) == ZoneGrip::Strip && at(300, 84) == ZoneGrip::Strip && at(300, 85) == ZoneGrip::None,
          "the strip: 14 px inside the top edge");
    check(at(300, 300) == ZoneGrip::None && at(20, 200) == ZoneGrip::None && at(300, 30) == ZoneGrip::None &&
              at(600, 200) == ZoneGrip::None,
          "the inside, and outside it: no grip -- a drag there pans");
    check(at(350, 219, false) == ZoneGrip::None && at(300, 80, false) == ZoneGrip::None &&
              at(50, 70, false) == ZoneGrip::TL && at(50, 200, false) == ZoneGrip::L,
          "without floors for the home: no pin and no strip, edges and corners only");

    cam.zoom = 0.2;
    // Box 290..410 x 198..262, the home at 350,229.8, its head at 350,219.8.
    check(at(350, 220) == ZoneGrip::Home && at(290, 198) == ZoneGrip::TL && at(410, 262) == ZoneGrip::BR &&
              at(290, 230) == ZoneGrip::L && at(350, 262) == ZoneGrip::B && at(310, 208) == ZoneGrip::Strip &&
              at(390, 240) == ZoneGrip::None,
          "at zoom 0.2: the pin, corners, edges and strip; the inside none");
}

// ------------------------------------------------------------- the drag

void test_gesture() {
    section("Map tab: a drag on a grip -- through the camera frozen at the press");

    const HuntZone z = owner_zone();
    ZoneLimits lim;
    lim.w_px = 3200;
    lim.h_px = 1280;
    ZoneGesture g;
    g.from = z;
    g.cam.cx = 1821;
    g.cam.cy = 360;
    g.cam.zoom = 0.5;
    g.origin = { 0, 0 };
    g.size = { 700, 460 };
    g.press = { 300, 100 };
    const auto drag = [&](ZoneGrip grip, double dx, double dy) {
        ZoneGesture h = g;
        h.grip = grip;
        return gesture_draft(h, { g.press.x + dx, g.press.y + dy }, lim);
    };

    HuntZone m = drag(ZoneGrip::Strip, 100, 50);
    check(m.left == 1721 && m.right == 2321 && m.top == 300 && m.bottom == 620 && m.home_x == 2021 &&
              m.home_y == 459 && m.width() == z.width() && m.height() == z.height(),
          "the strip, 100,50 on screen at zoom 0.5: box and home 200,100 on, the size kept");
    m = drag(ZoneGrip::Strip, 5000, -5000);
    check(m.right == 3199 && m.left == 2599 && m.top == 0 && m.bottom == 320 && m.home_x == 2899 && m.home_y == 159,
          "moved far: kept on the map, the home with it");
    m = drag(ZoneGrip::L, -100, 30);
    check(m.left == 1321 && m.right == 2121 && m.top == 200 && m.bottom == 520 && m.home_x == 1821,
          "the left edge 200 px out: only it moves");
    m = drag(ZoneGrip::R, 50, 0);
    check(m.right == 2221 && m.left == 1521, "the right edge: only it");
    m = drag(ZoneGrip::L, 1000, 0);
    check(m.left == 1821, "the left edge dragged past the home: it stops at the home");
    m = drag(ZoneGrip::T, 0, 1000);
    check(m.top == 359 && m.bottom == 520, "the top edge dragged down past the home: it stops there too");
    m = drag(ZoneGrip::B, 0, 5000);
    check(m.bottom == 1279 && m.top == 200, "the bottom edge down off the map: kept on it");
    m = drag(ZoneGrip::L, -5000, 0);
    check(m.left == 0, "the left edge to the map's own (Q3: a zone as large as the map)");
    m = drag(ZoneGrip::BR, -100, 60);
    check(m.right == 1921 && m.bottom == 640 && m.left == 1521 && m.top == 200, "a corner: its two edges");
    m = drag(ZoneGrip::TL, 2000, 2000);
    check(m.left == 1821 && m.top == 359, "the corner across the home: both stop there");
    {
        ZoneGesture h = g;
        h.from.home_x = 1530;
        h.grip = ZoneGrip::R;
        const HuntZone small = gesture_draft(h, { g.press.x - 1000, g.press.y }, lim);
        h.from.home_y = 500;
        h.grip = ZoneGrip::T;
        const HuntZone low = gesture_draft(h, { g.press.x, g.press.y + 1000 }, lim);
        check(small.right == 1621 && small.width() == kZoneMinW && low.top == 480 && low.height() == kZoneMinH,
              "never under 100 x 40 px");
    }
    m = drag(ZoneGrip::Home, -2000, 40);
    check(m.home_x == 1521 && m.home_y == 439 && m.left == 1521 && m.right == 2121,
          "the pin: the home moves, within the box");
    check(same_place(drag(ZoneGrip::None, 50, 50), z), "no grip: nothing moves");

    // Follow, or the wheel, moved the live camera between the press and the
    // release: the drag goes by the camera of the press, as the mouse saw it.
    MapCamera live = g.cam;
    live.cx += 400;
    live.zoom = 1.3;
    ZoneGesture h = g;
    h.grip = ZoneGrip::Strip;
    const P release{ 400, 150 };
    const HuntZone frozen = gesture_draft(h, release, lim);
    h.cam = live;
    const HuntZone by_live = gesture_draft(h, release, lim);
    h.cam = g.cam;
    check(same_place(gesture_draft(h, release, lim), frozen) && frozen.left == 1721 && !same_place(by_live, frozen),
          "the camera moved between press and release: the same draft by the frozen camera -- another by the live");
}

// ------------------------------------------------------------- snapping

void test_snap() {
    section("Map tab: the home snapped to a floor");

    // 256 x 640: platforms at 200 and 400 over x 0..63, a wall at 300 over
    // 64..79, nothing over 80..159, the bottom at 600 over 160..255.
    Floors fl(8, 20);
    fl.flat(0, 63, 200, 1);
    fl.flat(0, 63, 400, 1);
    fl.flat(64, 79, 300, 4);
    fl.flat(160, 255, 600, 2);
    const GameMapBook::Page p = fl.page("t9_s2");
    const auto is = [](const std::optional<MapPoint>& s, int x, int y) { return s && s->x == x && s->y == y; };
    check(is(snap_home(p, 10, 195), 10, 199), "a click 5 px above a floor: that floor, 1 px above its top");
    check(is(snap_home(p, 10, 300), 10, 399), "between two floors: the one below");
    check(is(snap_home(p, 10, 500), 10, 399), "nothing below: the nearest above");
    check(is(snap_home(p, 150, 590), 160, 599), "a column with no floor: the nearest within 16 columns");
    check(is(snap_home(p, 70, 295), 63, 399), "a wall is never stood on: the platforms beside it");
    HuntZone box;
    box.left = 0;
    box.right = 63;
    box.top = 150;
    box.bottom = 250;
    check(is(snap_home(p, 10, 300, &box), 10, 199), "within a box: the floor below it passed over, the one inside");
    box.top = 450;
    box.bottom = 550;
    check(!snap_home(p, 10, 300, &box) && !snap_home(p, 120, 300), "no floor inside the box, or none near: none");
    check(is(snap_home(p, -40, 195), 0, 199) && is(snap_home(p, 900, 590), 255, 599), "a point off the map: its edge");
}

// --------------------------------------------------------------- checks

/// 2048 x 1024: the home's floor at 500 over 400..1200, running down a pixel
/// a column to 700 at 1400; a platform at 700 over 500..900, one at 420 over
/// 1000..1100, and the bottom at 1000.
Floors check_floors() {
    Floors fl(64, 32);
    fl.flat(400, 1200, 500, 1);
    fl.run(1201, 1400, [](int x) { return 500 + (x - 1200); }, 1);
    fl.flat(500, 900, 700, 1);
    fl.flat(1000, 1100, 420, 1);
    fl.flat(0, 2047, 1000, 2);
    return fl;
}

void test_zone_check() {
    section("Map tab: a zone's refusals and warnings (zone_check)");

    const GameMapBook::Page page = check_floors().page("t9_s3");
    const ZoneLimits lim = zone_limits(&page, false);
    check(lim.w_px == 2048 && lim.h_px == 1024 && lim.floors && !zone_limits(&page, true).floors &&
              !zone_limits(nullptr, false).floors,
          "the limits: the map's size, its floors gone by unless the file is doubtful, none without a page");
    HuntZone z;
    z.home_x = 600;
    z.home_y = 499;
    z.left = 300;
    z.right = 1450;
    z.top = 350;
    z.bottom = 800;
    const std::vector<MapPoint> own = home_run(page, z);
    check(own.size() == 1001 && own.front().x == 400 && own.back().x == 1400 && own.back().y == 700,
          fmt("the home's own floor: 400..1200 and down the slope to 1400 (%zu columns)", own.size()));

    ZoneCheck c = zone_check(&page, z, lim, {}, false);
    check(c.refuse_vi == nullptr && c.below_unreachable && c.home_slope_low && c.above_by_edge && !c.gate_near &&
              !c.too_short_for_drops,
          "a floor 200 below, the home's own slope 200 down, a floor 80 above: all three warned of");
    bool red_floor = false, red_slope = false, amber_floor = false, slope_amber = false;
    for (const std::vector<MapPoint>& r : c.red) {
        red_floor = red_floor || (r.front().x == 500 && r.back().x == 900 && r.front().y == 700);
        red_slope = red_slope || (r.front().x == 1321 && r.back().x == 1400);
    }
    for (const std::vector<MapPoint>& a : c.amber) {
        amber_floor = amber_floor || (a.front().x == 1000 && a.back().x == 1100 && a.front().y == 420);
        slope_amber = slope_amber || a.front().x > 1200;
    }
    check(c.red.size() == 2 && red_floor && red_slope && c.amber.size() == 1 && amber_floor && !slope_amber,
          fmt("red: the low floor and the slope past 120 px down; amber: the floor above (%zu red, %zu amber)",
              c.red.size(), c.amber.size()));

    // The home at the slope's foot: its own floor climbing 200 px is never
    // amber.
    HuntZone foot;
    foot.home_x = 1400;
    foot.home_y = 699;
    foot.left = 1150;
    foot.right = 1450;
    foot.top = 450;
    foot.bottom = 760;
    c = zone_check(&page, foot, lim, {}, false);
    check(c.refuse_vi == nullptr && !c.above_by_edge && c.amber.empty() && !c.below_unreachable && !c.home_slope_low,
          "the home at the foot of its own slope: the climb is its own floor -- no amber");

    // Drops: a gate in the box, a no-drop mark in it, a box too short.
    GameMapBook::Page gated = page;
    gated.gates = { gate_at(800, 492) };
    check(zone_check(&gated, z, lim, {}, true).gate_near && !zone_check(&gated, z, lim, {}, false).gate_near,
          "a gate in the box, drop_down on: warned of; off, not");
    MapRun mark;
    mark.attr = 1;
    mark.pts = { { 200, 500 }, { 700, 500 } };
    mark.x0 = 200;
    mark.x1 = 700;
    mark.y0 = mark.y1 = 500;
    MapRun away = mark;
    away.pts = { { 1500, 500 }, { 1700, 500 } };
    away.x0 = 1500;
    away.x1 = 1700;
    const std::vector<MapRun> in_box = { mark }, out_box = { away };
    check(zone_check(&page, z, lim, in_box, true).gate_near && !zone_check(&page, z, lim, out_box, true).gate_near,
          "a no-drop mark running into the box: warned of; one beside it, not");
    c = zone_check(&page, z, lim, {}, true);
    check(!c.too_short_for_drops && c.shortest_drop_px == 80, "450 px tall: the shortest drop, 420 to 500, fits");
    HuntZone flat = z;
    flat.top = 460;
    flat.bottom = 540;
    c = zone_check(&page, flat, lim, {}, true);
    check(c.too_short_for_drops && c.shortest_drop_px == 200 && !zone_check(&page, flat, lim, {}, false).too_short_for_drops,
          fmt("80 px tall round the home: no drop from it (200 px the shortest) fits -- said with drop_down on only "
              "(%d)",
              c.shortest_drop_px));

    // The refusals.
    HuntZone narrow = z;
    narrow.right = narrow.left + 99;
    HuntZone thin = z;
    thin.bottom = thin.top + 39;
    HuntZone air = z;
    air.home_y = 450;
    HuntZone empty;
    empty.left = 1500;
    empty.right = 1700;
    empty.top = 100;
    empty.bottom = 300;
    empty.home_x = 1600;
    empty.home_y = 200;
    HuntZone off = z;
    off.right = 2048;
    const std::string small_text = zone_check(&page, narrow, lim, {}, false).refuse_vi
                                       ? zone_check(&page, narrow, lim, {}, false).refuse_vi
                                       : "";
    check(small_text == "Vùng phải rộng ít nhất 100 px và cao ít nhất 40 px." &&
              zone_refusal(thin, lim, &page) == zone_refusal(narrow, lim, &page),
          "99 px wide, or 39 px tall: refused");
    const char* home_text = zone_refusal(air, lim, &page);
    const char* floor_text = zone_refusal(empty, lim, &page);
    check(home_text && std::string(home_text) == "Chỗ về phải nằm trên một bục, trong vùng." && floor_text &&
              std::string(floor_text) == "Vùng phải chứa ít nhất một bục để làm chỗ về.",
          "the home in the air: refused; a box with no floor in it: refused");
    check(zone_refusal(off, lim, &page) != nullptr && zone_valid(z, lim, &page),
          "a box past the map's edge: refused; the zone itself: valid");
    ZoneLimits doubtful = zone_limits(&page, true);
    check(zone_valid(air, doubtful, &page) && zone_check(&page, air, doubtful, {}, true).red.empty() &&
              !zone_valid(narrow, doubtful, &page),
          "a doubtful file: no floor checked or warned of -- only the size and the map");
    HuntZone outside = z;
    outside.home_x = 200;
    check(zone_refusal(outside, doubtful, &page) == zone_refusal(air, lim, &page),
          "the home outside the box: refused, floors or not");
}

// --------------------------------------- walking in the zone (2026.11.04d)

/// 2048 x 1024: the ground at 600 over 0..399, down a pixel a column to 900
/// at 700, then 900 on to the map's edge; a ledge at 300 over 300..700; short
/// floors at 380 and 500 over 1000..1100; the bottom at 1000.
Floors walk_floors() {
    Floors fl(64, 32);
    fl.flat(0, 399, 600, 1);
    fl.run(400, 700, [](int x) { return 600 + (x - 400); }, 1);
    fl.flat(701, 2047, 900, 1);
    fl.flat(300, 700, 300, 1);
    fl.flat(1000, 1100, 380, 1);
    fl.flat(1000, 1100, 500, 1);
    fl.flat(0, 2047, 1000, 2);
    return fl;
}

HuntZone box_of(int home_x, int home_y, int left, int top, int right, int bottom) {
    HuntZone z;
    z.home_x = home_x;
    z.home_y = home_y;
    z.left = left;
    z.top = top;
    z.right = right;
    z.bottom = bottom;
    return z;
}

bool point_is(const std::optional<MapPoint>& p, int x, int y) { return p && p->x == x && p->y == y; }

void test_zone_walk() {
    section("Map tab, 04d: a walk kept in the zone, the way back into it, home by height, much red in red");

    const GameMapBook::Page page = walk_floors().page("t9_s4", { gate_at(500, 292), gate_at(600, 230) });
    // The ledge's home, the box deep below it -- as the owner's of 2026-11-04;
    // and the home low on the ground, the box up from it.
    const HuntZone deep = box_of(500, 299, 0, 250, 900, 700);
    const HuntZone low = box_of(800, 899, 0, 250, 900, 950);

    // Where a walk off an end lands.
    check(point_is(walk_off_landing(page, { 700, 300 }, 1, 0.0), 701, 899) &&
              point_is(walk_off_landing(page, { 300, 300 }, -1, 0.0), 299, 599),
          "off the ledge's ends with the arrow let go of: straight down the column past each");
    const std::optional<MapPoint> flung = walk_off_landing(page, { 300, 300 }, -1, kZoneWalkPxPerS);
    check(flung && flung->y == 599 && flung->x >= 115 && flung->x <= 125,
          fmt("at a walk's 360 px/s: 300 px down in half a second, some 180 px on (%d,%d)", flung ? flung->x : -1,
              flung ? flung->y : -1));
    check(!walk_off_landing(page, { 2047, 900 }, 1, 0.0) && !walk_off_landing(page, { 0, 600 }, -1, 0.0),
          "straight down past the map's side: nothing of the map under the feet -- none");

    // Q1: a walk never leaves the box.
    ZoneWalk w = walk_stays_in(page, deep, { 450, 649 }, 1);
    check(!w.ok && w.why == ZoneWalk::Why::Leaves && w.at.x == 502 && w.at.y == 701 && w.out_px == 1,
          fmt("down the ground's slope toward the box's bottom at 700: refused where it leaves it, 502,701 (%d,%d)",
              w.at.x, w.at.y));
    check(walk_stays_in(page, deep, { 450, 649 }, -1).why == ZoneWalk::Why::Inside &&
              walk_stays_in(page, deep, { 380, 599 }, 1).why == ZoneWalk::Why::Inside,
          "the other way, or 122 px back from where it leaves -- past kZoneWalkAheadPx: walked");
    w = walk_stays_in(page, deep, { 650, 299 }, 1);
    check(!w.ok && w.why == ZoneWalk::Why::WalkOff && w.at.x == 700 && point_is(w.land, 701, 899) && w.out_px == 199,
          "off the ledge's right end the fall lands on the ground at 899, 199 px below the box: refused");
    w = walk_stays_in(page, deep, { 350, 299 }, -1);
    check(w.ok && w.why == ZoneWalk::Why::Inside,
          "off its left end every fall lands on the ground in the box -- a fall inside is allowed (kZoneFallInsideOk)");
    w = walk_stays_in(page, deep, { 650, 849 }, -1);
    const ZoneWalk out = walk_stays_in(page, deep, { 650, 849 }, 1);
    check(w.ok && w.why == ZoneWalk::Why::Closer && !out.ok && out.why == ZoneWalk::Why::Leaves && out.out_px == 150,
          "149 px below the box: up the slope toward it always allowed, down it further out refused");
    check(walk_stays_in(page, deep, { 1050, 450 }, 1).ok &&
              walk_stays_in(page, deep, { 1050, 450 }, 1).why == ZoneWalk::Why::Unjudged &&
              std::string(zone_walk_name(ZoneWalk::Why::Leaves)) == "leaves the area along its floor",
          "a point on no floor: not judged, let be; the log's words");

    // The way back.
    std::optional<ZoneReturn> r = return_point(page, deep, { 650, 849 });
    check(r && !r->home && r->at.x == 500 && r->at.y == 699 && r->floor_y == 700 && r->falls == 0 &&
              r->walk_x == 500 && r->own_floor() && !r->as_before,
          fmt("from the ground below the box, the home 400 px up on the ledge: the ground's point in the box nearest "
              "it across, 500,699 -- walked to, never hopped (%d,%d)",
              r ? r->at.x : -1, r ? r->at.y : -1));
    r = return_point(page, deep, { 350, 299 });
    check(r && r->home && r->at.x == 500 && r->at.y == 299 && r->falls == 0 && r->walk_x == 500,
          "on the home's own ledge: the home");
    r = return_point(page, low, { 500, 299 });
    check(r && r->home && r->at.x == 800 && r->at.y == 899 && r->falls == 1 && r->walk_x == 300 - kZoneWalkOffPastPx &&
              !r->own_floor(),
          fmt("the home low: off the ledge's left end -- whose falls all land in the box, not its right's -- and "
              "along the ground to it (walk to %d)",
              r ? r->walk_x : -1));
    r = return_point(page, deep, { 1050, 379 });
    check(r && r->home && r->as_before && r->walk_x == 500 && !zone_walks_back_first(400, r),
          "nothing in the box reached from the floor at 380, the home 80 px up: walked toward as before -- not a way "
          "back the map shows");
    check(!return_point(page, deep, { 1050, 499 }),
          "from the one at 500, the home 200 px up -- higher than one jump: no way back");

    // 04d Q2: outside, with a way back the map shows, the walk back first.
    const std::optional<ZoneReturn> way = return_point(page, deep, { 650, 849 });
    check(zone_walks_back_first(149, way) && !zone_walks_back_first(kZoneOutsideBackPx, way) &&
              !zone_walks_back_first(149, std::nullopt) && kZoneOutsideBackPx == Hunter::kSpotKeepPx,
          "Q2 (a): 149 px out with a way back -- back first; 60 px out, or no way back -- it fights where it stands");

    // Home by height as well as across.
    check(zone_at_home(&page, deep, { 520, 299 }, 48) && !zone_at_home(&page, deep, { 510, 709 }, 48) &&
              !zone_at_home(&page, deep, { 560, 299 }, 48),
          "home: on its ledge within 48 px; under it on the ground, 10 px across, is not");
    check(zone_stands_at(&page, { 500, 699 }, { 505, 704 }, 48) && zone_at_home(nullptr, deep, { 520, 320 }, 48) &&
              !zone_at_home(nullptr, deep, { 520, 340 }, 48) && zone_at_home(&page, deep, { 520, 330 }, 48),
          "at a way back's point on its floor; with no page, or off every floor, within 31 px up or down");

    // Much red is said in red; the home by a gate is warned of.
    const ZoneLimits lim = zone_limits(&page, false);
    ZoneCheck c = zone_check(&page, deep, lim, {}, false);
    check(c.refuse_vi == nullptr && c.floor_cols == 903 && c.red_cols == 502 && c.red_bad && c.below_unreachable,
          fmt("the deep box: %d of its %d floor columns red -- more than half: said in red", c.red_cols,
              c.floor_cols));
    check(c.home_by_gate && c.home_gate_px == 7,
          fmt("a gate 7 px over the home: warned of, drops off as on (%d px)", c.home_gate_px));
    c = zone_check(&page, low, lim, {}, true);
    check(c.refuse_vi == nullptr && c.red_cols == 0 && !c.red_bad && !c.home_by_gate && c.home_gate_px == -1,
          "the home low, the box up from it: nothing red, no gate near it");
    const std::vector<MapGate> learned = { gate_at(500, 400), gate_at(860, 899) };
    c = zone_check(&page, low, lim, {}, false, learned);
    check(c.home_by_gate && c.home_gate_px == 60, "the map's gates as the tab has them, a learned one 60 px away: warned");
    check(zone_red_is_bad(1068, 1814) && !zone_red_is_bad(907, 1814) && zone_red_is_bad(908, 1814) &&
              !zone_red_is_bad(0, 0),
          "more than half red is bad: 1068 of 1814, as 2026-11-04 20:39; exactly half is not");

    // Q5: the home where the character stands; (b)'s lowest floor, at hand.
    check(!kZoneAroundHomeLowest && point_is(zone_home_lowest(page, low, 500), 700, 899) &&
              point_is(zone_home_lowest(page, deep, 500), 501, 700) && kZoneWalkStaysIn && kZoneFallInsideOk &&
              kZoneWalkAheadPx >= 80 && kZoneWalkAheadPx <= 100,
          "Q5 (a) as it is; (b)'s lowest floor, the nearest such: 700,899 and 501,700. Q1 (a): walks stay in the "
          "box, falls into it allowed, 80-100 px looked ahead");
}

void test_zone_t52s2() {
    section("Map tab, 04d: t52_s2's own floors -- the hunt of 2026-11-04 20:39 replayed (copies in "
            "build\\handoff\\drop\\testdata)");

    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): t52_s2's zones\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    check(page != nullptr, "t52_s2 reads");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    const ZoneLimits lim = zone_limits(&p, false);
    // The owner's box, from 20:39:10: home 880,274 on the spawn ledge, 7 px
    // under the 피스존 gate, the box dragged down to the ground; and the one
    // the 04d notes advise: the home low on the ground, the box up from it.
    const HuntZone owner = box_of(880, 274, 265, 174, 1399, 694);
    const HuntZone home_low = box_of(1100, 714, 265, 174, 1399, 810);
    const HuntZone first = box_of(880, 274, 580, 174, 1180, 374);

    ZoneCheck c = zone_check(&p, owner, lim, {}, false);
    check(c.refuse_vi == nullptr && c.floor_cols == 1814 && c.red_cols == 1068 && c.red_bad && c.below_unreachable &&
              c.home_by_gate && c.home_gate_px == 7,
          fmt("the owner's box: %d of %d floor columns red -- said in red now -- and the home 7 px from the gate "
              "(%d)",
              c.red_cols, c.floor_cols, c.home_gate_px));
    c = zone_check(&p, home_low, lim, {}, false);
    check(c.refuse_vi == nullptr && c.red_cols == 0 && !c.red_bad && !c.home_by_gate && c.floor_cols == 2238,
          fmt("265..1399 x 174..810, home 1100,714: no red at all (%d of %d)", c.red_cols, c.floor_cols));
    c = zone_check(&p, first, lim, {}, true);
    check(c.refuse_vi == nullptr && c.red_cols == 0 && !c.red_bad && c.home_by_gate && c.gate_near,
          "20:32:28's box round the spawn: no red, the gate warned of");

    // The ledge's real end, the fall off it.
    check(point_is(walk_off_landing(p, { 1104, 274 }, 1, 0.0), 1105, 503) &&
              point_is(walk_off_landing(p, { 1104, 274 }, 1, kZoneWalkPxPerS), 1231, 421),
          "off the spawn ledge's end at 1104: onto the ramp, 1105,503 straight down to 1231,421 at a walk");

    // 20:43:31, 987,683 inside the box on the ground: right, it leaves the
    // box's bottom at x 1035 -- refused; left, it climbs inside.
    ZoneWalk w = walk_stays_in(p, owner, { 987, 683 }, 1);
    check(!w.ok && w.why == ZoneWalk::Why::Leaves && w.at.x == 1035 && w.at.y == 695 && w.out_px == 1,
          fmt("from 987,683 right: refused -- the ground leaves the box at 1035,695 (%d,%d)", w.at.x, w.at.y));
    check(walk_stays_in(p, owner, { 987, 683 }, -1).why == ZoneWalk::Why::Inside &&
              !walk_stays_in(p, owner, { 1030, 693 }, 1).ok && !walk_stays_in(p, owner, { 940, 663 }, 1).ok,
          "left from it: walked; right from 940 or from 1030, the edge within 96 px: refused -- never past x 1035");
    w = walk_stays_in(p, owner, { 1090, 273 }, 1);
    check(w.ok && w.why == ZoneWalk::Why::Inside,
          "off the ledge's end at 1104 every fall lands on the ramp inside the box: allowed, as Q1 (a) has it");
    check(walk_stays_in(p, owner, { 1402, 802 }, -1).why == ZoneWalk::Why::Closer &&
              walk_stays_in(p, owner, { 1402, 802 }, 1).why == ZoneWalk::Why::Leaves,
          "20:43:51, 1402,802, 108 px below the box: left up the ground, closer -- allowed; right, further: refused");
    check(walk_stays_in(p, owner, { 1360, 701 }, 1).why == ZoneWalk::Why::WalkOff &&
              walk_stays_in(p, owner, { 1360, 701 }, -1).why == ZoneWalk::Why::WalkOff,
          "on the small floor 7 px below the box: off either of its ends, 1337 or 1404, it lands further out");

    // The way back from where the hunt ended: left along the ground it stands
    // on, up 168 px of slope, to 880 -- under the home, which it cannot reach.
    std::optional<ZoneReturn> r = return_point(p, owner, { 1402, 802 });
    check(r && !r->home && r->at.x == 880 && r->floor_y == 635 && r->at.y == 634 && r->falls == 0 &&
              r->walk_x == 880 && r->own_floor() && !r->as_before,
          fmt("from 1402,802: the return point is 880 on floor 635 (standing 880,634), along its own floor to the "
              "left -- walked, no hop (%d,%d)",
              r ? r->at.x : -1, r ? r->floor_y : -1));
    check(zone_walks_back_first(zone_outside_px(owner, 1402, 802), r) && zone_outside_px(owner, 1402, 802) == 108,
          "108 px outside with that way back: Q2 (a) walks back first");
    r = return_point(p, owner, { 1200, 437 });
    check(r && !r->home && r->at.x == 992 && r->at.y == 587 && r->falls == 0,
          "from the ramp: its foot at 992, the ramp's nearest the home across -- never down off it while it is in "
          "the box");
    r = return_point(p, owner, { 880, 274 });
    check(r && r->home && r->falls == 0, "on the spawn ledge: the home");
    r = return_point(p, home_low, { 1402, 802 });
    check(r && r->home && r->at.x == 1100 && r->at.y == 714 && r->falls == 0 && r->walk_x == 1100,
          "the home low on the ground: from 1402,802 the home itself, 88 px up the same floor");
    r = return_point(p, home_low, { 1200, 437 });
    check(r && r->home && r->falls == 1 && r->walk_x == 992 - kZoneWalkOffPastPx,
          fmt("from the ramp: off its foot at 992 onto the ground, then to the home (walk to %d)", r ? r->walk_x : -1));
    r = return_point(p, home_low, { 880, 274 });
    check(r && r->home && r->falls == 1 && r->walk_x == 472 - kZoneWalkOffPastPx,
          "from the spawn ledge: off its left end at 472, over the hole at 711, onto the ground");

    // Home by height: 20:41:11's 882,616 -- "3 px off" by x alone -- is not
    // home; nor is the ground under it.
    check(!zone_at_home(&p, owner, { 882, 616 }, 48) && !zone_at_home(&p, owner, { 882, 635 }, 48) &&
              zone_at_home(&p, owner, { 885, 274 }, 48) && zone_stands_at(&p, { 880, 634 }, { 900, 644 }, 48),
          "882,616 and the ground at 882 are not home at 880,274; the ledge at 885 is; 900,644 has come to the "
          "return point 880,634");
}

// -------------------------------------------- the owner's answers, in one place

void test_answers() {
    section("Map tab: the owner's three answers, each in one place -- the take rule, Đặt chỗ này, the largest zone");

    const HuntZone z = owner_zone();
    check(zone_take(z, 1700, 400, false, false) == ZoneTake::Inside &&
              zone_take(z, 2221, 300, false, false) == ZoneTake::Near &&
              zone_take(z, 1821, 669, false, false) == ZoneTake::Near &&
              zone_take(z, 2272, 300, false, false) == ZoneTake::Wait &&
              zone_take(z, 3000, 300, false, true) == ZoneTake::Back &&
              zone_take(z, 3000, 300, true, false) == ZoneTake::Edited,
          "Q1 (a): inside, within 150 px, back where its spot was left, or an edit of the one in use -- else it waits");
    check(zone_outside_px(z, 1700, 400) == 0 && zone_outside_px(z, 2271, 300) == 150 &&
              zone_outside_px(z, 1400, 700) == 180 && std::string(zone_take_name(ZoneTake::Back)) == "back where it left",
          "how far outside: the larger of across and up or down");
    check(kZoneTakePx == Hunter::kSpotBackPx && !kZoneTakeAnywhere, "Q1's 150 px is the spot's own way back");
    check(kSpotHereMovesZone, "Q2 (a): Đặt chỗ này moves the saved zone");
    ZoneLimits lim;
    lim.w_px = 3200;
    lim.h_px = 1280;
    check(zone_max_size(lim).x == 3200 && zone_max_size(lim).y == 1280, "Q3 (a): a zone as large as the map");
    HuntZone whole;
    whole.left = 0;
    whole.top = 0;
    whole.right = 3199;
    whole.bottom = 1279;
    whole.home_x = 100;
    whole.home_y = 100;
    lim.floors = false;
    check(zone_valid(whole, lim, nullptr), "the whole map, as one zone: valid");

    // A new zone round a point, and one moved.
    const GameMapBook::Page page = check_floors().page("t9_s3");
    const ZoneLimits pl = zone_limits(&page, false);
    const std::optional<HuntZone> around = zone_around(&page, { 600, 495 }, 300, 100, pl);
    check(around && around->home_x == 600 && around->home_y == 499 && around->left == 300 && around->right == 900 &&
              around->top == 399 && around->bottom == 599 && zone_valid(*around, pl, &page),
          "Tạo vùng quanh: the home snapped onto its floor, the box 300 and 100 round it");
    const std::optional<HuntZone> edge = zone_around(&page, { 30, 990 }, 300, 100, pl);
    check(edge && edge->home_x == 30 && edge->home_y == 999 && edge->left == 0 && edge->right == 330 &&
              edge->bottom == 1023,
          "at the map's corner: cut to the map");
    const std::optional<HuntZone> bare = zone_around(nullptr, { 5, 5 }, 300, 100, lim);
    const std::optional<HuntZone> down = zone_around(&page, { 1700, 300 }, 300, 100, pl);
    check(bare && bare->home_x == 5 && bare->home_y == 5 && bare->left == 0 && bare->right == 305 && down &&
              down->home_y == 999,
          "with no floors: the home where it was asked; with them, snapped -- here onto the bottom ground");
    const std::optional<HuntZone> moved = zone_moved(&page, *around, { 1100, 495 }, pl);
    check(moved && moved->home_x == 1100 && moved->home_y == 499 && moved->left == 800 && moved->right == 1400 &&
              moved->top == 399 && moved->bottom == 599,
          "Dời vùng tới: the size and the home's place in it kept");
    const std::optional<HuntZone> slid = zone_moved(&page, *around, { 2000, 990 }, pl);
    check(slid && slid->home_x == 2000 && slid->home_y == 999 && slid->right == 2047 && slid->left == 1447 &&
              slid->bottom == 1023 && slid->top == 823 && zone_valid(*slid, pl, &page),
          "moved to the map's corner: slid back onto it, the same size");
    HuntZone with_floor = *around;
    with_floor.saved = "2026-11-04 21:36";
    check(same_place(with_floor, *around) && zone_text(z) == "home 1821,359, box 1521..2121 x 200..520",
          "same_place ignores when it was saved; the log's words");
}

// ------------------------------------------------------------- spots.ini

void test_zone_book() {
    section("Map tab: configs\\spots.ini -- read, written in place, never over one that does not read");

    const std::string dir = temp_path("map_tab_zones");
    make_folder(dir);
    const std::string path = dir + "\\spots.ini";
    check(zone_stem("t52_s2") == "t52_s2" && zone_stem("T52_S2") == "t52_s2" && zone_stem("t52_s2.bak").empty() &&
              zone_stem("..\\t1_s1").empty() && zone_stem("t_s1").empty() && zone_stem("t1_s").empty() &&
              zone_stem("Notes").empty(),
          "a stem: lowercase t<digits>_s<digits>, nothing else");

    ZoneBook book;
    check(book.load(path) == ZoneBook::Load::Missing && book.size() == 0, "no file: an empty book");
    HuntZone a = owner_zone();
    a.saved = "2026-11-04 21:36";
    check(book.put("T52_S2", a) && book.get("t52_s2") && !book.put("..\\evil", a) && book.size() == 1,
          "put: a stem in any case, kept lowercase; one that is no map's refused");
    const std::uint64_t r1 = book.revision("t52_s2");
    book.put("t52_s2", a);
    check(book.revision("t52_s2") > r1 && book.revision("t41_s9") == 0, "each put moves its stem's revision");

    // A round trip.
    check(ZoneBook::save_many(path, { { "t52_s2", a } }) && !path_exists(path + ".new"),
          "saved: written, and no .new left");
    ZoneBook back;
    std::optional<HuntZone> got;
    check(back.load(path) == ZoneBook::Load::Ok && (got = back.get("t52_s2")) && same_place(*got, a) &&
              got->saved == a.saved,
          "read back: the same zone, and when it was saved");
    const std::string text = read_text(path);
    check(text.rfind("# GoBot: hunting areas drawn on the Bản đồ tab, one per map.", 0) == 0 &&
              text.find("[t52_s2]") != std::string::npos && text.find("home_x=1821") != std::string::npos,
          "the file begins with its header, then the section");

    // Hand edits: other sections, unknown keys, a section that does not
    // read, one in capitals.
    write_text(path, "# a note of the owner's\n[Notes]\nowner=bạn\n\n[t52_s2]\nhome_x=1821\nhome_y=359\nleft=1521\n"
                     "right=2121\ntop=200\nbottom=520\nsaved=x\ncolour=blue\n\n[t41_s9]\nhome_x=abc\nleft=1\n\n"
                     "[T7_S1]\nhome_x=150\nhome_y=150\nleft=100\nright=300\ntop=100\nbottom=200\nmark=1\n");
    ZoneBook hand;
    check(hand.load(path) == ZoneBook::Load::Ok && hand.size() == 2 && hand.get("t52_s2") && hand.get("t7_s1") &&
              hand.skipped() == 1 && !hand.get("t41_s9"),
          "a section that does not read is left out -- one in capitals found as lowercase");
    HuntZone b = a;
    b.left = 1500;
    HuntZone c7;
    c7.home_x = 160;
    c7.home_y = 160;
    c7.left = 110;
    c7.right = 310;
    c7.top = 110;
    c7.bottom = 210;
    check(ZoneBook::save_many(path, { { "t52_s2", b }, { "t7_s1", c7 } }), "two zones saved at once");
    IniFile ini;
    const bool read = ini.load(path);
    check(read && ini.get("Notes", "owner") == "bạn" && ini.get("t52_s2", "colour") == "blue" &&
              ini.get("t52_s2", "left") == "1500" && ini.get("t41_s9", "home_x") == "abc" &&
              ini.get("t41_s9", "left") == "1" && ini.get("t7_s1", "mark") == "1" && ini.get("t7_s1", "left") == "110" &&
              !ini.has_section("T7_S1"),
          "every other section and key kept as it was; [T7_S1] merged into [t7_s1], its own key with it");
    check(ZoneBook::save_many(path, { { "t52_s2", std::nullopt } }) && ini.load(path) && !ini.has_section("t52_s2") &&
              ini.has_section("t7_s1") && ini.has_section("Notes") && ini.has_section("t41_s9"),
          "erased: that section gone, the rest kept");
    check(!ZoneBook::save_many(path, { { "..\\evil", a } }) && !ZoneBook::save_many(path, { { "T52_S2", a } }) &&
              ini.load(path) && !ini.has_section("t52_s2"),
          "a stem that is no map's, or not lowercase: refused, nothing written");
    HuntZone wide;
    wide.home_x = 11'000;
    wide.home_y = 300;
    wide.left = 10'500;
    wide.right = 12'000;
    wide.top = 200;
    wide.bottom = 400;
    ZoneBook big;
    check(ZoneBook::save_many(path, { { "t9_s9", wide } }) && big.load(path) == ZoneBook::Load::Ok &&
              big.get("t9_s9") && big.get("t7_s1"),
          "a zone past 10240 px -- as wide as a .map may be -- read back");

    // A folder by the file's name: it does not read, and is never written over.
    const std::string folder = dir + "\\folder\\spots.ini";
    make_folder(dir + "\\folder");
    make_folder(folder);
    ZoneBook bad;
    check(bad.load(folder) == ZoneBook::Load::Bad && !ZoneBook::save_many(folder, { { "t52_s2", a } }) &&
              is_folder(folder) && !path_exists(folder + ".new"),
          "a folder named spots.ini: Bad, and save_many writes nothing -- not even the .new");

    // The queue: the latest value per stem, one batch out at a time.
    ZoneSaveQueue q;
    HuntZone a1 = a, a2 = a, bz = b;
    a2.left = 1400;
    q.commit("t1_s1", a1);
    std::optional<ZoneSaveQueue::Batch> first = q.take();
    q.commit("t1_s1", a2);
    const bool waits = !q.take() && q.out() && q.owed();
    q.done(*first, false);
    std::optional<ZoneSaveQueue::Batch> again = q.take();
    check(first && first->size() == 1 && waits && again && again->size() == 1 && again->at("t1_s1") &&
              again->at("t1_s1")->left == 1400,
          "A1 out, A2 committed, A1 failed: A2 owed, never A1 over it");
    q.commit("t2_s1", bz);
    check(!q.take() && q.out(), "B committed while A is out: it waits");
    q.done(*again, true);
    std::optional<ZoneSaveQueue::Batch> next = q.take();
    check(next && next->size() == 1 && next->count("t2_s1") == 1 && !q.owed(), "A done: B taken");
    q.done(*next, false);
    q.commit("t3_s1", std::nullopt);
    std::optional<ZoneSaveQueue::Batch> both = q.take();
    check(both && both->size() == 2 && !both->at("t3_s1"), "B failed: owed again beside a delete");
}

// -------------------------------------------------------------- identity

WorldFix fix_at(bool map_known, std::uint16_t map, std::uint16_t area, std::uint32_t epoch) {
    WorldFix f;
    f.state = WorldFix::State::Known;
    f.map_known = map_known;
    f.map = map;
    f.area = area;
    f.epoch = epoch;
    return f;
}

void test_identity() {
    section("Map tab: which map the tab knows -- an entry, a name, and when either is still to be gone by");

    const std::string dir = tab_maps();
    if (dir.empty()) return;
    GameMapBook book(dir);
    MapTrust::Report none;
    KnownMap id = map_identity(fix_at(true, 52, 2, 3), none, 0);
    check(id.by == By::Entry && id.stem == "t52_s2" && id.epoch == 3 && !id.file_doubtful, "an entry: its stem");
    MapTrust::Report off = none;
    off.why = Why::Mismatch;
    check(map_identity(fix_at(true, 52, 2, 3), off, 0).file_doubtful &&
              map_identity(fix_at(true, 52, 2, 3), off, 0).by == By::Entry,
          "an entry whose file MapTrust found off: still the entry's map, its file doubtful");
    WorldFix gone = fix_at(true, 52, 2, 3);
    gone.state = WorldFix::State::NoCapture;
    check(map_identity(gone, none, 0).by == By::None, "no capture: none -- a door may be gone through unseen");

    MapTrust t(&book, "Map view");
    Feed f;
    for (int i = 0; i < 11; ++i) f.stand(t, 40 + i * 70, 199);
    check(map_identity(fix_at(false, 0, 0, 0), t.report(), 0).by == By::None, "naming: none yet");
    f.stand(t, 900, 199);
    id = map_identity(fix_at(false, 0, 0, 0), t.report(), 7);
    check(id.by == By::Named && id.stem == "t8_s1" && id.trust_gen == 7, "named and trusted: Named");
    id = map_identity(fix_at(true, 8, 2, 1), t.report(), 7);
    check(id.by == By::Entry && id.stem == "t8_s2", "an entry beats a name");

    MapTrust nogate(&book);
    Feed g;
    for (int i = 0; i < 12; ++i) g.stand(nogate, 40 + i * 70, 399);
    id = map_identity(fix_at(false, 0, 0, 0), nogate.report(), 0);
    check(id.by == By::Named && id.stem == "t8_s3" && nogate.report().why == Why::NoGates,
          "named with no gate (NoGates): Named all the same");
    book.distrust("t8_s1");
    MapTrust wrong(&book);
    Feed h;
    for (int i = 0; i < 12; ++i) h.stand(wrong, 40 + i * 70, 199);
    check(map_identity(fix_at(false, 0, 0, 0), wrong.report(), 0).by == By::None &&
              wrong.report().why == Why::Distrusted,
          "named, but a drop found the file wrong: none -- a doubtful named file is not the map");

    // usable_identity: the GUI's log-in count, the epoch a log in left, the live epoch.
    KnownMap entry = map_identity(fix_at(true, 52, 2, 5), none, 2);
    check(usable_identity(entry, 2, false, 0, 5).by == By::Entry, "as it was had: usable");
    check(usable_identity(entry, 3, false, 0, 5).by == By::None, "had under an older log in (trust_gen): none");
    check(usable_identity(entry, 2, true, 5, 5).by == By::None && usable_identity(entry, 2, true, 4, 5).by == By::Entry,
          "an entry of the epoch the log in left: none; a later entry: usable");
    check(usable_identity(entry, 2, false, 0, 6).by == By::None &&
              usable_identity(entry, 2, false, 0, 6).epoch == 5,
          "another map entry since (the live fix's epoch): none, until the job catches up");
    KnownMap named = id;
    named.by = By::Named;
    named.epoch = 5;
    named.trust_gen = 2;
    check(usable_identity(named, 2, true, 5, 5).by == By::Named, "a name had after the log in, at its epoch: usable");
    // The log in's epoch: the keeper's crossing began at epoch 9; entries
    // counted during it are the new game's.
    const std::uint32_t lose = lose_epoch_for(1'000'000, 1'000'000 - 60'000, 9, 11);
    KnownMap during = map_identity(fix_at(true, 52, 2, 10), none, 1);
    check(lose == 9 && usable_identity(during, 1, true, lose, 10).by == By::Entry &&
              lose_epoch_for(1'000'000, 1'000'000 - kCrossKeepMs, 9, 11) == 11 &&
              lose_epoch_for(1'000'000, 0, 9, 11) == 11,
          "a log in within 5 min of the crossing: the crossing's epoch -- an entry during it is usable; later, or "
          "none: the place's");
}

// --------------------------------------------------------------- the job

/// The worker's job on a made-up world: you at a new settled report each
/// stand(), the capture healthy.
struct Job {
    GameMapBook&       book;
    MapViewState       ws;
    PacketWorld::View  v;
    PacketWorld::Stats s;
    std::uint64_t      now = 100'000;
    std::uint64_t      gen = 0;
    int                gate_px = 150;

    explicit Job(GameMapBook& b) : book(b) {}
    MapViewResult run() {
        PacketLink link;
        link.npcap = true;
        link.capturing = true;
        link.last_tcp_ms = now;
        return MapViewJob::run(v, s, link, now, gen, gate_px, book, ws);
    }
    MapViewResult stand(int x, int y) {
        now += 300;
        v.me.known = true;
        v.me.x = v.me.report_x = static_cast<float>(x);
        v.me.y = v.me.report_y = static_cast<float>(y);
        v.me.report_ms = now;
        s.last_own_ms = now;
        return run();
    }
    /// Twelve points along t8_s1's platform; then runs a second apart while
    /// the files are read -- every file is read at most every kNameRescanMs.
    MapViewResult name_t8_s1() {
        MapViewResult r;
        for (int i = 0; i < 12; ++i) r = stand(40 + i * 70, 199);
        for (int i = 0; i < 40 && r.id.by == By::None; ++i) {
            now += 1000;
            r = run();
        }
        return r;
    }
};

void test_view_job() {
    section("Map tab: the worker's job (MapViewJob::run) on made-up map files");

    const std::string dir = tab_maps();
    if (dir.empty()) return;
    const std::string table = temp_path("map_tab_tb_Map.tbl");
    MapTable::Entry named_row;
    named_row.theme = 8;
    named_row.stage = 1;
    named_row.name = "Rừng Thử";
    MapTable::Entry other_row;
    other_row.theme = 8;
    other_row.stage = 2;
    other_row.name = "Hang Thử";
    check(write_bytes(table, table_bytes({ other_row, named_row })), "a made-up tb_Map.tbl written");
    GameMapBook book(dir);
    Job j(book);
    j.ws.table_path = [table] { return table; };

    MapViewResult r;
    const std::vector<std::string> log = info_lines([&] { r = j.name_t8_s1(); });
    check(r.id.by == By::Named && r.id.stem == "t8_s1" && r.page && r.sketch && r.sketch->runs.size() == 2 &&
              r.gates.size() == 1 && r.learned_gates == 0 && r.map_name == "Rừng Thử" && r.data_dir_ok &&
              r.fix.state == WorldFix::State::Known,
          fmt("12 standing points on t8_s1: named, its page and sketch (2 runs), its gate and its name from tb_Map "
              "(%s, %s)",
              r.id.stem.c_str(), r.map_name.c_str()));
    check(lines_with(log, "Map view: map named from 12 standing points: t8_s1") == 1 &&
              lines_with(log, "Map view: t8_s1 -- 1024x640 px, 2048 floor columns -> 2 runs / 4 points, 1 gates (+0 "
                              "learned)") == 1 &&
              lines_with(log, "Hunter:") == 0,
          "logged as the map view's, the map's read once with its sizes -- never as the hunter's");
    const std::shared_ptr<const MapSketch> first = r.sketch;
    for (int i = 0; i < 3; ++i) r = j.stand(500 + i * 9, 199);
    check(r.sketch == first && j.ws.sketches == 1 && r.no_drop && r.no_drop_px == 150,
          "more runs on the same map: the sketch built once, the no-drop marks kept");
    j.gate_px = 200;
    const std::shared_ptr<const std::vector<MapRun>> marks = r.no_drop;
    r = j.stand(520, 199);
    check(r.no_drop != marks && r.no_drop_px == 200 && r.sketch == first, "drop_gate_px changed: the marks built again");

    // A capture started again: named afresh.
    ++j.s.resets;
    r = j.stand(530, 199);
    check(r.id.by == By::None && r.capture_gap && j.ws.view_trust.stem().empty(),
          "Stats::resets moved: the next run knows no map until it is named again");
    r = j.name_t8_s1();
    check(r.id.by == By::Named && j.ws.sketches == 1, "named again -- the same map's sketch not built again");
    // Not looked at for 2.5 s: the same.
    j.now += 2500;
    r = j.run();
    check(r.id.by == By::None && r.capture_gap, "2.5 s with no run: the next knows no map");
    r = j.name_t8_s1();
    // Logged back in: lose().
    j.gen = 1;
    r = j.stand(600, 199);
    check(r.id.by == By::None && r.id.trust_gen == 1 && j.ws.lost_gen == 1 && !r.capture_gap,
          "trust_gen moved on: the tab's MapTrust let go -- named afresh");

    // A map entry: its map at once, the spawn learned as a gate.
    j.v.map_since_ms = j.now;
    j.v.map = 8;
    j.v.area = 2;
    j.v.map_epoch = 1;
    j.v.entry_x = 300;
    j.v.entry_y = 290;
    r = j.stand(320, 299);
    check(r.id.by == By::Entry && r.id.stem == "t8_s2" && r.id.epoch == 1 && r.page && r.sketch &&
              j.ws.sketches == 2 && r.gates.size() == 2 && r.learned_gates == 1 && r.map_name == "Hang Thử" &&
              !r.id.file_doubtful,
          fmt("a map entry to 8/2: t8_s2, its sketch, its gate and the spawn learned (%zu gates), its name",
              r.gates.size()));
    // What jumps up learned of it (2026.11.05b), beside the gates: a point a
    // key did not get up from, and a floor solid from below, followed end to
    // end for the tab to draw grey.
    check(r.no_jumps.empty() && r.solid.empty() && r.solid_runs.empty(), "nothing learned by jumps up yet: nothing");
    book.learn_no_jump("t8_s2", GameMapBook::NoJump{ { 320, 599 }, { 320, 299 }, 2 });
    book.learn_solid("t8_s2", { 320, 299 });
    r = j.stand(330, 299);
    check(r.no_jumps.size() == 1 && r.no_jumps[0].take.x == 320 && r.no_jumps[0].land.y == 299 &&
              r.no_jumps[0].kind == 2 && r.solid.size() == 1 && r.solid_runs.size() == 1 &&
              r.solid_runs[0].x0 <= 8 && r.solid_runs[0].x1 >= 1015 && r.solid_runs[0].y0 == 300 &&
              r.solid_runs[0].y1 == 300,
          fmt("a Ctrl + Up that fell back at 320 and the floor at 300 solid: both in the job's result, the floor drawn "
              "%d..%d",
              r.solid_runs.empty() ? -1 : r.solid_runs[0].x0, r.solid_runs.empty() ? -1 : r.solid_runs[0].x1));
    j.v.map_epoch = 2;
    j.v.area = 9;
    r = j.stand(320, 299);
    check(r.id.by == By::Entry && r.id.stem == "t8_s9" && !r.page && !r.sketch && !r.no_drop &&
              r.gates.size() == 1 && r.learned_gates == 1 && r.map_name.empty(),
          "an entry to a map with no file: known, no page, only the spawn learned");

    // No game data folder at all: positions only.
    GameMapBook nothing{ std::string() };
    Job k(nothing);
    r = k.stand(100, 100);
    check(r.id.by == By::None && !r.data_dir_ok, "no data folder: no map, and the job says so");
}

// ------------------------------------------------------- the tab's rules

/// An identity as the GUI copies it from the job.
KnownMap ident(By by, const char* stem, std::uint32_t epoch, Why why = Why::Trusted) {
    KnownMap id;
    id.by = by;
    id.stem = stem;
    id.epoch = epoch;
    id.report.why = why;
    id.report.stem = stem;
    id.report.named = by == By::Named;
    return id;
}

void test_tab_rules() {
    section("Map tab: what the GUI decides each frame -- the status line, editing, the hand-off, the card's line");

    // What the status line says, the first row of §1.3 that holds.
    MapShowsIn in;
    in.state = WorldFix::State::Known;
    in.live_epoch = 4;
    in.ran = true;
    in.page = true;
    in.id = in.usable = ident(By::Entry, "t52_s2", 4);
    check(map_shows(in) == MapShows::ByEntry && map_drawn(MapShows::ByEntry), "known by an entry, with its file: drawn");
    in.usable = in.id = ident(By::Named, "t52_s2", 4);
    check(map_shows(in) == MapShows::ByName, "known from standing points");
    in.usable.report.why = Why::NoGates;
    check(map_shows(in) == MapShows::NoGates && map_drawn(MapShows::NoGates),
          "a map with no gate: drawn, its own line about the drops");
    in.usable = in.id = ident(By::Entry, "t52_s2", 4, Why::Mismatch);
    in.usable.file_doubtful = true;
    check(map_shows(in) == MapShows::EntryDoubtful && map_drawn(MapShows::EntryDoubtful),
          "an entry whose file was found off: drawn, faded");
    in.page = false;
    check(map_shows(in) == MapShows::NoFile && !map_drawn(MapShows::NoFile), "known, no file: positions only");
    in.page = true;
    in.data_dir_ok = false;
    check(map_shows(in) == MapShows::NoGame, "no game data folder: positions only");
    in.data_dir_ok = true;
    in.live_epoch = 5;
    check(map_shows(in) == MapShows::Changing && !map_drawn(MapShows::Changing),
          "an entry the job has not looked at yet: changing map, nothing of the old one drawn");
    in.live_epoch = 4;
    KnownMap none;
    none.epoch = 4;
    none.report.why = Why::Naming;
    in.id = in.usable = none;
    check(map_shows(in) == MapShows::Naming, "not known, naming");
    in.usable.report.why = Why::Confirming;
    check(map_shows(in) == MapShows::Naming, "not known, an entry's file being confirmed: the same line");
    in.usable.report.why = Why::Ambiguous;
    check(map_shows(in) == MapShows::Ambiguous, "two files fit: ambiguous");
    in.usable.report.why = Why::Distrusted;
    in.usable.report.named = true;
    check(map_shows(in) == MapShows::NamedDoubtful, "the file named was found wrong: said, not used");
    in.usable.report.named = false;
    in.usable.report.why = Why::NoFiles;
    check(map_shows(in) == MapShows::NoGame, "no map files at all: no game");
    in.usable.report.why = Why::NoCapture;
    check(map_shows(in) == MapShows::NothingYet, "nothing under way: not known yet");
    // An entry still in the job's copy, but at the epoch a log in left: not
    // usable, so not drawn.
    in.id = ident(By::Entry, "t52_s2", 4, Why::Confirming);
    in.usable = usable_identity(in.id, 1, true, 4, 4);
    check(in.usable.by == By::None && map_shows(in) == MapShows::Naming,
          "an entry the log in left: not drawn, whatever the job's copy says");
    in.state = WorldFix::State::NoCapture;
    check(map_shows(in) == MapShows::NoCapture, "no capture: said first");
    in.positions_off = true;
    check(map_shows(in) == MapShows::PositionsOff, "positions off: said before anything");

    // Editing.
    const KnownMap known = ident(By::Entry, "t52_s2", 4);
    check(map_edit(true, false, known, MapShows::ByEntry) == MapEdit::Ok &&
              map_edit(false, false, known, MapShows::ByEntry) == MapEdit::BookLoading &&
              map_edit(true, true, known, MapShows::ByEntry) == MapEdit::BookBad &&
              map_edit(false, true, known, MapShows::ByEntry) == MapEdit::BookBad &&
              map_edit(true, false, known, MapShows::Changing) == MapEdit::Changing &&
              map_edit(true, false, none, MapShows::Naming) == MapEdit::MapUnknown &&
              map_edit(true, false, known, MapShows::NoGame) == MapEdit::NoGame &&
              map_edit(true, false, known, MapShows::NoFile) == MapEdit::Ok &&
              map_edit(true, false, known, MapShows::EntryDoubtful) == MapEdit::Ok,
          "editing: only with spots.ini read and readable, the map known at this entry and the game's map folder "
          "there -- a map with no file, or a doubtful one, edited without its floors");
    check(map_edit(true, false, known, MapShows::PositionsOff) == MapEdit::MapUnknown &&
              map_edit(true, false, known, MapShows::NoCapture) == MapEdit::MapUnknown,
          "editing: not with the positions off or no capture, whatever map was known last -- it may be gone by");

    // The hand-off: a zone on t8_s1's platform at 200, the bottom at 600.
    Floors fl(32, 20);
    fl.flat(0, 1023, 200, 1);
    fl.flat(0, 1023, 600, 2);
    const GameMapBook::Page page = fl.page("t8_s1");
    HuntZone z;
    z.home_x = 500;
    z.home_y = 199;
    z.left = 300;
    z.right = 700;
    z.top = 100;
    z.bottom = 300;
    const KnownMap entry = ident(By::Entry, "t8_s1", 6);
    check(zone_send(entry, z, &page, false, 0, {}) == ZoneSend::Send, "an entry's map, the zone on its floors: sent");
    check(zone_send(none, z, &page, true, 6, { 500, 199 }) == ZoneSend::NoMap, "no map: not sent");
    HuntZone off = z;
    off.home_y = 250;
    check(zone_send(entry, off, &page, false, 0, {}) == ZoneSend::DoesNotFit,
          "its home on no floor of the file: not sent");
    HuntZone wide = z;
    wide.right = 1100;
    check(zone_send(entry, wide, &page, false, 0, {}) == ZoneSend::DoesNotFit, "off the map's 1024 px: not sent");
    KnownMap doubtful = entry;
    doubtful.file_doubtful = true;
    check(zone_send(doubtful, off, &page, false, 0, {}) == ZoneSend::Send && zone_send(entry, off, nullptr, false, 0, {}) ==
                                                                               ZoneSend::Send,
          "a doubtful file, or none: no floor test, sent");
    const KnownMap named = ident(By::Named, "t8_s1", 6);
    check(zone_send(named, z, &page, true, 6, { 400, 199 }) == ZoneSend::Send,
          "named: sent once this entry's settled report is on its floors");
    KnownMap named_off = named;
    named_off.report.own_off = true;
    check(zone_send(named, z, &page, false, 0, {}) == ZoneSend::NotSure &&
              zone_send(named, z, &page, true, 5, { 400, 199 }) == ZoneSend::NotSure &&
              zone_send(named, z, &page, true, 6, { 400, 250 }) == ZoneSend::NotSure &&
              zone_send(named_off, z, &page, true, 6, { 400, 199 }) == ZoneSend::NotSure &&
              zone_send(named, z, nullptr, true, 6, { 400, 199 }) == ZoneSend::NotSure,
          "named: not sent with no settled report, one of another entry, one off its floors, one of yours off them "
          "since, or no page");

    // A zone put where the character stands, and this run's spot kept: on a
    // named map held to what zone_send holds it to.
    KnownMap entry_off = entry;
    entry_off.report.own_off = true;
    check(zone_here_ok(entry, nullptr, { 400, 250 }) && zone_here_ok(entry_off, &page, { 400, 250 }),
          "where the character stands, by an entry's map: put, whatever the file says");
    check(zone_here_ok(named, &page, { 400, 199 }), "where the character stands, named, on a floor of the file: put");
    check(!zone_here_ok(named_off, &page, { 400, 199 }) && !zone_here_ok(named, &page, { 400, 250 }) &&
              !zone_here_ok(named, nullptr, { 400, 199 }),
          "where the character stands, named: not put with one of yours off its floors, the point off them, or no "
          "page");
    check(spot_keep_ok(entry) && spot_keep_ok(entry_off) && spot_keep_ok(named) && !spot_keep_ok(named_off),
          "this run's spot kept as the map's zone: not on a named map with one of yours off its floors");

    // The card's line.
    ZoneCardIn c;
    c.hunt_enabled = c.spot = c.hunting = c.book_loaded = c.book_any = c.zone = true;
    c.stem = "t8_s1";
    c.send = ZoneSend::Send;
    Hunter::Status hs;
    check(zone_card(c, hs) == ZoneCard::Other, "sent, not taken yet: the hunter's own line");
    hs.spot_set = hs.spot_from_map = true;
    hs.spot_stem = "t8_s1";
    check(zone_card(c, hs) == ZoneCard::InUse, "the hunter's spot is this zone: in use");
    // 2026.11.04d (F5): outside its box, never the green "in use".
    hs.spot_outside_px = 0;
    check(zone_card(c, hs) == ZoneCard::InUse, "in its box (outside 0 px): in use");
    hs.spot_outside_px = 522;
    check(zone_card(c, hs) == ZoneCard::OutsideArea, "522 px outside its box, as at 1402,802: outside the area");
    hs.spot_why = Hunter::SpotWhy::NoReturn;
    check(zone_card(c, hs) == ZoneCard::InUse, "... with no way back: the hunter's own NoReturn line, as in use");
    hs.spot_why = Hunter::SpotWhy::None;
    hs.spot_suspended = true;
    check(zone_card(c, hs) == ZoneCard::Suspended, "outside and set aside: suspended");
    hs.spot_outside_px = -1;
    check(zone_card(c, hs) == ZoneCard::Suspended, "set aside: suspended");
    hs.spot_stem = "t9_s1";
    hs.zone_wait = Hunter::ZoneWait::Outside;
    hs.zone_wait_stem = "t8_s1";
    check(zone_card(c, hs) == ZoneCard::Outside, "another map's spot aside, this one waiting outside: outside");
    hs.zone_wait = Hunter::ZoneWait::NotSettled;
    check(zone_card(c, hs) == ZoneCard::Waiting, "waiting to stand settled");
    hs.zone_wait = Hunter::ZoneWait::None;
    c.send = ZoneSend::DoesNotFit;
    check(zone_card(c, hs) == ZoneCard::DoesNotFit, "not on the file's floors");
    c.send = ZoneSend::NotSure;
    check(zone_card(c, hs) == ZoneCard::Waiting, "named, not sure yet: waiting");
    c.hunting = false;
    check(zone_card(c, hs) == ZoneCard::NotHunting, "no hunt: press BẮT ĐẦU");
    c.spot = false;
    check(zone_card(c, hs) == ZoneCard::SpotOff, "the spot not ticked: the zone not used");
    c.spot = c.hunting = true;
    c.zone = false;
    hs = Hunter::Status{};
    check(zone_card(c, hs) == ZoneCard::Empty, "no zone, no spot: how to make one");
    hs.spot_set = true;
    check(zone_card(c, hs) == ZoneCard::SessionSpot, "no zone: this run's spot, and how to keep it");
    hs.spot_from_map = true;
    hs.spot_stem = "t9_s1";
    check(zone_card(c, hs) == ZoneCard::Other, "no zone here, another map's zone spot: the hunter's own line");
    c.stem.clear();
    check(zone_card(c, hs) == ZoneCard::MapUnknown, "hunting, zones saved, the map not known: said");
    c.book_any = false;
    check(zone_card(c, hs) == ZoneCard::NoMap, "nothing saved anywhere: just the map not known");
    c.book_loaded = false;
    check(zone_card(c, hs) == ZoneCard::BookLoading, "spots.ini not read yet");
    c.book_bad = true;
    check(zone_card(c, hs) == ZoneCard::BookBad, "spots.ini unreadable: before the loading");
    c.hunt_enabled = false;
    check(zone_card(c, hs) == ZoneCard::HuntOff, "Tự đánh quái off: said first");

    // This run's spot as a zone.
    const ZoneLimits lim = zone_limits(&page, false);
    std::optional<HuntZone> s = zone_from_spot(&page, 500.4f, 197.0f, 200.4f, 97.0f, 800.4f, 297.0f, lim);
    check(s && s->left == 200 && s->right == 800 && s->top == 97 && s->bottom == 297 && s->home_x == 500 &&
              s->home_y == 199 && zone_valid(*s, lim, &page),
          "this run's spot as a zone: its box rounded, its home snapped to the floor it stands on");
    s = zone_from_spot(&page, 100.0f, 199.0f, -200.0f, 99.0f, 400.0f, 299.0f, lim);
    check(s && s->left == 0 && s->right == 400 && s->home_x == 100, "cut to the map");
    check(!zone_from_spot(&page, 500.0f, 400.0f, 300.0f, 380.0f, 700.0f, 420.0f, lim),
          "no floor in its box near its home: none");
    s = zone_from_spot(nullptr, 500.0f, 400.0f, 300.0f, 380.0f, 700.0f, 420.0f, zone_limits(nullptr, false));
    check(s && s->home_x == 500 && s->home_y == 400, "no page: the spot itself, no floor to snap to");
}

// ------------------------------------------------- where a dot is drawn

/// A living monster as the world works it out: its frame at fx, fy, followed
/// to x -- walking or not -- at the frame's height, as follow_mob leaves it.
PacketWorld::Mob dot_mob(float fx, float fy, float x, bool moving) {
    PacketWorld::Mob m;
    m.slot = 7;
    m.gen = 3;
    m.frame_x = fx;
    m.frame_y = fy;
    m.x = x;
    m.y = fy;
    m.moving = moving;
    m.facing = x < fx ? -1 : 1;
    m.speed = moving ? 2.6f : 0.0f;
    m.hp = m.max_hp = 100;
    m.max_hp_known = true;
    m.event_ms = 10000;
    return m;
}

const char* dot_why(DotWhy w) {
    switch (w) {
        case DotWhy::AsIs:     return "as is";
        case DotWhy::Followed: return "followed";
        case DotWhy::Held:     return "held";
        case DotWhy::SetDown:  return "set down";
        case DotWhy::Hidden:   return "hidden";
        case DotWhy::Projected: return "projected";
    }
    return "?";
}

std::string dot_text(const MapDot& d) { return fmt("%.1f,%.1f %s", d.x, d.y, dot_why(d.why)); }

bool dot_is(const MapDot& d, double x, double y, DotWhy why) {
    return std::abs(d.x - x) < 0.01 && std::abs(d.y - y) < 0.01 && d.why == why;
}

/// The dots' made-up map, 3200 x 1280: the ground at 1000 all the way across;
/// a ramp from 200 to 599, half a pixel down a column from 600, with a hole
/// at 400; a platform at 400 from 1000 to 1199, a wall after it; and a wall
/// at 960 over the ground at 1490-1510.
GameMapBook::Page dot_page() {
    Floors fl(100, 40);
    fl.flat(0, 3199, 1000, 2);
    fl.run(200, 399, [](int x) { return 600 + (x - 200) / 2; }, 1);
    fl.run(401, 599, [](int x) { return 600 + (x - 200) / 2; }, 1);
    fl.flat(1000, 1199, 400, 1);
    fl.flat(1200, 1210, 400, 4);
    fl.flat(1490, 1510, 960, 4);
    return fl.page("t9_s7");
}

void test_dots() {
    section("Map tab, 2026.11.05: where a dot is drawn -- a walker on its floor, a spawn set down, the dead hidden, "
            "you on your floor (a made-up map)");

    const GameMapBook::Page p = dot_page();
    const std::uint64_t now = 12000;

    // A walk: the frame's floor followed to its x, the frame's height over
    // it kept -- 1 px, and 3.
    MapDot d = mob_dot(&p, dot_mob(250, 624, 350, true), now);
    check(dot_is(d, 350, 674, DotWhy::Followed),
          "a walker from 250,624, 1 px over the ramp, at x 350: on the ramp at 674, not at 624 -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(250, 622, 350, true), now);
    check(dot_is(d, 350, 672, DotWhy::Followed), "a frame 3 px over its floor: 3 px over it there -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(250, 624, 450, true), now);
    check(dot_is(d, 450, 724, DotWhy::Followed), "across the ramp's hole at 400: 724 at 450 -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(250, 624, 400, true), now);
    check(dot_is(d, 400, 698, DotWhy::Followed),
          "at the hole itself: the column before it, 699 -- as follow_to has it -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(450, 724, 250, true), now);
    check(dot_is(d, 250, 624, DotWhy::Followed), "back up the ramp, leftward over the hole -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(250, 624, 350, false), now);
    check(dot_is(d, 350, 674, DotWhy::Followed), "a walk run out, held where it got to: on its floor -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(250, 624, 250, true), now);
    check(dot_is(d, 250, 624, DotWhy::Followed), "a walk just begun: at its frame -- " + dot_text(d));

    // Where its floor ends first: held at the end, never off into the air.
    d = mob_dot(&p, dot_mob(500, 749, 700, true), now);
    check(dot_is(d, 599, 798, DotWhy::Held), "past the ramp's end at 599: held there -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(250, 624, 100, true), now);
    check(dot_is(d, 200, 599, DotWhy::Held), "past its other end at 200, leftward: held there -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(1100, 399, 1300, true), now);
    check(dot_is(d, 1199, 399, DotWhy::Held), "into the wall at 1200: held before it -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(3000, 999, 3500, true), now);
    check(dot_is(d, 3199, 999, DotWhy::Held), "past the map's edge: held at its last column -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(100, 999, 0, true), now);
    check(dot_is(d, 0, 999, DotWhy::Followed), "to the map's first column: there -- " + dot_text(d));

    // Bounded: never more than kDotFollowMaxPx columns a dot.
    d = mob_dot(&p, dot_mob(100, 999, 2000, true), now);
    check(dot_is(d, 2000, 999, DotWhy::Followed), "1,900 px along the ground: followed -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(100, 999, 3000, true), now);
    check(kDotFollowMaxPx == 2048 && dot_is(d, 100.0 + kDotFollowMaxPx, 999, DotWhy::Held),
          "a runaway walk 2,900 px on: held 2,048 px from its frame, the most followed -- " + dot_text(d));

    // A frame on no floor, or no map: as the packets have it.
    d = mob_dot(&p, dot_mob(250, 500, 350, true), now);
    check(dot_is(d, 350, 500, DotWhy::AsIs), "a walker whose frame stood on no floor: as it is -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(1205, 399, 1250, true), now);
    check(dot_is(d, 1250, 399, DotWhy::AsIs), "a frame on a wall: as it is -- " + dot_text(d));
    d = mob_dot(nullptr, dot_mob(250, 624, 350, true), now);
    check(dot_is(d, 350, 624, DotWhy::AsIs), "no map drawn: where the packets put it -- " + dot_text(d));

    // Standing: on its floor as it is; on none, set down on its own.
    d = mob_dot(&p, dot_mob(250, 624, 250, false), now);
    check(dot_is(d, 250, 624, DotWhy::AsIs), "standing on the ramp: as it is -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(700, 950, 700, false), now);
    check(dot_is(d, 700, 999, DotWhy::SetDown),
          "a spawn 50 px over the ground: on it, 1 px over its top (kDotStandPx) -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(1500, 940, 1500, false), now);
    check(dot_is(d, 1500, 999, DotWhy::SetDown), "a wall between: passed over, onto the ground -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(250, 631, 250, false), now);
    check(dot_is(d, 250, 624, DotWhy::SetDown),
          "6 px under the ramp's top: onto it, as Hunter::monster_floor takes 12 px up -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(700, 850, 700, false), now);
    check(dot_is(d, 700, 850, DotWhy::AsIs) && kDotStandPx == 1,
          "150 px over the ground, nothing within Hunter::kDropMonsterFloorPx: as it is -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(1050, 1100, 1050, false), now);
    check(dot_is(d, 1050, 1100, DotWhy::AsIs), "under the bottom ground, nothing below: as it is -- " + dot_text(d));
    d = mob_dot(nullptr, dot_mob(700, 950, 700, false), now);
    check(dot_is(d, 700, 950, DotWhy::AsIs), "no map drawn: a spawn where it is -- " + dot_text(d));

    // Dead: hidden at once (Q1 (a)); the hunter keeps its own 1.5 s.
    PacketWorld::Mob dead = dot_mob(250, 624, 250, false);
    dead.dead = true;
    dead.hp = 0;
    dead.dead_ms = now;
    const bool hidden = !mob_dot(&p, dead, now).shown() && !mob_dot(&p, dead, now + 10).shown() &&
                        !mob_dot(&p, dead, now - 15).shown() && !mob_dot(nullptr, dead, now).shown();
    dead.dead_ms = 0;
    check(hidden && mob_dot(&p, dead, now).why == DotWhy::Hidden && kMapDeadShownMs == 0 &&
              PacketEyes::kDeadShownMs == 1'500,
          "Q1 (a): a dead monster is not drawn -- at its kill, after it, a frame stamped after the tab's moment, "
          "with no map; the hunter's own kDeadShownMs stays 1.5 s");
    PacketWorld::Mob walker_dead = dot_mob(250, 624, 350, false);
    walker_dead.dead = true;
    walker_dead.dead_ms = now - 400;
    check(mob_dot(&p, walker_dead, now).why == DotWhy::Hidden, "killed mid-walk: hidden too");

    // You: on your floor while you walk on the ground; otherwise as now.
    WorldFix f;
    f.state = WorldFix::State::Known;
    f.moving = true;
    f.rx = 250.0f;
    f.ry = 624.0f;
    f.x = 450.0f;
    f.y = 624.0f;
    d = you_dot(&p, f, 450, 624);
    check(dot_is(d, 450, 724, DotWhy::Followed), "you walking down the ramp from 250,624: on it at 724 -- " + dot_text(d));
    f.uncertain = true;
    check(dot_is(you_dot(&p, f, 450, 624), 450, 724, DotWhy::Followed),
          "a walk followed past its time, held where it got to: still on its floor");
    f.uncertain = false;
    f.rx = 500.0f;
    f.ry = 749.0f;
    d = you_dot(&p, f, 700, 749);
    check(dot_is(d, 599, 798, DotWhy::Held), "past the ramp's end: held there until the fall's report -- " + dot_text(d));
    f.rx = 250.0f;
    f.ry = 624.0f;
    f.airborne = true;
    check(dot_is(you_dot(&p, f, 450, 590), 450, 590, DotWhy::AsIs), "in the air: as it is, the jump's own rise");
    f.airborne = false;
    f.state = WorldFix::State::Lost;
    check(dot_is(you_dot(&p, f, 300, 610), 300, 610, DotWhy::AsIs), "lost: where you were last known, as it is");
    f.state = WorldFix::State::Known;
    f.moving = false;
    check(dot_is(you_dot(&p, f, 250, 624), 250, 624, DotWhy::AsIs), "standing still: your report, as it is");
    f.moving = true;
    check(dot_is(you_dot(nullptr, f, 450, 624), 450, 624, DotWhy::AsIs), "no map drawn: as it is");
    f.ry = 500.0f;
    check(dot_is(you_dot(&p, f, 450, 500), 450, 500, DotWhy::AsIs), "a report on no floor: as it is");

    // Cheap: 64 runaway walkers, each at the bound, ten frames.
    std::vector<PacketWorld::Mob> many;
    for (int i = 0; i < 64; ++i) many.push_back(dot_mob(static_cast<float>(i), 999.0f, 3199.0f, true));
    const auto t0 = std::chrono::steady_clock::now();
    int held = 0;
    for (int frame = 0; frame < 10; ++frame)
        for (const PacketWorld::Mob& m : many) held += mob_dot(&p, m, now).why == DotWhy::Held ? 1 : 0;
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(held == 640 && ms < 1000,
          fmt("64 dots each followed the most, ten frames: %lld ms (all held at the bound: %d of 640)", ms, held));
}

void test_dots_t52s2() {
    section("Map tab, 2026.11.05: the dots on t52_s2's own floors -- walkers along its ramps, the spawns of "
            "2026-11-04/05 set down, you on your floor (copies in build\\handoff\\drop\\testdata)");

    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): t52_s2's dots\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    check(page != nullptr, "t52_s2 reads");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    const std::uint64_t now = 50000;

    // The fight ramp: 586 at 750, 715 at 1100, 802 at 1400. A walker from a
    // frame 1 px over it at 750, followed 100, 200 and 500 px -- across the
    // ramp's hole at 1056 for the last.
    const struct {
        float  x;
        double y;
    } ramp[] = { { 850.0f, 619.0 }, { 950.0f, 668.0 }, { 1250.0f, 733.0 } };
    for (const auto& r : ramp) {
        const MapDot d = mob_dot(&p, dot_mob(750, 585, r.x, true), now);
        const std::optional<int> top = p.follow_to(750, 585, static_cast<int>(r.x));
        check(top && dot_is(d, r.x, r.y, DotWhy::Followed) && std::abs(d.y - (*top - 1)) <= 3.0,
              fmt("a walker from 750,585 at x %.0f: on the ramp at %.0f (its top %d, less the frame's 1 px), where "
                  "the packets' y put it %d px over -- %s",
                  r.x, r.y, top ? *top : -1, top ? *top - 585 : -1, dot_text(d).c_str()));
    }
    MapDot d = mob_dot(&p, dot_mob(1250, 733, 750, true), now);
    check(dot_is(d, 750, 585, DotWhy::Followed), "up the ramp again, 500 px left: 585 at 750 -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(1000, 684, 1100, true), now);
    check(dot_is(d, 1100, 714, DotWhy::Followed), "the lower ramp from 1000,684 to 1100: 714 -- " + dot_text(d));

    // The ledge's end at 1104: held there, never on into the air at 274.
    d = mob_dot(&p, dot_mob(1000, 274, 1200, true), now);
    check(dot_is(d, 1104, 273, DotWhy::Held),
          "a walker on the spawn ledge followed to 1200: held at its end, 1104 -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(1360, 701, 1460, true), now);
    check(dot_is(d, 1404, 690, DotWhy::Held), "the small floor's end at 1404: held there -- " + dot_text(d));

    // The holes: 711 on the spawn ledge, 1056 on the upper ramp.
    d = mob_dot(&p, dot_mob(750, 272, 650, true), now);
    check(dot_is(d, 650, 272, DotWhy::Followed), "left along the ledge over its hole at 711 -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(750, 272, 711, true), now);
    check(dot_is(d, 711, 274, DotWhy::Followed), "at 711 itself: the column before it, 712's 275 -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(1000, 582, 1100, true), now);
    check(dot_is(d, 1100, 507, DotWhy::Followed),
          "up the upper ramp from 1000,582 over its hole at 1056: 507 at 1100 -- " + dot_text(d));
    d = mob_dot(&p, dot_mob(1000, 582, 1056, true), now);
    check(dot_is(d, 1056, 543, DotWhy::Followed), "at 1056 itself: 1055's 544 -- " + dot_text(d));

    // The spawns of the logs of 2026-11-04 20:39 and 2026-11-05, 15-66 px over
    // their floors (the "off t52_s2's floors" lines): each set down on it.
    const struct {
        int x, y, floor;
    } spawns[] = { { 793, 561, 598 },    { 484, 1013, 1076 }, { 2791, 917, 939 },  { 506, 1034, 1065 },
                   { 3043, 1102, 1117 }, { 3006, 949, 966 },  { 2425, 968, 985 },  { 3060, 1101, 1116 },
                   { 1114, 1018, 1084 }, { 1548, 330, 348 },  { 2615, 1107, 1122 }, { 610, 987, 1008 },
                   { 1065, 657, 706 },   { 1697, 319, 345 },  { 2076, 1096, 1121 }, { 1294, 704, 751 },
                   { 135, 508, 558 },    { 1131, 683, 716 },  { 1517, 781, 840 },  { 2929, 394, 416 } };
    std::string wrong;
    for (const auto& s : spawns) {
        const float x = static_cast<float>(s.x), y = static_cast<float>(s.y);
        const MapDot sd = mob_dot(&p, dot_mob(x, y, x, false), now);
        if (!dot_is(sd, s.x, s.floor - kDotStandPx, DotWhy::SetDown))
            wrong += fmt(" %d,%d -> %s;", s.x, s.y, dot_text(sd).c_str());
    }
    check(wrong.empty(), "20 spawn points, 15 to 66 px over their floors: each set down 1 px over its floor --" +
                             (wrong.empty() ? std::string(" all") : wrong));
    d = mob_dot(&p, dot_mob(1032, 758, 1032, false), now);
    check(dot_is(d, 1032, 758, DotWhy::AsIs),
          "1032,758 -- 63 px under floor 695, 287 px over 1045 -- nothing within 100 px below: as it is -- " +
              dot_text(d));

    // The dead: hidden.
    PacketWorld::Mob dead = dot_mob(750, 585, 850, false);
    dead.dead = true;
    dead.dead_ms = now - 200;
    check(mob_dot(&p, dead, now).why == DotWhy::Hidden, "a monster killed on the ramp: not drawn");

    // You. 08:00:34 (L269): from 1342,772 left up the ground to 1295 -- the
    // report there said 1295,750. 08:00:04 (L243): the tab had you at
    // 1420,735, 74 px over floor 809, on the way down from about 1256,735.
    WorldFix f;
    f.state = WorldFix::State::Known;
    f.moving = true;
    f.rx = 1342.0f;
    f.ry = 772.0f;
    d = you_dot(&p, f, 1295, 772);
    check(dot_is(d, 1295, 750, DotWhy::Followed),
          "you walking from 1342,772 to 1295: drawn at 1295,750, where the next report put you -- " + dot_text(d));
    f.rx = 1256.0f;
    f.ry = 735.0f;
    d = you_dot(&p, f, 1420, 735);
    check(dot_is(d, 1420, 808, DotWhy::Followed),
          "you walking down from 1256,735 to 1420: on floor 809, not 74 px over it -- " + dot_text(d));
}

/// The canvas's rules for its dots: which are drawn at all, and which grey.
void test_dots_drawn() {
    section("Map tab, 2026.11.05: the dots drawn -- none with nothing to place them, grey where the hunter has "
            "them and while it walks back first");

    const GameMapBook::Page p = dot_page();
    const std::uint64_t now = 12000;

    // No monster with nothing to place them by: the world's last ones are
    // not the game's now.
    PacketWorld::View v;
    v.mobs.push_back(dot_mob(250, 624, 450, true));     // walking down the ramp: drawn at 724
    v.mobs.push_back(dot_mob(700, 950, 700, false));    // a spawn: set down at 999
    PacketWorld::Mob dead = dot_mob(1100, 399, 1100, false);
    dead.dead = true;
    dead.hp = 0;
    dead.dead_ms = now - 100;
    v.mobs.push_back(dead);
    std::string wrong;
    for (const MapShows s : { MapShows::PositionsOff, MapShows::NoCapture, MapShows::Changing, MapShows::Naming,
                              MapShows::Ambiguous, MapShows::NamedDoubtful, MapShows::NothingYet, MapShows::NoGame,
                              MapShows::NoFile, MapShows::EntryDoubtful, MapShows::NoGates, MapShows::ByEntry,
                              MapShows::ByName }) {
        const bool none = s == MapShows::PositionsOff || s == MapShows::NoCapture;
        const std::vector<MapDot> dots = mob_dots(&p, v, s, now);
        bool ok = map_shows_mobs(s) == !none && dots.size() == (none ? std::size_t{ 0 } : v.mobs.size());
        for (std::size_t i = 0; ok && i < dots.size(); ++i) {
            const MapDot one = mob_dot(&p, v.mobs[i], now);
            ok = dot_is(dots[i], one.x, one.y, one.why);
        }
        if (!ok) wrong += fmt(" %d", static_cast<int>(s));
    }
    check(wrong.empty(), "positions off or no capture: no monster drawn; otherwise each at its own mob_dot, in the "
                         "world's order --" + (wrong.empty() ? std::string(" all 13 states") : wrong));
    std::vector<MapDot> dots = mob_dots(&p, v, MapShows::ByEntry, now);
    check(dots.size() == 3 && dot_is(dots[0], 450, 724, DotWhy::Followed) &&
              dot_is(dots[1], 700, 999, DotWhy::SetDown) && !dots[2].shown(),
          "on the map: the walker on its ramp, the spawn set down, the dead not drawn");
    dots = mob_dots(nullptr, v, MapShows::NothingYet, now);
    check(dots.size() == 3 && dot_is(dots[0], 450, 624, DotWhy::AsIs) && dot_is(dots[1], 700, 950, DotWhy::AsIs) &&
              !dots[2].shown(),
          "no map known, on the grid: where the packets put them, the dead not drawn");

    // An entry's file found off (EntryDoubtful): drawn faded, its floors not
    // gone by -- the dots where the packets put them, as zone_send has it.
    bool floors_kept = true;
    for (const MapShows s : { MapShows::ByEntry, MapShows::NoGates, MapShows::ByName })
        floors_kept = floors_kept && dot_floors(&p, s) == &p;
    check(dot_floors(&p, MapShows::EntryDoubtful) == nullptr && floors_kept &&
              dot_floors(nullptr, MapShows::ByEntry) == nullptr,
          "dot_floors: none on a file found off; the page itself on a map drawn by entry, with no gates, or by name");
    dots = mob_dots(dot_floors(&p, MapShows::EntryDoubtful), v, MapShows::EntryDoubtful, now);
    check(dots.size() == 3 && dot_is(dots[0], 450, 624, DotWhy::AsIs) && dot_is(dots[1], 700, 950, DotWhy::AsIs) &&
              !dots[2].shown(),
          "a file found off: the walker not taken down its ramp to 724, the spawn not set down to 999, the dead not "
          "drawn");
    WorldFix walking;
    walking.state = WorldFix::State::Known;
    walking.moving = true;
    walking.rx = 250.0f;
    walking.ry = 624.0f;
    check(dot_is(you_dot(&p, walking, 450, 624), 450, 724, DotWhy::Followed) &&
              dot_is(you_dot(dot_floors(&p, MapShows::EntryDoubtful), walking, 450, 624), 450, 624, DotWhy::AsIs),
          "you walking down the ramp: on it at 724 by a trusted file; where the packets have you by one found off");

    // Grey: outside the area and out of reach -- or in reach while the
    // hunter walks back first.
    HunterConfig cfg;
    cfg.reach = 450;
    cfg.height_tolerance = 60;
    Hunter::Status hs;
    hs.spot_set = true;
    hs.spot_left = 0.0f;
    hs.spot_right = 1000.0f;
    hs.spot_top = 500.0f;
    hs.spot_bottom = 700.0f;
    const auto grey = [&](const PacketWorld::Mob& m, double yx, double yy, bool you = true, bool area_on = true,
                          bool target = false) { return mob_grey(cfg, hs, area_on, you, yx, yy, m, target); };
    const PacketWorld::Mob inside = dot_mob(500, 600, 500, false);
    const PacketWorld::Mob near_out = dot_mob(1100, 650, 1100, false);
    const PacketWorld::Mob far_out = dot_mob(1500, 650, 1500, false);
    check(!grey(inside, 2500, 999) && grey(far_out, 900, 650) && !grey(near_out, 900, 650),
          "inside the area red, out of reach too; outside it and out of reach grey; outside in reach red -- fought "
          "where it stands");
    check(!grey(far_out, 900, 650, true, false), "no area in use: never grey");
    check(grey(near_out, 900, 650, false), "you not drawn: nothing is in reach -- outside grey");
    PacketWorld::Mob dead_out = far_out;
    dead_out.dead = true;
    dead_out.dead_ms = now;
    check(!grey(dead_out, 900, 650), "the dead: never grey (faded, should they be drawn)");
    hs.spot_back_first = true;
    check(grey(near_out, 900, 650) && grey(far_out, 900, 650) && !grey(inside, 900, 650),
          "walking back first (Status::spot_back_first): outside grey in reach too, as Hunter::mark_out_of_area "
          "lets it be; inside red");
    // The one fought: let go of only Hunter::kSpotKeepPx past the edge.
    const PacketWorld::Mob edge_out = dot_mob(1030, 650, 1030, false);     // 30 px right of the box
    check(Hunter::kSpotKeepPx == 60 && grey(edge_out, 1100, 650) && !grey(edge_out, 1100, 650, true, true, true) &&
              grey(near_out, 1100, 650, true, true, true),
          "walking back first: 30 px outside, grey for any other, red for the target the hunter keeps fighting "
          "within kSpotKeepPx; the target 100 px out grey");
    hs.spot_back_first = false;
    check(!grey(edge_out, 2500, 999, true, true, true) && grey(edge_out, 2500, 999) &&
              grey(near_out, 2500, 999, true, true, true),
          "out of reach: the target 30 px outside red, as the hunter still walks after it; any other grey; the "
          "target 100 px out grey");

    // As the hunter has the plate: packet_dy on the height, packet_scale both ways.
    const PacketWorld::Mob low_out = dot_mob(1100, 710, 1100, false);
    check(!grey(low_out, 900, 650), "60 px below you: level (height_tolerance 60), in reach -- red");
    cfg.packet_dy = 10;
    const bool dy_down = grey(low_out, 900, 650);
    cfg.packet_dy = -10;
    const bool dy_up = grey(low_out, 900, 650);
    cfg.packet_dy = 0;
    cfg.packet_scale = 2.0f;
    const bool scaled = grey(far_out, 900, 650);
    cfg.packet_scale = 1.0f;
    check(dy_down && !dy_up && !scaled,
          "packet_dy 10 puts it 70 px below, out of reach: grey; -10, 50 px: red; packet_scale 2 halves 600 px "
          "across to 300: red");

    // Judged on the world's heights, never the dot's (the owner's Q2 (a)):
    // a walker whose dot has left the box, or come into it, keeps the
    // colour the hunter's own height gives it.
    const PacketWorld::Mob down = dot_mob(250, 624, 450, true);
    const PacketWorld::Mob up = dot_mob(450, 724, 250, true);
    const MapDot down_dot = mob_dot(&p, down, now);
    const MapDot up_dot = mob_dot(&p, up, now);
    check(dot_is(down_dot, 450, 724, DotWhy::Followed) && dot_is(up_dot, 250, 624, DotWhy::Followed) &&
              !grey(down, 2500, 999) && grey(up, 2500, 999),
          "down the ramp, drawn at 724 below the box's 700 but at 624 in the world: red, as the hunter has it; up "
          "the ramp, drawn at 624 inside but at 724 in the world: grey -- " +
              dot_text(down_dot) + ", " + dot_text(up_dot));
}

// ------------------------------------------------- the GUI's own source

#ifdef GOBOT_SOURCE_DIR
std::string gui_source(const std::string& rel) { return read_text(std::string(GOBOT_SOURCE_DIR) + rel); }

/// From `head` to the first line that is "}" alone after it.
std::string body_of(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}

bool names(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
#endif

void test_gui_source() {
    section("Map tab: what simple_map.cpp may do on the GUI thread (its source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string src = gui_source("/apps/gobot_gui/simple_map.cpp");
    if (src.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    // Every file the tab reads or writes, on its worker: the jobs are posted
    // from one function, and nothing else names a read or a write.
    const std::string posts = body_of(src, "void GuiApp::map_post(MapUi& ui, std::uint64_t now) {");
    const std::string stop = body_of(src, "void GuiApp::simple_map_stop() {");
    std::string rest = src;
    for (const std::string* part : { &posts, &stop }) {
        const std::size_t at = rest.find(*part);
        if (!part->empty() && at != std::string::npos) rest.erase(at, part->size());
    }
    check(!posts.empty() && names(posts, "MapViewJob::run(") && names(posts, ".load(path)") &&
              names(posts, "ZoneBook::save_many(") && names(posts, "ui.worker.post("),
          "map_post posts the view job, the book's load and the saves to the worker");
    bool clean = true;
    std::string named;
    for (const char* io : { "game_maps_.get(", ".stems(", "read_page(", "MapFile", "ZoneBook::load", "load(path",
                            "save_many(", "MapViewJob::run(", "set_in_place(", "std::ifstream", "open_file(",
                            "IniFile", ".save(" }) {
        if (!names(rest, io)) continue;
        clean = false;
        named += std::string(" ") + io;
    }
    check(clean, "the rest of simple_map.cpp reads and writes no file:" +
                     (named.empty() ? std::string(" none named") : named));
    // hunt.ini and macro.ini are written whole at each simple_changed(): only
    // the "Chỗ đánh quái" tick does it, never a zone's edit.
    std::size_t saves = 0, ticks = 0;
    for (std::size_t at = src.find("simple_changed("); at != std::string::npos;
         at = src.find("simple_changed(", at + 1)) {
        ++saves;
        const std::size_t line = src.rfind('\n', at);
        const std::size_t end = src.find('\n', at);
        const std::string text = src.substr(line + 1, end - line - 1);
        if (names(text, "ImGui::Checkbox(\"Chỗ đánh quái\", &hunt_config_.spot)")) ++ticks;
    }
    check(saves == 1 && ticks == 1, fmt("simple_changed() once, on the \"Chỗ đánh quái\" tick (%zu, %zu)", saves, ticks));
    check(names(stop, "wait_idle()") && names(stop, "ZoneBook::save_many(") && names(stop, "ui.worker.stop()"),
          "simple_map_stop: the worker waited for, the saves owed written, then stopped");

    // A drag commits on release only: the gesture's frame-by-frame part
    // changes nothing but the draft.
    const std::string drag = body_of(src, "void GuiApp::map_drag(MapUi& ui, const MapView& mv) {");
    check(!drag.empty() && !names(drag, "map_commit_zone") && !names(drag, "set_zone") && !names(drag, "post(") &&
              !names(drag, "save"),
          "the drag's frame-by-frame part: no commit, no hand-off, no job, no save");
    const std::string release = body_of(src, "void GuiApp::map_release(MapUi& ui, const MapView& mv) {");
    check(names(release, "same_place(") && names(release, "zone_check(") && names(release, "map_commit_zone("),
          "the release: nothing for an unchanged zone, checked, then committed");

    // Mouse only, and nothing of the game's: of the game's input, only its
    // window's size is asked (the hunter's monster box under "Chi tiết").
    bool quiet = true;
    for (const char* key : { "send_key", "press(", "keybd_event", "SendInput", "key_down(", "key_up(", "facing_ms",
                             "said_facing", ".facing =", "BeginPopupContextItem", "ImGuiKey_Enter", "ImGuiKey_Escape",
                             "IsKeyPressed", "retake_spot(" })
        quiet = quiet && !names(src, key);
    for (std::size_t at = src.find("input_."); at != std::string::npos; at = src.find("input_.", at + 1))
        quiet = quiet && src.compare(at, 19, "input_.window_size(") == 0;
    check(quiet, "no key sent, pressed or read, no facing written, no context popup that opens on a dragged release, "
                 "no retake: of the game's input only its size");
    check(names(src, "IsMouseDragPastThreshold(ImGuiMouseButton_Right)") && names(src, "OpenPopup(kMenu)") &&
              names(src, "SetItemKeyOwner(ImGuiKey_MouseWheelY)"),
          "the menu only on a right click that did not drag; the wheel the canvas's while over it");
    const std::string menu = body_of(src, "void GuiApp::map_menu(MapUi& ui, const MapView& mv) {");
    check(names(src, "ui.menu_stem = mv.stem;") && names(menu, "if (mv.stem != ui.menu_stem) {") &&
              names(menu, "CloseCurrentPopup()"),
          "the menu closes on another map than it was opened on: its point is not this map's pixels");
    check(names(release, "IsMouseDragPastThreshold(ImGuiMouseButton_Left)") &&
              names(drag, "IsMouseDragPastThreshold(ImGuiMouseButton_Left)"),
          "a left press let go under the drag threshold saves nothing, and draws the zone as it was");
    check(names(menu, "const bool here_ok = can && mv.limits.floors;"),
          "the menu neither makes nor moves a zone with no floors to put its home on");
    const std::string tick = body_of(src, "void GuiApp::simple_map_tick() {");
    const std::string hand = body_of(src, "void GuiApp::map_zone_tick(MapUi& ui, std::uint64_t now) {");
    const std::string look = body_of(src, "void GuiApp::map_look(MapUi& ui, MapView& mv) {");
    check(names(tick, "if (!packets_wanted()) ui.cur.id = KnownMap{};") && names(hand, "packets_wanted() ?") &&
              names(look, "mv.shows == MapShows::NoCapture) mv.u = KnownMap{};") &&
              names(src, "if (!map_ui_ || !packets_wanted()) return false;") &&
              names(src, "if (!map_ui_ || !packets_wanted()) return std::nullopt;"),
          "positions off or the capture stopped by hand: no map known to the tab, the hand-off or Đánh quái");
    const std::string here = body_of(src, "bool GuiApp::map_zone_here() {");
    const std::string card = body_of(src, "void GuiApp::map_card(MapUi& ui, const MapView& mv) {");
    check(names(here, "zone_here_ok(mv.u, mv.page, at)") && names(card, "zone_here_ok(mv.u, mv.page, at)") &&
              names(card, "spot_keep_ok(mv.u)") && names(look, "spot_keep_ok(mv.u)"),
          "a zone put where the character stands, or this run's spot kept: held to the named map's rule");
    check(names(src, "if (!mv.stem.empty() && ui.undo.stem != mv.stem) ui.undo = {};"),
          "Hoàn tác kept for the map shown -- \"Đặt chỗ này\" with the tab shut leaves one");
    check(names(src, "if (ui.mode != MapUi::Mode::Free && !follow) ui.mode = MapUi::Mode::Free;"),
          "a zoom with nobody to follow leaves Follow: else the camera's placing undoes it");
    // 2026.11.05: the grey worked out by mob_grey (map_view.cpp) on the
    // world's own heights, never a dot's; the Chi tiết box round where the
    // packets have you, as the hunter's is.
    const std::string greyed = body_of(gui_source("/src/map_view.cpp"), "bool mob_grey(");
    const std::string canvas = body_of(src, "void GuiApp::map_canvas(MapUi& ui, MapView& mv) {");
    check(names(greyed, "round_px((m.y - you_y) / scale) + cfg.packet_dy") && names(greyed, "m.y >= hs.spot_top") &&
              names(greyed, "if (hs.spot_back_first) return true;") && !names(greyed, "frame_") &&
              !names(greyed, "mob_dot(") && !names(greyed, "along_floor(") &&
              names(canvas, "mob_grey(hunt_config_, hs, area_on, mv.you, mv.you_x, mv.you_y, m, target)") &&
              names(canvas, "const bool fought = mv.hunt_on && hs.has_target && hs.target.slot >= 0;") &&
              names(canvas, "fought && m.slot == static_cast<std::uint32_t>(hs.target.slot) && "
                            "m.gen == hs.target.gen;") &&
              names(greyed, "Hunter::kSpotKeepPx") &&
              names(canvas, "const double cy = mv.you_y - hunt_config_.packet_dy * scale;"),
          "the grey monsters: mob_grey on the world's own m.x, m.y and where the packets have you, packet_dy on the "
          "height, the one fought kept kSpotKeepPx past the edge, as the hunter has it -- never on a dot; the Chi "
          "tiết box round where the packets have you");

    // 2026.11.04d: much of the box red said in red, at every edit; a home by
    // a gate warned of; outside the box never the green "in use".
    check(names(card, "if (c.red_bad) {") && names(card, "coloured(kBad, red_bad_vi(c).c_str());") &&
              names(card, "} else if (c.below_unreachable) {") && names(card, "if (c.home_by_gate) {") &&
              names(card, "case ZoneCard::OutsideArea:") &&
              names(card, "coloured(kWait, spot_outside_vi(hs).c_str());"),
          "the card: much of the box red in red, a little in amber; a home by a gate warned of; outside the box in "
          "amber");
    const std::string tried = body_of(src, "bool GuiApp::map_try(");
    // Only "Tạo vùng quanh nhân vật" is round the character: the right-click's
    // "Tạo vùng ở đây" has its home at the point clicked.
    check(names(tried, "map_red_note(ui, mv.stem, c, around_you);") && names(src, "MapCommit::New, true);") &&
              names(menu, "MapCommit::New);") && !names(menu, "MapCommit::New, true);") &&
              names(release, "map_red_note(ui, stem, c, false);") &&
              names(src, "if (ui.note_bad) coloured(kBad, ui.note.c_str());") &&
              names(src, "made_new && !kZoneAroundHomeLowest ? \"Vùng mới đặt chỗ về ở chỗ nhân vật đứng. \""),
          "every commit that leaves much of the box red says so in red at once -- \"Tạo vùng quanh nhân vật\" with "
          "its home where the character stands (Q5), the right-click's \"Tạo vùng ở đây\" never so");
    const std::string tab = body_of(src, "void GuiApp::simple_map_tab() {");
    check(names(tab, "mv.limits.floors ? 1 : 0, ui.cur.gates.size());") &&
              names(tab, "hunt_config_.drop_down, gates);"),
          "the card's check worked out again when the tab's gates change, with them");

    // 2026.11.05: every monster, the ring on the one fought and the hover at
    // its dot, none of them with nothing to place them by; you at yours.
    check(names(look, "const GameMapBook::Page* floors = dot_floors(mv.page, mv.shows);") &&
              names(look, "mv.dots = mob_dots(floors, mv.v, mv.shows, mv.now);") &&
              names(look, "mv.mobs = map_shows_mobs(mv.shows);") &&
              names(look, "mv.you_at = you_dot(floors, mv.fix, mv.you_x, mv.you_y);") &&
              names(look, "mv.limits = zone_limits(mv.page, mv.u.file_doubtful);") &&
              look.find("mv.page = ui.cur.page.get();") < look.find("dot_floors("),
          "map_look: the dots worked out once a frame, on the floors of the map drawn only, never a file found off; "
          "none while the monsters are not shown");
    check(names(canvas, "if (m.dead != (pass == 0) || !dot.shown()) continue;") &&
              names(canvas, "const ImVec2 c = ts.at(dot.x, dot.y);") &&
              names(canvas, "ts.at(mv.dots[i].x, mv.dots[i].y), 7.0f, MapColors::kTarget") &&
              names(canvas, "tip += \" · \" + place_vi(mv.dots[mob_hover], m.x, m.y);") &&
              !names(src, "ts.at(m.x, m.y)") && !names(src, "kDeadShownMs"),
          "the monsters, the ring on the one fought and the hover at their dots, never at the world's own m.x, m.y; "
          "the dead by kMapDeadShownMs alone, not the hunter's 1.5 s");
    const std::string grid = body_of(src, "Bounds grid_bounds(");
    check(names(canvas, "if (mv.mobs && mv.hunt_on && hs.has_target) {") &&
              names(canvas, "grid_bounds(mv.v, mv.dots, mv.you, mv.you_at.x, mv.you_at.y)") &&
              names(grid, "if (!v.mobs[i].dead && dots[i].shown()) add(dots[i].x, dots[i].y);") &&
              !names(grid, "add(m.x, m.y)"),
          "nothing to place them by: no ring, and no grid's box fitted to the world's last monsters");
    check(names(canvas, "const ImVec2 me = ts.at(mv.you_at.x, mv.you_at.y);") &&
              names(canvas, "ui.cam.cx = mv.you_at.x;") && names(canvas, "ui.cam.cy = mv.you_at.y;") &&
              names(canvas, "std::hypot(mv.fix.rx - mv.you_at.x, mv.fix.ry - mv.you_at.y) > 8.0") &&
              names(canvas, "floor_below(round_px(mv.you_at.x), round_px(mv.you_at.y), 0)") &&
              names(canvas, "ts.at(mv.you_at.x + hs.dx * scale, mv.you_at.y + (hs.dy - hunt_config_.packet_dy) * scale)") &&
              names(tab, "\"Nhân vật: \" + place_vi(mv.you_at, mv.you_x, mv.you_y);") &&
              names(tab, "if (mv.mobs) {"),
          "you at your dot, the camera on it, Chi tiết's fall line and last report and a screen plate's ring from "
          "it; the status line its place, the packets' beside it when elsewhere, and no monster count with none "
          "drawn");
    const std::string spot = gui_source("/apps/gobot_gui/simple_spot.cpp");
    const std::string outside = body_of(spot, "std::string GuiApp::spot_outside_vi(const Hunter::Status& hs) {");
    check(names(outside, "if (!hs.spot_from_map || hs.spot_outside_px <= 0) return {};") &&
              names(outside, "if (hs.spot_back_first)") &&
              names(spot, "coloured(outside.empty() ? kGood : kWait, (line + \".\").c_str());"),
          "simple_spot.cpp: outside the map tab's box, said in amber with the way back; this run's spot as before");
#else
    skip("GOBOT_SOURCE_DIR is not set\n");
#endif
}

/// spots.ini is the player's own: never in the source's configs, which the
/// package copies, and refused by package.ps1 if it is there; and the notes
/// say what the code does about the owner's answers.
void test_package_notes() {
    section("Map tab: configs\\spots.ini never ships; the notes give the take rule and Đặt chỗ này as the code does");
#ifdef GOBOT_SOURCE_DIR
    const std::string pack = gui_source("/package.ps1");
    const std::string doc = gui_source("/DOC-TRUOC.txt");
    const std::string readme = gui_source("/README.md");
    const std::string hunt = gui_source("/configs/hunt.ini");
    if (pack.empty() || doc.empty() || readme.empty() || hunt.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    check(!path_exists(std::string(GOBOT_SOURCE_DIR) + "\\configs\\spots.ini"),
          "no configs\\spots.ini in the source: the package copies its configs from there");
    check(names(pack, "Where-Object { $_.Name -like 'spots.ini*' }") &&
              names(pack, "somebody's hunting areas (Ban do)"),
          "package.ps1 refuses a spots.ini (or its .new) anywhere in the staged package");
    check(names(pack, "Copy spots.ini too, to keep the hunting areas drawn on Ban do"),
          "the package's notes say to copy spots.ini over when updating");
    check(names(doc, "- Mới trong bản 2026.11.04c (") && names(doc, "configs\\spots.ini") &&
              names(doc, "chép thêm\r\n  spots.ini"),
          "DOC-TRUOC.txt: the 2026.11.04c entry, configs\\spots.ini, and copying it in the update notes");
    // The owner's answers as the notes put them: when they change, the notes go too.
    const std::string take = "cách vùng không quá " + std::to_string(kZoneTakePx) + " px";
    check(!kZoneTakeAnywhere && names(doc, take.c_str()),
          "DOC-TRUOC.txt: a saved zone takes over within " + std::to_string(kZoneTakePx) +
              " px of it (Q1), as zone_take has it");
    const std::string within = "within " + std::to_string(kZoneTakePx) + " px of it";
    const std::string vi_within = "vùng không quá " + std::to_string(kZoneTakePx) + " px";
    check(!kZoneTakeAnywhere && names(readme, within.c_str()) && names(pack, within.c_str()) &&
              names(hunt, within.c_str()) && names(hunt, vi_within.c_str()),
          "README.md, START-HERE (package.ps1) and hunt.ini: a saved zone takes over within " +
              std::to_string(kZoneTakePx) + " px of it (Q1), as zone_take has it");
    check(kSpotHereMovesZone && names(doc, "dời vùng đã lưu tới chỗ nhân vật"),
          "DOC-TRUOC.txt: \"Đặt chỗ này\" on a map with a zone moves the zone (Q2), as kSpotHereMovesZone has it");
#else
    skip("GOBOT_SOURCE_DIR is not set\n");
#endif
}

#ifdef GOBOT_SOURCE_DIR
/// The notes' words whatever their lines break them at: every run of spaces
/// and line ends one space -- and, for an .ini, the '#' that starts each
/// comment line gone with it.
std::string flat(const std::string& text, bool ini = false) {
    std::string out;
    out.reserve(text.size());
    bool space = false, line_start = true;
    for (const char c : text) {
        if (c == '\r' || c == '\n') {
            space = line_start = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            space = true;
            continue;
        }
        if (ini && line_start && c == '#') {
            line_start = false;
            space = true;
            continue;
        }
        line_start = false;
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}
#endif

/// 2026.11.04d's notes against the code: 04c's advice -- the box run below
/// the home, which the card paints red -- reversed wherever it was given, the
/// owner's answers (a) as the notes put them with the numbers the code goes
/// by, and each card line and log line they quote the code's own.
void test_notes_04d() {
    section("Map tab: the 2026.11.04d notes -- the home low and the box up, the owner's answers (a) as the code has "
            "them");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(gui_source("/DOC-TRUOC.txt"));
    const std::string readme = flat(gui_source("/README.md"));
    const std::string hunt = flat(gui_source("/configs/hunt.ini"), true);
    const std::string pack = flat(gui_source("/package.ps1"));
    const std::string map_src = gui_source("/apps/gobot_gui/simple_map.cpp");
    const std::string spot_src = gui_source("/apps/gobot_gui/simple_spot.cpp");
    const std::string hunter_src = gui_source("/src/hunter.cpp") + gui_source("/src/hunter_drop.cpp");
    const std::string more_src = gui_source("/src/cast_match.cpp") + gui_source("/src/bag.cpp") +
                                 gui_source("/src/input_manager.cpp") + gui_source("/apps/gobot_gui/gui_app.cpp") +
                                 gui_source("/apps/gobot_gui/packets_bag.cpp") +
                                 gui_source("/apps/gobot_gui/simple_potions.cpp");
    if (doc.empty() || readme.empty() || hunt.empty() || pack.empty() || map_src.empty() || spot_src.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto has = [](const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    };
    const auto n = [](int v) { return std::to_string(v); };

    const std::size_t d04 = doc.find("- Mới trong bản 2026.11.04d (");
    const std::size_t c04 = doc.find("- Mới trong bản 2026.11.04c (");
    check(d04 != std::string::npos && c04 != std::string::npos && d04 < c04 &&
              has(doc, "2026.11.04d (sửa theo lần thử tab Bản đồ trên game thật tối 04/11; chưa thử trên game thật)"),
          "DOC-TRUOC.txt: the 2026.11.04d entry, on top of 04c's, not yet tried on the game");

    // 04c's advice, reversed wherever it was given.
    check(!has(doc, "có thể kéo vùng xuống thấp hơn chỗ về nhiều") &&
              !has(doc, "kéo cạnh dưới của vùng xuống thấp hơn bậc dưới") &&
              !has(doc, "Kéo cạnh dưới của vùng xuống thấp hơn bục bên dưới") &&
              !has(doc, "có cạnh dưới thấp hơn bậc dưới") &&
              has(doc, "đặt chỗ về (ghim cam) ở bục THẤP NHẤT bạn muốn đánh, rồi kéo vùng LÊN TRÊN") &&
              has(doc, "đặt chỗ về ở bục thấp nhất bạn muốn đánh, rồi kéo vùng lên trên. (Sửa ở bản 2026.11.04d") &&
              has(doc, "Đừng bấm \"Tạo vùng quanh nhân vật\" ngay khi vừa qua cửa"),
          "DOC-TRUOC.txt: the box run below the home advised nowhere -- the home on the lowest floor and the box up, "
          "04c's entry put right in place, no zone made right after a door");
    check(!has(readme, "a box running well below it lets drops land inside") &&
              !has(readme, "with its bottom edge below the floor beneath lets the drop go") &&
              has(readme, "put the home on the lowest floor to be hunted and run the box up over the floors above it") &&
              has(pack, "Put the home on the LOWEST floor to be hunted and drag the box UP, not down"),
          "README.md and START-HERE (package.ps1): the same");

    // The owner's answers (a), each with the number the code goes by.
    check(kZoneWalkStaysIn && kZoneFallInsideOk && has(doc, "(tới " + n(kZoneWalkAheadPx) + " px phía trước)") &&
              has(doc, "Rơi xuống một bục vẫn ở trong vùng thì vẫn đi") &&
              has(doc, "bỏ con quái đó " + n(static_cast<int>(2 * HunterConfig{}.give_up_ms / 1000)) +
                           " giây (gấp đôi give_up_ms)") &&
              has(readme, "`Hunter::kZoneWalkSlackPx` (" + n(Hunter::kZoneWalkSlackPx) + ") more, " +
                              n(kZoneWalkAheadPx) + " px at most"),
          "Q1 (a): no walk to a monster out of the box, a fall into it walked; looked 96 px ahead at most, the target "
          "passed over twice give_up_ms");
    check(has(doc, "Ở ngoài vùng quá " + n(kZoneOutsideBackPx) +
                       " px mà có đường về như vậy: bot thôi đánh quái ở ngoài vùng, kể cả quái trong tầm") &&
              has(readme, "`kZoneOutsideBackPx` (" + n(kZoneOutsideBackPx) + ") outside the box") &&
              has(hunt, "more than " + n(kZoneOutsideBackPx) + " px outside it with a way back the file shows") &&
              has(hunt, "ở ngoài vùng quá " + n(kZoneOutsideBackPx) + " px mà có đường đi bộ về") &&
              has(pack, "more than " + n(kZoneOutsideBackPx) + " px outside it"),
          "Q2 (a): more than kZoneOutsideBackPx outside with a way back, the walk back first -- the notes, README, "
          "hunt.ini and START-HERE");
    check(has(doc, "qua cả các lỗ nhỏ 1-" + n(GameMapBook::kFollowHolePx) +
                       " px của bục), hoặc đi ra mép bục để rơi xuống một bục trong vùng (tối đa " +
                       n(kZoneReturnFallsMax) + " lần rơi") &&
              has(doc, "Đi về như vậy KHÔNG bao giờ bấm phím nhảy") &&
              has(readme, "`kZoneReturnFallsMax` (" + n(kZoneReturnFallsMax) + ") falls") &&
              has(doc, "spot_tolerance (" + n(HunterConfig{}.spot_tolerance) +
                           " px) theo chiều ngang VÀ đứng trên đúng bục của chỗ về") &&
              has(readme, "off every floor, within " + n(Hunter::kSpotFloorPx) + " px up or down"),
          "the way back: holes of kFollowHolePx walked over, kZoneReturnFallsMax falls at most, no hop; home by "
          "height as well as across");
    HunterConfig spot_on;
    spot_on.spot = true;
    spot_on.jump_to_reach = true;
    spot_on.jump_key = VK_UP;
    check(Hunter::kSkipAboveUnreached && !Hunter::hops_to_targets(spot_on) &&
              has(doc, "bot không chọn con quái cao hơn số \"Cao nhất\" (trong \"Tầm đánh…\") mà không đứng trên "
                       "chính bục nhân vật đang đứng") &&
              has(doc, "thì bot vẫn không nhảy lên bục trên để đánh quái (câu hỏi 4), như trước") &&
              has(readme, "`Hunter::kSkipAboveUnreached`") &&
              has(hunt, "a monster above that does not stand on the character's own floor is not picked"),
          "Q3 (a) and Q4 (a): monsters above off the character's own floor skipped with no hop to follow; still no "
          "hop toward a monster with the spot on");
    check(!kZoneAroundHomeLowest && kZoneRedBadPercent == 50 && zone_red_is_bad(51, 100) &&
              !zone_red_is_bad(50, 100) &&
              has(doc, "\"Tạo vùng quanh nhân vật\" vẫn đặt chỗ về ở chỗ nhân vật đứng (câu hỏi 5)") &&
              has(doc, "Vùng có hơn nửa số bục tô đỏ") && has(readme, "`kZoneAroundHomeLowest`") &&
              has(doc, "Chỗ về cách một cửa không quá " + n(kZoneHomeGatePx) + " px") &&
              has(readme, "within " + n(kZoneHomeGatePx) + " px of a gate"),
          "Q5 (a): the home where the character stands, more than half the box red said in red; a home by a gate "
          "within kZoneHomeGatePx warned of");
    check(has(doc, "khi con quái ở dưới cách không quá " + n(Hunter::kDropStopAbovePx) + " px theo chiều ngang") &&
              has(doc, "ít nhất " + n(Hunter::kDropMarginPressPx) + " px mỗi bên") &&
              has(doc, "bục đầu tiên dưới nó trong vòng " + n(Hunter::kDropMonsterFloorPx) + " px") &&
              has(readme, "within " + n(Hunter::kDropStopAbovePx) + " px across of it") &&
              has(readme, "within " + n(Hunter::kDropMonsterFloorPx) + " px (`Hunter::kDropMonsterFloorPx`)") &&
              has(hunt, "stops within " + n(Hunter::kDropStopAbovePx) + " px across of the target below") &&
              has(hunt, "không quá " + n(Hunter::kDropStopAbovePx) + " px theo chiều ngang rồi mới xét") &&
              has(pack, "within " + n(Hunter::kDropStopAbovePx) + " px across of the monster below"),
          "Q6 (a): the stop within kDropStopAbovePx across above a target below, judged then at kDropMarginPressPx; "
          "the monster's floor within kDropMonsterFloorPx under it");

    // What the notes quote -- the card's lines, the alert, the log's words --
    // is the code's own.
    check(has(map_src, "bot không tự đi về được (tô đỏ): rời bục chỗ về là bot ở lại dưới đó.") &&
              has(map_src, "\"có quái, rồi kéo vùng lên trên.\"") &&
              has(doc, "\"Phần lớn bục trong vùng (N%) bot không tự đi về được (tô đỏ):") &&
              has(doc, "Kéo ghim cam xuống bục thấp nhất có quái, rồi kéo vùng lên trên.\"") &&
              has(map_src, " px): thường là chỗ vào bản đồ, ít quái\"") &&
              has(map_src, "\". Kéo ghim cam ra chỗ có quái.\"") &&
              has(doc, "px): thường là chỗ vào bản đồ, ít quái. Kéo ghim cam ra chỗ có quái.\""),
          "the map card's red and gate lines as simple_map.cpp says them");
    check(has(spot_src, "\"Nhân vật đang ở ngoài vùng \"") && has(spot_src, "\" trước, rồi mới đánh tiếp.\"") &&
              has(spot_src, "\" — bot vẫn đánh quái trong tầm; hết quái thì đi bộ về \"") &&
              has(spot_src, "rơi xuống nào về vùng: bot đánh ở đây. Tự đưa nhân vật về \"") &&
              has(doc, "\"Nhân vật đang ở ngoài vùng N px — bot đi bộ về X, Y trước, rồi mới đánh tiếp.\"") &&
              has(doc, "bot vẫn đánh quái trong tầm; hết quái thì đi bộ về X, Y.\"") &&
              has(doc, "rơi xuống nào về vùng: bot đánh ở đây. Tự đưa nhân vật về vùng, hoặc bấm Đặt chỗ này.\"") &&
              has(spot_src, "\", outside the area \"") && has(spot_src, "\" (walking back first, to \"") &&
              has(doc, "\", outside the area N px\" (và \"walking back first, to X,Y\""),
          "the outside and no-way-back lines, and the Positions line's words, as simple_spot.cpp says them");
    const std::string low = alert_text::potion_low(true, "1", 6);
    const std::string low_bag = alert_text::potion_low(false, "H", 5, 205);
    check(has(doc, "(tin \"" + low + "\")") && has(doc, "\"" + alert_text::potion_low(true, "1", 5) + "\"") &&
              has(low_bag, "; cả túi còn 205 bình (theo gói tin, chưa kiểm).") &&
              has(doc, "tin thêm \"; cả túi còn N bình (theo gói tin, chưa kiểm)\""),
          "the potion alert's words: \"" + low + "\"");
    check(has(hunter_src, "\"Hunter: not walking %s toward") &&
              has(hunter_src, "walks stopped at the hunting area's edge %d") &&
              has(hunter_src, "\"Hunter: no way back into the hunting area from") &&
              has(hunter_src, "\"; drop looks %d (settles %d\"") && has(hunter_src, "\"; refused: \"") &&
              has(doc, "\"Hunter: not walking ...\"") && has(doc, "\"walks stopped at the hunting area's edge N\"") &&
              has(doc, "\"Hunter: no way back into the hunting area ...\"") &&
              has(doc, "\"drop looks N (settles M; refused: ...)\"") &&
              has(readme, "`walks stopped at the hunting area's edge N`") &&
              has(readme, "`; drop looks N (settles N; refused: …)`"),
          "the hunter's new log words, as hunter.cpp and hunter_drop.cpp write them");
    check(has(more_src, "under the labels %d %d %d %d %d") && has(more_src, "\" in the bag\"") &&
              has(more_src, "\"Buff (packets): \"") && has(more_src, ": maybe not cast\"") &&
              has(more_src, "\"; before it: \"") && has(more_src, "\"typed text\"") &&
              has(more_src, "\" unreadable on two looks running\"") &&
              has(more_src, "kept a picture of the slot alone as potion_debug") &&
              has(map_src, "floor columns red -- ") && has(doc, "\"under the labels ...\"") &&
              has(doc, "260 in the bag)") && has(doc, "\"Buff (packets): key ... -- ...\"") &&
              has(doc, "(\"maybe not cast\" là không thấy)") && has(doc, "\"typed text\"") &&
              has(doc, "has N of M floor columns red\"") && has(readme, "`; before it: …`") &&
              has(readme, "kept a picture of the slot alone as …`"),
          "the bag's, the casts', the map entry's, the potions' and the map tab's new log words, as their code "
          "writes them");
#else
    skip("GOBOT_SOURCE_DIR is not set\n");
#endif
}

/// 2026.11.05's notes against the code: the dots on their floors in the
/// drawing alone, the owner's three answers (a) with the numbers the code goes
/// by, the far frames the hunter sees too, and the words and steps they quote
/// the code's own.
void test_notes_05() {
    section("Map tab: the 2026.11.05 notes -- the dots on their floors, the owner's answers (a) as the code has them");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(gui_source("/DOC-TRUOC.txt"));
    const std::string readme = flat(gui_source("/README.md"));
    const std::string pack = flat(gui_source("/package.ps1"));
    const std::string map_src = gui_source("/apps/gobot_gui/simple_map.cpp");
    const std::string gui_src = gui_source("/apps/gobot_gui/gui_app.cpp");
    const std::string world_src = gui_source("/src/packet_world.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || map_src.empty() || gui_src.empty() || world_src.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto has = [](const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    };
    const auto n = [](long long v) { return std::to_string(v); };
    // 1500 ms as the notes say it: "1,5".
    const auto vi_s = [](std::uint64_t ms) {
        std::string s = std::to_string(ms / 1000);
        if (ms % 1000 != 0) s += "," + std::to_string(ms % 1000 / 100);
        return s;
    };

    const std::size_t e05 = doc.find("- Mới trong bản 2026.11.05 (");
    const std::size_t d04 = doc.find("- Mới trong bản 2026.11.04d (");
    check(e05 != std::string::npos && d04 != std::string::npos && e05 < d04 &&
              has(doc, "2026.11.05 (sửa cách tab Bản đồ vẽ quái, theo điều bạn báo ngày 05/11; chưa thử trên game "
                       "thật)") &&
              has(doc, "\"Lệch chỗ\"") && has(doc, "\"Thừa chấm\""),
          "DOC-TRUOC.txt: the 2026.11.05 entry, on top of 04d's, the owner's two symptoms, not yet tried on the game");

    // The owner's answers (a), each with the number the code goes by.
    check(kMapDeadShownMs == 0u && PacketEyes::kDeadShownMs == 1500u &&
              has(doc, "còn được vẽ mờ " + vi_s(PacketEyes::kDeadShownMs) + " giây ở chỗ bị đánh chết") &&
              has(doc, "Nay chấm của nó mất ngay, như GoC (câu hỏi 1)") &&
              has(doc, "1. Quái chết: (a) chấm mất ngay, như GoC; (b) để mờ " + vi_s(PacketEyes::kDeadShownMs) +
                           " giây ở chỗ bị đánh chết, như trước.") &&
              has(readme, "(`kMapDeadShownMs`, " + n(static_cast<long long>(kMapDeadShownMs)) +
                              ", the owner's Q1 (a)") &&
              has(pack, "a dead monster's dot goes at once"),
          "Q1 (a): the dead hidden at once on the map (kMapDeadShownMs 0); (b) the 1.5 s of PacketEyes::kDeadShownMs");
    check(has(doc, "bot vẫn xét độ cao của quái và của nhân vật như bản 04d (câu hỏi 2)") &&
              has(doc, "2. Độ cao theo bục: (a) chỉ tab Bản đồ vẽ theo bục, bot vẫn xét độ cao như trước") &&
              has(readme, "the owner's Q2 (a): the hunter keeps its heights until answered") &&
              has(pack, "(the bot itself still judges heights as before)") &&
              has(doc, "(câu 1 và câu 3 mỗi câu chỉ là một chỗ nhỏ để đổi; câu 2 (b) là một thay đổi riêng ở phần "
                       "bot đánh quái, phải làm và thử riêng):"),
          "Q2 (a): only the map's drawing follows the floors; the hunter's heights as before -- (b) not a one-place "
          "change, as Q1 and Q3 are");
    check(kMapTabFps == 10 && kMapTabLightFps == 2 &&
              has(doc, "GoBot vẽ lại " + n(kMapTabFps) + " lần mỗi giây (trước đây 5), cả lúc game ở trước") &&
              has(doc, "có dấu \"Nhẹ máy\" thì vẫn " + n(kMapTabLightFps) + " lần") &&
              has(doc, "3. Vẽ lại khi tab mở: (a) " + n(kMapTabFps) + " lần mỗi giây") &&
              has(doc, "(Từ bản 2026.11.05: " + n(kMapTabFps) + " lần mỗi giây;") &&
              has(readme, "`kMapTabFps` (" + n(kMapTabFps) + ") times a second behind the game and in front alike") &&
              has(readme, "`kMapTabLightFps` (" + n(kMapTabLightFps) + ") behind the game in light mode") &&
              has(readme, "shown, " + n(kMapTabFps) + " behind the game and in front alike (" + n(kMapTabLightFps) +
                              " behind it in light mode") &&
              !has(readme, "5 behind the game too") && !has(readme, "drawn 5 times a second behind the game") &&
              has(pack, "the tab is redrawn " + n(kMapTabFps) + " times a second (" + n(kMapTabLightFps) +
                            " behind the game with Nhe may)"),
          "Q3 (a): the tab redrawn kMapTabFps times a second, kMapTabLightFps in light mode -- the notes, README and "
          "START-HERE, the old 5 gone");

    // The dots, with the numbers the drawing goes by.
    check(has(doc, "qua cả lỗ nhỏ 1-" + n(GameMapBook::kFollowHolePx) + " px của bục) tới chỗ ngang GoBot tính") &&
              has(doc, "Bục hết trước chỗ đó thì chấm đứng ở đầu bục") &&
              has(doc, "bục đầu tiên ở dưới nó trong vòng " + n(Hunter::kDropMonsterFloorPx) +
                           " px (bục mà bot cũng coi là của nó), cao hơn mặt bục " + n(kDotStandPx) + " px") &&
              has(readme, "`kDotFollowMaxPx` (" + n(kDotFollowMaxPx) + ")") &&
              has(readme, "`kDotStandPx` (" + n(kDotStandPx) + ")") &&
              has(readme, "the first at or below it within " + n(Hunter::kDropMonsterFloorPx) + " px") &&
              has(readme, "across holes of 1-" + n(GameMapBook::kFollowHolePx) +
                              " px (`GameMapBook::Page::follow`) to its x") &&
              has(readme, "or on an entry's file found off (drawn faded), whose floors are not gone by "
                          "(`dot_floors`)") &&
              has(doc, "Khi file bản đồ có thể không khớp chỗ nhân vật đứng (bục vẽ mờ), mọi chấm, cả chấm xanh, vẽ "
                       "như cũ"),
          "the walker followed across holes of kFollowHolePx and held at its floor's end, a spawn set down "
          "kDotStandPx over the floor within kDropMonsterFloorPx, as mob_dot has them; none on a file found off");
    check(!map_shows_mobs(MapShows::NoCapture) && !map_shows_mobs(MapShows::PositionsOff) &&
              has(map_src, "\"Chưa có gói tin của game\"") && has(map_src, "\"Vị trí từ gói tin đang tắt\"") &&
              has(doc, "lúc giữa bản đồ ghi \"Chưa có gói tin của game\" hay \"Vị trí từ gói tin đang tắt\"") &&
              has(readme, "(`map_shows_mobs`) no monster, ring or monster hover is drawn"),
          "no monster drawn without packets, under the tab's own words for it");
    check(has(map_src, "trong lúc bot đi bộ về vùng trước, rồi mới đánh tiếp — bot không đi tới.") &&
              has(doc, "nay xám cả con quái ở ngoài vùng mà ở trong tầm đánh, trong lúc bot đi bộ về vùng trước, "
                       "rồi mới đánh tiếp (bot bỏ qua") &&
              has(doc, "chỉ xám khi đã ra ngoài vùng quá " + n(Hunter::kSpotKeepPx) + " px") &&
              has(readme, "`Hunter::kSpotKeepPx` (" + n(Hunter::kSpotKeepPx) + ") past the box's edge") &&
              has(readme, "outside it in reach too while the hunter walks back first") &&
              has(map_src, "s += \" (gói tin \" + px(x) + \", \" + px(y) + \")\";") &&
              has(doc, "thêm \"(gói tin x, y)\" khi chỗ GoBot tính theo gói tin khác chỗ đó") &&
              has(readme, "`(gói tin x, y)`"),
          "the grey while walking back first and the hover's packet place, as simple_map.cpp says them");

    // The far frames: the one change the hunter sees.
    check(PacketWorld::kPlaceFarFrames && PacketWorld::kUnknownPlacePx == 200.0f &&
              has(doc, "quái xa hơn " + n(static_cast<long long>(PacketWorld::kUnknownPlacePx)) +
                           " px so với chỗ đã tính") &&
              has(doc, "các gói tin khác xa quá " + n(static_cast<long long>(PacketWorld::kUnknownPlacePx)) +
                           " px vẫn bỏ qua như trước") &&
              has(doc, "Đây là thay đổi duy nhất mà bot cũng thấy") &&
              has(doc, "trước gói tin đó, chỗ tính vẫn có thể trượt xa như trước.") &&
              !has(doc, "không còn bị tính ở xa hơn 1000 px") &&
              has(readme, "`PacketWorld::kUnknownPlacePx` (" +
                              n(static_cast<long long>(PacketWorld::kUnknownPlacePx)) + ")") &&
              has(readme, "(`PacketWorld::kPlaceFarFrames`, the one rule `places_far`; false turns it back)") &&
              has(readme, "This is the one change of 2026.11.05 the hunter sees."),
          "the far frames placed: a chained hurt and a contact, the rest held to kUnknownPlacePx, the hunter's too");
    const long long walk = static_cast<long long>(PacketWorld::kPlausibleWalkSpeed);
    check(has(world_src, "\"Positions: a far monster frame %s -- state %u (%s), slot %u gen %u at %u,%u") &&
              has(world_src, "%s, %.0f px off; its last placing frame: %s at speed %.2f") &&
              has(gui_src, "line << \", far frames placed \" << ws.far_corrections << \" refused \" << ws.far_refused "
                           "<< \", walks faster than \"") &&
              has(doc, "\"Positions: a far monster frame placed ...\" (hay \"refused ...\"; tối đa một dòng mỗi " +
                           n(static_cast<long long>(PacketWorld::kFarSaidEveryMs / 1000)) + " giây)") &&
              has(doc, "thêm \", far frames placed N refused N, walks faster than " + n(walk) + " px/step N\"") &&
              has(readme, "at most once every " + n(static_cast<long long>(PacketWorld::kFarSaidEveryMs / 1000)) +
                              " s (`kFarSaidEveryMs`)") &&
              has(readme, "`far frames placed N refused N, walks faster than " + n(walk) + " px/step N`") &&
              has(readme, "the far line's `so far` counts run from the packet world's last fresh start") &&
              has(doc, "Số ở dòng \"Positions\" tính từ lúc bấm BẮT ĐẦU; số \"so far\" ở cuối dòng"),
          "the far line and the Positions line's new counts, as packet_world.cpp and gui_app.cpp write them, and "
          "from when each counts");

    // The live test: every button it names is GoBot's own.
    check(has(doc, "Thử như bước 9 g ở mục 6.") &&
              has(doc, "g. Từ bản 2026.11.05 (chấm của quái), chưa cần bấm BẮT ĐẦU") &&
              has(doc, "bấm \"Nâng cao ►\", thẻ Capture, bấm \"Stop capture\", rồi bấm \"◄ Giao diện đơn giản\"") &&
              has(doc, "giữa bản đồ phải ghi \"Chưa có gói tin của game\"") &&
              has(map_src, "mode_button(\"Theo nhân vật\"") && has(gui_src, "ImGui::BeginTabItem(\"Capture\")") &&
              has(gui_src, "ImGui::Button(\"Stop capture\"") && has(gui_src, "MenuItem(\"◄ Giao diện đơn giản\")") &&
              has(gui_source("/apps/gobot_gui/simple_view.cpp"), "\"Nâng cao ►\""),
          "the live test's step 9 g: Theo nhân vật, Nâng cao ►, Capture, Stop capture and back, as the GUI names them");
#else
    skip("GOBOT_SOURCE_DIR is not set\n");
#endif
}

/// 2026.11.05b's notes against the code: "Nhảy lên bục trên" and "Tự tìm quái
/// trong vùng" -- the owner's answers to the one and the seek's questions (a)
/// with the numbers the code goes by, the old promises of no jump up put right
/// where they were made, t52_s2's take-offs as the planner finds them on the
/// map's own file, and every word and log line they quote the code's own.
void test_notes_05b() {
    section("Map tab: the 2026.11.05b notes -- the jump up and the seek, the owner's answers as the code has them");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(gui_source("/DOC-TRUOC.txt"));
    const std::string readme = flat(gui_source("/README.md"));
    const std::string pack = flat(gui_source("/package.ps1"));
    const std::string hunt = flat(gui_source("/configs/hunt.ini"), true);
    const std::string proto = flat(gui_source("/docs/protocol-findings.md"));
    const std::string goc = flat(gui_source("/docs/goc-reference.md"));
    const std::string style_src = gui_source("/apps/gobot_gui/simple_style.cpp");
    const std::string view_src = gui_source("/apps/gobot_gui/simple_view.cpp");
    const std::string spot_src = gui_source("/apps/gobot_gui/simple_spot.cpp");
    const std::string map_src = gui_source("/apps/gobot_gui/simple_map.cpp");
    const std::string gui_src = gui_source("/apps/gobot_gui/gui_app.cpp");
    const std::string jump_src = gui_source("/src/hunter_jump.cpp");
    const std::string seek_src = gui_source("/src/hunter_seek.cpp");
    const std::string hunter_src = gui_source("/src/hunter.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || hunt.empty() || proto.empty() || goc.empty() ||
        style_src.empty() || view_src.empty() || spot_src.empty() || map_src.empty() || gui_src.empty() ||
        jump_src.empty() || seek_src.empty() || hunter_src.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto has = [](const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    };
    const auto n = [](auto v) { return std::to_string(v); };
    // 1500 ms as the notes say it, "1,5"; and as README does, "1.5".
    const auto vi_s = [](std::uint64_t ms) {
        std::string s = std::to_string(ms / 1000);
        if (ms % 1000 != 0) s += "," + std::to_string(ms % 1000 / 100);
        return s;
    };
    const auto en_s = [](std::uint64_t ms) {
        std::string s = std::to_string(ms / 1000);
        if (ms % 1000 != 0) s += "." + std::to_string(ms % 1000 / 100);
        return s;
    };
    const std::size_t npos = std::string::npos;

    const std::size_t e05b = doc.find("- Mới trong bản 2026.11.05b (");
    const std::size_t e05 = doc.find("- Mới trong bản 2026.11.05 (");
    const std::size_t sec6 = doc.find("6. THỬ TỪNG PHẦN CHO AN TOÀN");
    const std::size_t step10 = doc.find("10. Nhảy lên bục trên và Tự tìm quái trong vùng (bản 2026.11.05b");
    check(e05b != npos && e05 != npos && e05b < e05 && sec6 != npos && step10 != npos && step10 > sec6 &&
              has(doc, "2026.11.05b (nhảy lên bục trên, và tự tìm quái trong vùng đánh quái theo điều bạn báo tối "
                       "05/11; chưa thử trên game thật)") &&
              has(doc, "Thử như bước 10 ở mục 6.") &&
              has(readme, "Since 2026.11.05b (not yet run on the game) an area in use has two more switches") &&
              has(pack, "Since 2026.11.05b (not yet tried on the game) an area has two more ticks"),
          "DOC-TRUOC.txt: the 2026.11.05b entry on top of 05's, not yet tried on the game, with its live test as "
          "step 10 of section 6; README and START-HERE (package.ps1) say it too");

    // Both on by default -- the owner's answer 5, the seek's Q7 (a) -- and a
    // hunt.ini without them reads them on; the ticks named as the GUI has them.
    HunterConfig old_file;
    old_file.jump_up = false;
    old_file.seek_in_zone = false;
    const std::string ini = temp_path("hunt_05b_notes.ini");
    const bool loaded = write_text(ini, "[Hunt]\nreach=150\n") && old_file.load(ini);
    check(HunterConfig{}.jump_up && HunterConfig{}.seek_in_zone && loaded && old_file.jump_up &&
              old_file.seek_in_zone && has(style_src, "\"Nhảy lên bục trên (↑ / ↑↑ / Ctrl+↑)\"") &&
              has(style_src, "\"Tự tìm quái trong vùng\"") &&
              has(gui_src, "\"Jump up onto floors above (Up / Up Up / Ctrl+Up)\"") &&
              has(gui_src, "\"Seek monsters anywhere in the area\"") &&
              has(doc, "cả hai BẬT SẴN: \"Nhảy lên bục trên (↑ / ↑↑ / Ctrl+↑)\", sau \"Nhảy xuống bậc dưới\", và "
                       "\"Tự tìm quái trong vùng\", sau \"Chỗ đánh quái\"") &&
              has(doc, "\"Jump up onto floors above (Up / Up Up / Ctrl+Up)\" và \"Seek monsters anywhere in the "
                       "area\"") &&
              has(doc, "hunt.ini không có dòng jump_up hay seek_in_zone thì GoBot coi như ô đó có dấu") &&
              has(readme, "a `hunt.ini` without `jump_up` or `seek_in_zone` reads true") &&
              has(readme, "*Jump up onto floors above (Up / Up Up / Ctrl+Up)*") &&
              has(readme, "*Seek monsters anywhere in the area*") &&
              has(readme, "the jump up onto a floor above (`jump_up`) and the seek inside a **Bản đồ** area "
                          "(`seek_in_zone`), both on as shipped") &&
              has(pack, "both on as shipped (a hunt.ini without them reads them on)"),
          "both ticks on by default and on for a hunt.ini without them; their names as the GUI has them");

    // The heights: the model's peaks and sure landings, the owner's "Theo GoC"
    // within them (answer 1), the second Up's time.
    const JumpPeak up = jump_peak(JumpKind::Up), uu = jump_peak(JumpKind::UpUp), cu = jump_peak(JumpKind::CtrlUp);
    const auto within = [](const JumpPeak& p, int v) { return p.lo <= v && v <= p.hi; };
    check(within(up, 133) && within(uu, 280) && within(cu, 432) &&
              has(doc, "\"Theo GoC\": ↑ khoảng 133 px, ↑↑ khoảng 280 px, Ctrl+↑ khoảng 432 px (câu 1)") &&
              has(doc, "↑ lên cao " + n(up.lo) + "–" + n(up.hi) + " px, ↑↑ " + n(uu.lo) + "–" + n(uu.hi) +
                           " px, Ctrl+↑ " + n(cu.lo) + "–" + n(cu.hi) + " px; số của GoC nằm trong các khoảng đó") &&
              has(doc, "không quá " + n(up.usable()) + " px (↑), " + n(uu.usable()) + " px (↑↑) hay " +
                           n(cu.usable()) + " px (Ctrl+↑)") &&
              has(doc, "(↑ thứ hai bấm " + n(kJumpUpSecondAtMs) + " ms sau ↑ đầu)") &&
              has(readme, "peaks of " + n(up.lo) + ".." + n(up.hi) + ", " + n(uu.lo) + ".." + n(uu.hi) + " and " +
                              n(cu.lo) + ".." + n(cu.hi) + " px (`jump_peak`), so a floor up to " +
                              n(up.usable()) + ", " + n(uu.usable()) + " and " + n(cu.usable()) + " px above") &&
              has(readme, "`kJumpUpSecondAtMs` (" + n(kJumpUpSecondAtMs) + ")") &&
              has(readme, "`kJumpUpClearPx` (" + n(kJumpUpClearPx) + ")"),
          fmt("the heights: peaks %d..%d, %d..%d, %d..%d, sure to %d, %d and %d px, GoC's 133/280/432 within them, "
              "the second Up %d ms on -- as jump_plan.hpp has them",
              up.lo, up.hi, uu.lo, uu.hi, cu.lo, cu.hi, up.usable(), uu.usable(), cu.usable(), kJumpUpSecondAtMs));

    // Never out of the box: the margin, the 650 px ceiling, the weaker jump;
    // no gate band (answer 2) and the 10 s map-change watch.
    check(!kJumpUpGateBand && has(doc, "chỗ đó và " + n(kJumpUpMarginPx) + " px mỗi bên") &&
              has(doc, "không có bục nào NGOÀI vùng trong khoảng " + n(kJumpUpCeilingPx) + " px phía trên") &&
              has(doc, "cú ↑ thường còn lại vẫn rơi về bục cũ hay một bục trong vùng") &&
              has(doc, "(lệch thì nhích tối đa " + n(Hunter::kJumpUpFineTapsMax) + " lần)") &&
              has(doc, "nên GoBot không tránh cửa khi nhảy lên") &&
              has(doc, "nếu bản đồ đổi trong " + n(Hunter::kPressWatchMs / 1000) +
                           " giây sau bất kỳ lần nhảy lên nào") &&
              has(readme, "`kJumpUpCeilingPx` (" + n(kJumpUpCeilingPx) + ")") &&
              has(readme, "`kJumpUpMarginPx` (" + n(kJumpUpMarginPx) + ") either way") &&
              has(readme, "(`kJumpUpGateBand` false)") &&
              has(readme, "`kPressWatchMs` (" + n(Hunter::kPressWatchMs / 1000) + " s)") &&
              has(pack, "within " + n(kJumpUpCeilingPx) + " px above"),
          "never out of the box: the margin either way, the ceiling guard, the weaker jump; no band round a gate "
          "(kJumpUpGateBand false), the map-change watch -- as the code has them");

    // The limits and the latches, each with the number the hunter goes by.
    const HunterConfig def;
    check(has(doc, "tối đa " + n(Hunter::kJumpUpsPerMinute) + " lần nhảy lên mỗi phút, " +
                       n(Hunter::kJumpUpsPerTarget) + " lần cho một con quái (và " +
                       n(Hunter::kJumpUpSettlesPerTarget) + " lần đứng lại chờ nhảy mà không bấm)") &&
              has(doc, "cách nhau ít nhất " + vi_s(Hunter::kDropGapMs) + " giây") &&
              has(doc, "không nhảy trong " + n(Hunter::kDropAfterEntryMs / 1000) + " giây đầu sau khi vào bản đồ") &&
              has(doc, "thì " + n(Hunter::kJumpUpStayMs / 1000) + " giây không nhảy xuống") &&
              has(doc, "thì " + n(Hunter::kDropStayMs / 1000) + " giây không nhảy lên tới quái") &&
              has(doc, n(Hunter::kJumpUpFailsInRow) + " lần liền (hay " + n(Hunter::kJumpUpFailsPerRun) +
                           " lần trong một lượt) không lên được bục") &&
              has(doc, n(Hunter::kJumpUpKindMisses) + " lần bấm Ctrl+↑ mà game chỉ nhảy thường") &&
              has(doc, "phím ↑ bị giữ quá " + n(Hunter::kJumpUpHeldMaxMs) + " ms") &&
              Hunter::kJumpUpSolidAfter == 2 && has(doc, "rơi lại lần thứ hai lên cùng một bục") &&
              has(doc, "trong vòng " + n(GameMapBook::kNoJumpXPx) + " px quanh chỗ đó") &&
              has(doc, "tạm bỏ con quái đó " + n(2 * def.give_up_ms / 1000) + " giây") &&
              has(doc, "có tới " + n(kZoneReturnJumpsMax) + " lần nhảy lên") &&
              has(doc, "tối đa một dòng mỗi " + n(Hunter::kJumpUpSayMs / 1000) + " giây") &&
              has(doc, "đều giữ " + n(InputManager::kJumpHoldMinMs) + "–" + n(InputManager::kJumpHoldMaxMs) +
                           " ms") &&
              has(readme, "At most `kJumpUpsPerMinute` (" + n(Hunter::kJumpUpsPerMinute) +
                              ") presses a minute, `kJumpUpsPerTarget` (" + n(Hunter::kJumpUpsPerTarget) +
                              ") per monster") &&
              has(readme, "`kJumpUpFailsInRow` (" + n(Hunter::kJumpUpFailsInRow) + ") fails in a row or "
                          "`kJumpUpFailsPerRun` (" + n(Hunter::kJumpUpFailsPerRun) + ") in a run") &&
              has(readme, "`kJumpUpKindMisses` (" + n(Hunter::kJumpUpKindMisses) + ")") &&
              has(readme, "`GameMapBook::kNoJumpXPx` (" + n(GameMapBook::kNoJumpXPx) + ")") &&
              has(readme, "`kJumpUpStayMs` (" + n(Hunter::kJumpUpStayMs / 1000) + " s, `DropWhy::AfterJump`)") &&
              has(readme, "`kZoneReturnJumpsMax` (" + n(kZoneReturnJumpsMax) + ")") &&
              has(readme, "`kJumpUpSayMs` (" + n(Hunter::kJumpUpSayMs / 1000) + " s)") &&
              has(readme, "`kJumpUpJudgeMs` (" + n(Hunter::kJumpUpJudgeMs) + " ms)") &&
              has(readme, "`Hunter::kJumpUpLeadPx` (" + n(Hunter::kJumpUpLeadPx) + ")") &&
              has(readme, "`kJumpHoldMinMs`-`kJumpHoldMaxMs` (" + n(InputManager::kJumpHoldMinMs) + "-" +
                              n(InputManager::kJumpHoldMaxMs) + ")"),
          "the limits and latches: a minute's, a monster's, the shared gap, after an entry, after a drop or a jump, "
          "fails, kinds missed, Up held, the learned points and solid floors, the way back's jumps -- as hunter.hpp "
          "has them");

    // t52_s2's take-offs and the way back, and t53_s2's none, as the planner
    // finds them on the map's own file -- the owner's box kept (answer 6).
    const std::string data = game_data();
    if (data.empty() || !path_exists(data + "\\Map\\t53_s2.map")) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): the notes' t52_s2 and "
             "t53_s2 jumps\n");
    } else {
        GameMapBook book(data);
        const std::shared_ptr<const GameMapBook::Page> p52 = book.get("t52_s2");
        const std::shared_ptr<const GameMapBook::Page> p53 = book.get("t53_s2");
        const JumpRules all;
        const auto usable = [&all](const GameMapBook::Page& p, const HuntZone& z) {
            std::vector<JumpRun> out;
            for (const JumpRun& r : jump_runs(p, z, all))
                if (r.usable()) out.push_back(r);
            return out;
        };
        const auto takes = [&n](const JumpRun& r, const char* dash) {
            return n(r.take_l()) + dash + n(r.take_r());
        };
        bool ok = p52 != nullptr && p53 != nullptr;
        std::string seen;
        if (ok) {
            const HuntZone owner = box_of(880, 274, 265, 174, 1399, 694);
            const std::vector<JumpRun> use = usable(*p52, owner);
            const std::vector<JumpRun> use810 = usable(*p52, box_of(880, 274, 265, 174, 1399, 810));
            ok = use.size() == 2 && use810.size() == 3 &&
                 std::all_of(use810.begin(), use810.end(), [](const JumpRun& r) { return r.kind == JumpKind::CtrlUp; });
            if (ok) {
                seen = "Ctrl+Up take-offs " + takes(use[0], "..") + ", " + takes(use[1], "..") + ", with 810 " +
                       takes(use810[2], "..");
                const std::optional<ZoneReturn> back = return_point(*p52, owner, { 880, 634 }, &all);
                const int x = back && back->jump ? back->jump->x : -1;
                seen += ", the way back's from 880,634 at " + n(x);
                ok = std::none_of(use.begin(), use.end(), [](const JumpRun& r) { return r.x1 >= 505 && r.x0 <= 638; }) &&
                     x >= 800 && x <= 820 && back->home && back->jump->kind == JumpKind::CtrlUp &&
                     has(doc, "chỉ có Ctrl+↑ nhảy được, lên bục chỗ về: từ mặt đất ở khoảng x " + takes(use[0], "–") +
                                  ", hay từ chân dốc bên phải ở khoảng x " + takes(use[1], "–")) &&
                     has(doc, "Ở x 505–638 thì không") &&
                     has(doc, "có thêm Ctrl+↑ từ mặt đất lên dốc, ở khoảng x " + takes(use810[2], "–")) &&
                     has(doc, "bot đi sang trái tới khoảng x 800–820, đứng lại rồi bấm Ctrl+↑") &&
                     has(readme, "from the ground at x " + takes(use[0], "..") + " (the run " + n(use[0].x0) + ".." +
                                     n(use[0].x1) + ") and from the ramp's foot at " + takes(use[1], "..") + " (" +
                                     n(use[1].x0) + ".." + n(use[1].x1) + ")") &&
                     has(readme, "adds Ctrl + ↑ from the ground onto the ramp at " + takes(use810[2], "..")) &&
                     has(readme, "from 880,634 the way back walks to about x " + n(x) + " and presses Ctrl + ↑") &&
                     has(pack, "by Ctrl+Up at about x " + takes(use[0], "-"));
            }
            const bool none53 = usable(*p53, box_of(2635, 1188, 2335, 1088, 2935, 1288)).empty() &&
                                usable(*p53, box_of(2635, 1188, 2335, 1020, 2935, 1288)).empty();
            ok = ok && none53 &&
                 has(doc, "Ở t53_s2, trong vùng của bạn không có chỗ nào nhảy lên chắc chắn được (kể cả khi kéo cạnh "
                          "trên lên 1020)") &&
                 has(readme, "On t53_s2 no key has a usable jump in the owner's box, its top at 1088 or at 1020");
        }
        check(ok, "t52_s2's jumps as the notes give them -- Ctrl+Up from the ground and the ramp's foot onto the home "
                  "ledge, the way back from 880,634, the bottom at 810 -- and none on t53_s2, as the planner finds "
                  "them: " + seen);
    }

    // The seek's numbers.
    check(kSeekEdgeInsetPx == Hunter::kZoneWalkSlackPx && kSeekPlanEveryMs == 250 &&
              has(doc, "(chừa " + n(kSeekReachMarginPx) + " px theo chiều ngang, " + n(kSeekBandMarginPx) +
                           " px cao thấp)") &&
              has(doc, "Chỗ đứng đánh luôn cách chỗ bục ra khỏi vùng ít nhất " + n(kSeekEdgeInsetPx) + " px") &&
              has(doc, "(mỗi lần rơi tính thêm " + n(kSeekFallCostPx) + " px, mỗi lần nhảy lên thêm " +
                           n(kSeekJumpCostPx) + " px)") &&
              has(doc, "gần hơn ít nhất " + n(kSeekSwitchPx) + " px mà đã giữ con cũ " + n(kSeekCommitMs / 1000) +
                           " giây") &&
              has(doc, "Đi " + n(kSeekStuckMs / 1000) + " giây mà không gần chỗ đứng đánh thêm " +
                           n(kSeekProgressPx) + " px thì bỏ qua con đó " + n(kSeekPassOverMs / 1000) + " giây") &&
              has(doc, "mà " + vi_s(kSeekArriveMs) + " giây vẫn không thấy con quái trong tầm") &&
              has(doc, "GoBot tính lại mỗi 0,25 giây") &&
              has(doc, "rồi mỗi " + n(kSeekSaidMs / 1000) + " giây;") &&
              has(doc, "\"Hunter: monsters at X,Y ...\" mỗi " + n(kSeekSnapshotMs / 1000) + " giây") &&
              has(readme, "`kSeekReachMarginPx` (" + n(kSeekReachMarginPx) + ")") &&
              has(readme, "`kSeekBandMarginPx` (" + n(kSeekBandMarginPx) + ")") &&
              has(readme, "`kSeekEdgeInsetPx` (" + n(kSeekEdgeInsetPx) + ")") &&
              has(readme, "`kSeekFallCostPx` (" + n(kSeekFallCostPx) + ")") &&
              has(readme, "`kSeekJumpCostPx` (" + n(kSeekJumpCostPx) + ")") &&
              has(readme, "`kSeekSwitchPx` (" + n(kSeekSwitchPx) + ")") &&
              has(readme, "`kSeekCommitMs` (" + en_s(kSeekCommitMs) + " s)") &&
              has(readme, "`kSeekProgressPx` (" + n(kSeekProgressPx) + ")") &&
              has(readme, "`kSeekStuckMs` (" + en_s(kSeekStuckMs) + " s)") &&
              has(readme, "`kSeekArriveMs` (" + en_s(kSeekArriveMs) + " s)") &&
              has(readme, "`kSeekPassOverMs` (" + en_s(kSeekPassOverMs) + " s)") &&
              has(readme, "`kSeekPlanEveryMs` (" + n(kSeekPlanEveryMs) + " ms)") &&
              has(readme, "`kSeekSaidMs` (" + en_s(kSeekSaidMs) + " s)") &&
              has(readme, "`kSeekSnapshotMs` (" + en_s(kSeekSnapshotMs) + " s)"),
          "the seek's margins, inset, costs, switch, progress, arrival, pass-over and log clocks -- as hunt_seek.hpp "
          "has them");

    // The seek's seven questions, each (a) in one constant -- "báo lại nếu
    // muốn khác".
    check(!kSeekIdleHome && kSeekTiered && kPlatesOnFloors && kPlaceProjectWalks && !kSeekOutsideHit &&
              HunterConfig{}.seek_in_zone &&
              has(doc, "Các câu hỏi về Tự tìm quái chưa có câu trả lời, nên bản này làm theo cách (a) của từng câu; "
                       "báo lại nếu muốn khác") &&
              has(doc, "1. Trong vùng không còn con nào tới được: (a) đứng chờ tại chỗ, không đi về chỗ về") &&
              has(doc, "2. Thứ tự đi tìm: (a) bục đang đứng, rồi bục dưới, cuối cùng con phải nhảy lên") &&
              has(doc, "3. Độ cao của quái: (a) dùng đúng chỗ chấm đỏ của tab Bản đồ cho cả việc tìm lẫn việc đánh") &&
              has(doc, "4. Quái đi lâu không có gói tin mới: (a) đoán nó đi tiếp dọc bục, tới đầu bục thì dừng (tối "
                       "đa " + n(kPlaceWalkOnMaxMs / 1000) + " giây)") &&
              has(doc, "(b) như bản 05: sau " + n(PacketWorld::kMobWalkMs / 1000) + " giây coi như nó đứng lại") &&
              has(doc, "5. Quái ở sát ngoài vùng: (a) chỉ đánh khi đã ở trong tầm, không đi tới") &&
              has(doc, "6. Quái người khác đang đánh: (a) không đi tìm con mà người khác vừa đánh trong " +
                           n(kSeekOthersHurtMs / 1000) + " giây") &&
              has(doc, "7. Cách bật: (a) dấu \"Tự tìm quái trong vùng\" bật sẵn") &&
              has(readme, "(Q1, `kSeekIdleHome` false)") && has(readme, "(Q2, `kSeekTiered`)") &&
              has(readme, "(Q3, `kPlatesOnFloors`") && has(readme, "(Q4, `kPlaceProjectWalks`") &&
              has(readme, "(Q5, `kSeekOutsideHit` false)") &&
              has(readme, "`kSeekOthersHurtMs` (" + en_s(kSeekOthersHurtMs) + " s)") &&
              has(readme, "`kPlaceWalkOnMaxMs` (" + en_s(kPlaceWalkOnMaxMs) + " s)") &&
              has(readme, "(`PacketWorld::kMobWalkMs`, " + en_s(PacketWorld::kMobWalkMs) + " s)"),
          "the seek's questions 1-7, each (a) as its constant has it: stand where it is, tiered, plates on floors, "
          "walks projected, none just outside gone to, others' passed by, on by default");

    // The owner's answers to the jump up, and the old promises -- 04d's Q4, no
    // jump up; its way back that never jumps; its advice to move the home
    // down; 05's heights -- put right where they were made.
    HunterConfig spot_on;
    spot_on.spot = true;
    spot_on.jump_to_reach = true;
    spot_on.jump_key = VK_UP;
    check(!Hunter::hops_to_targets(spot_on) && spot_on.jump_up &&
              has(doc, "Câu trả lời của bạn cho Nhảy lên bục trên (bản này làm đúng như vậy):") &&
              has(doc, "1. Độ cao: \"Theo GoC\" (↑ khoảng 133, ↑↑ khoảng 280, Ctrl+↑ khoảng 432 px)") &&
              has(doc, "2. Ctrl+↑ đứng trên cửa không đổi bản đồ") &&
              has(doc, "3. Lúc bay được vọt cao hơn cạnh trên của vùng") &&
              has(doc, "4. Quái ở bục trên: nhảy lên bục của nó rồi đánh như thường") &&
              has(doc, "5. Bật sẵn, với mọi giới hạn ở trên") &&
              has(doc, "thì bot vẫn không nhảy lên bục trên để đánh quái (câu hỏi 4), như trước. (Sửa ở bản "
                       "2026.11.05b: bạn đã đổi câu 4.") &&
              has(doc, "Chưa cho bot nhảy lên bục trên để đánh quái. (Sửa ở bản 2026.11.05b: bạn đã trả lời khác") &&
              has(doc, "nếu có dốc lên tới đó. (Sửa ở bản 2026.11.05b: có dấu Nhảy lên bục trên thì đường về có thể "
                       "có tới " + n(kZoneReturnJumpsMax) + " lần nhảy lên") &&
              has(doc, "(tính thử thì vùng đó không có bục đỏ nào). (Sửa ở bản 2026.11.05b: không cần nữa.") &&
              has(doc, "lệch xa chỗ đứng thật. (Sửa ở bản 2026.11.05b: khi bot dùng vùng của tab Bản đồ với dấu Tự "
                       "tìm quái trong vùng") &&
              has(readme, "Q4: no hop toward a monster is added (`hops_to_targets`, as it was) -- reversed by the "
                          "owner 2026-11-05") &&
              has(readme, "the hunter keeps its heights until answered (since 2026.11.05b, while an area is in use "
                          "with *Tự tìm quái trong vùng*, the hunter's plates stand where `mob_place` puts the dots") &&
              has(hunt, "Since 2026.11.05b, with jump_up and seek_in_zone (above), such an area also has planned "
                        "jumps up") &&
              has(hunt, "Từ bản 2026.11.05b, có jump_up và seek_in_zone (ở trên)"),
          "the owner's answers 1-5 to the jump up; 04d's Q4 reversed (README: \"reversed by the owner 2026-11-05\"), "
          "and its way back, its advice to move the home down and 05's heights put right where they were said");

    // The jump up's log words, as hunter_jump.cpp and gui_app.cpp write them.
    check(has(jump_src, "\"Hunter: jump up (%s) %s: from %d,%d on floor %d of %s (trusted: %s), onto floor %d at x %d "
                        "(rise %d; \"") &&
              has(jump_src, "\"peak %d..%d, headroom %d; no floor outside the area within %d above), run %d..%d, "
                            "inside the area; no \"") &&
              has(doc, "\"Hunter: jump up (Ctrl+Up) toward ... (hay on the way home): from X,Y on floor ..., onto "
                       "floor ... (rise ...; peak " + n(cu.lo) + ".." + n(cu.hi) +
                           ", headroom ...; no floor outside the area within " + n(kJumpUpCeilingPx) + " above) ...\"") &&
              has(readme, "(rise N; peak " + n(cu.lo) + ".." + n(cu.hi) +
                              ", headroom N; no floor outside the area within " + n(kJumpUpCeilingPx) + " above)") &&
              has(jump_src, "\"Hunter: jump up worked: landed at %d,%d on floor %d, %d px up, %llu ms after the "
                            "key%s\"") &&
              has(jump_src, "\"0x002A: %s %.0f at +%lld ms, landed +%lld; flight %d ms%s\"") &&
              has(jump_src, "\", g≈%.0f\"") &&
              has(doc, "\"Hunter: jump up worked: landed at ...\" (cuối dòng có \"flight N ms, g≈N\"") &&
              has(jump_src, "\"FELL BACK\" : \"landed LOWER\"") && has(jump_src, "did nothing in %llu ms") &&
              has(jump_src, "\"Hunter: %s came out as %s -- %d of %d%s\"") &&
              has(jump_src, "OFF until the next BẮT ĐẦU") &&
              has(jump_src, "\"Hunter: MAP CHANGED %.1f s after %s at %d,%d on %s -> %s at %d,%d -- hunting STOPS") &&
              has(doc, "\"jump up FELL BACK ...\", \"jump up landed LOWER ...\", \"did nothing in ... ms\", \"Ctrl+Up "
                       "came out as ...\", \"... OFF until the next BẮT ĐẦU\", và WARN \"MAP CHANGED\"") &&
              has(jump_src, "\"Hunter: not jumping up %s -- %s%s\"") &&
              has(doc, "\"Hunter: not jumping up to ... -- ...\"") &&
              has(jump_src, "\"Hunter: a jump up would go here (%s): %s at x %d from floor %d onto floor %d, %s\"") &&
              has(jump_src, "\"jump_up is off\"") &&
              has(doc, "\"Hunter: a jump up would go here (jump_up is off): Ctrl+Up at x ... from floor ... onto "
                       "floor ...\"") &&
              has(jump_src, "\"; jump-ups: looks %d, would %d, settles %d, pressed %d (Up %d, Up,Up %d, Ctrl+Up %d), "
                            "worked %d, \"") &&
              has(jump_src, "\"; g≈%.0f (%d flight%s)\"") &&
              has(doc, "\"; jump-ups: looks N, would N, settles N, pressed N (Up N, Up,Up N, Ctrl+Up N), worked N, "
                       "fell back N, ...\" và \"; g≈N (N flights)\"") &&
              has(readme, "`; jump-ups: looks N, would N, settles N, pressed N (Up a, Up,Up b, Ctrl+Up c), worked N, "
                          "fell back N, …; g≈N (n flights); refused: …`") &&
              has(gui_src, "<< \" (jumps \"") &&
              has(gui_src, "<< \" (walk-offs \" << ws.air_walkoffs << \"), second \"") &&
              has(doc, "\"002A:N (jumps N (walk-offs N), second N, high N, landed N, other N)\"") &&
              has(readme, "`002A:N (jumps J (walk-offs W), second S, high H, landed L, other O)`") &&
              has(proto, "`002A:N (jumps J (walk-offs W), second S, high H, landed L, other O)`"),
          "the jump up's press, verdict, refusal, would-jump and minute lines, and the Positions line's walk-offs, "
          "as the code writes them");

    // The seek's log words, as hunter_seek.cpp, hunter.cpp and gui_app.cpp
    // write them.
    check(has(seek_src, "\"Hunter: seeking slot %d gen %u at %d,%d (floor %d, %s%s)%s -- attack from %d,%d: %s (%s; "
                        "cost \"") &&
              has(seek_src, "\"Hunter: not going for slot %d gen %u at %d,%d (floor %d) -- %s%s\"") &&
              has(seek_src, "\"Hunter: slot %d %s; passed over for %llu s\"") &&
              has(seek_src, "\"not found at %d,%d -- in reach on the map from %d,%d") &&
              has(seek_src, "\"Hunter: nothing to seek for %.1f s at %d,%d (floor %d, inside the area) -- %d alive on "
                            "the map, %d \"") &&
              has(seek_src, "-- standing where it is\"") &&
              has(seek_src, "\"Hunter: monsters at %d,%d (floor %d, %s the area): %s\"") &&
              has(seek_src, "\" (projected %.1f s)\"") &&
              has(seek_src, "\"; seek: goals %d (your floor %d, below %d, above %d), reached %d, switched %d, not "
                            "found %d, stuck \"") &&
              has(seek_src, "nothing to seek %.1f s; time: attacking %.1f, approaching %.1f, seeking %.1f, ") &&
              has(hunter_src, "\"Hunter (last %.1f s, hunt stopped): %d fights%s\"") &&
              has(gui_src, "line << \"; the hunter's plates: \" << pc.in_view << \" of \" << pc.alive << \" living "
                           "monsters in view (±\"") &&
              has(gui_src, "\" projected past \"") &&
              has(doc, "\"Hunter: seeking slot ... -- attack from X,Y: ...\"") &&
              has(doc, "\"Hunter: not going for slot ... -- ...\"") &&
              has(doc, "\"Hunter: slot N not found at ...; passed over for " + n(kSeekPassOverMs / 1000) + " s\"") &&
              has(doc, "\"Hunter: nothing to seek for N s at X,Y ... -- standing where it is\"") &&
              has(doc, "(con đoán chỗ có \"projected\")") &&
              has(doc, "\"; seek: goals N (your floor N, below N, above N), reached N, ..., nothing to seek N s; time: "
                       "attacking N, approaching N, seeking N, nothing to seek N, walking back N, ...\"") &&
              has(doc, "(\"Hunter (last N s, hunt stopped): ...\")") &&
              has(doc, "\"the hunter's plates: N of M living monsters in view (...); placed on floors: N walking (N "
                       "projected past " + n(PacketWorld::kMobWalkMs / 1000) + " s), N set down\"") &&
              has(readme, "`Hunter: seeking slot N gen G at x,y (floor N, in view|out of view) -- attack from x,y: "
                          "<the way> (<tier>; cost N); in the area N: reachable N, above with no way up N, passed "
                          "over N`") &&
              has(readme, "`Hunter: nothing to seek for N s at x,y … -- standing where it is`") &&
              has(readme, "`; seek: goals N (your floor a, below b, above c), reached N, switched N, not found N, "
                          "stuck N, no way to N; nothing to seek N s; time: attacking …") &&
              has(readme, "(`Hunter (last N s, hunt stopped): …`)") &&
              has(readme, "`the hunter's plates: N of M living monsters in view (±W x ±H); placed on floors: N "
                          "walking (P projected past " + en_s(PacketWorld::kMobWalkMs) + " s), S set down`"),
          "the seek's goal, no-way, not-found, nothing-to-seek, snapshot and minute lines, the stop's flush and the "
          "Positions line's plates, as the code writes them");

    // What the notes quote of the screen: the GUI's own words.
    const std::string tries = std::string("\"Thử ") + jump_kind_key(JumpKind::Up) + "\", \"Thử " +
                              jump_kind_key(JumpKind::UpUp) + "\" và \"Thử " + jump_kind_key(JumpKind::CtrlUp) +
                              "\" ở dòng \"Đo cú nhảy:\"";
    check(has(style_src, "label(\"Đo cú nhảy:\")") && has(style_src, "std::string(\"Thử \") + jump_kind_key(k)") &&
              has(doc, tries) &&
              has(readme, std::string("**Thử ") + jump_kind_key(JumpKind::Up) + "**, **Thử " +
                              jump_kind_key(JumpKind::UpUp) + "** and **Thử " + jump_kind_key(JumpKind::CtrlUp) +
                              "**") &&
              has(style_src, "\"Jump test: %s %s -- %s (packets: jumps %d, walk-offs %d") &&
              has(doc, "\"Jump test: ...\"") && has(gui_src, "std::string(\"Lần nhảy lên lúc \")") &&
              has(gui_src, "\" làm đổi bản đồ — GoBot đã dừng đánh quái và bỏ dấu Nhảy lên.\"") &&
              has(doc, "\"Lần nhảy lên lúc ... làm đổi bản đồ — GoBot đã dừng đánh quái và bỏ dấu Nhảy lên.\"") &&
              has(style_src, "\"Nhảy lên đã tắt tới lần BẮT ĐẦU sau: \"") &&
              has(style_src, "\"nhân vật nhảy khác với GoBot tính. Hãy gửi file log.\"") &&
              has(doc, "\"Nhảy lên đã tắt tới lần BẮT ĐẦU sau: nhân vật nhảy khác với GoBot tính. Hãy gửi file "
                       "log.\"") &&
              has(style_src, "\"Nhảy lên (đang tắt): lẽ ra đã nhảy \"") && has(style_src, "\" lần — xem log.\"") &&
              has(doc, "\"Nhảy lên (đang tắt): lẽ ra đã nhảy N lần — xem log.\"") &&
              has(view_src, "std::string(\" Về: nhảy \") + jump_kind_key(hs.spot_way_jump_kind) + \" ở x \"") &&
              has(doc, "\"Về: nhảy Ctrl+↑ ở x ...\"") &&
              has(spot_src, "\" không có đường đi bộ, rơi xuống hay nhảy lên nào về vùng: bot đánh ở đây. Tự đưa \"") &&
              has(doc, "\"... không có đường đi bộ, rơi xuống hay nhảy lên nào về vùng: bot đánh ở đây. ...\"") &&
              has(style_src, "\"Trong vùng không còn con quái nào tới được — đứng chờ ở đây.\"") &&
              has(doc, "\"Trong vùng không còn con quái nào tới được — đứng chờ ở đây.\"") &&
              has(style_src, "\" con ở bục trên: chưa nhảy lên được — bật “Nhảy lên bục trên”.\"") &&
              has(doc, "\"N con ở bục trên: chưa nhảy lên được — bật “Nhảy lên bục trên”.\"") &&
              has(style_src, "\"Đang đi tìm quái trong vùng: \"") && has(style_src, "std::string(\" (bên \")") &&
              has(style_src, "\" lên bục \"") && has(style_src, "\" rồi đi tới con ở \"") &&
              has(doc, "\"Đang đi tìm quái trong vùng: con ở X, Y (bên trái N px).\"") &&
              has(doc, "\"Đang đi tìm quái trong vùng: nhảy Ctrl+↑ ở x ... lên bục ... rồi đi tới con ở X, Y.\"") &&
              has(spot_src, "\"Tự tìm quái: bật — bot đi tới quái") && has(doc, "\"Tự tìm quái: bật — ...\"") &&
              has(map_src, "\"Không tới được: bục trên, chưa nhảy lên\"") && has(map_src, "\"Tạm bỏ qua, còn \"") &&
              has(doc, "\"Không tới được: bục trên, chưa nhảy lên\", \"Tạm bỏ qua, còn N s\""),
          "the Thử buttons, the red lines, the would-jump and way-back lines, the seek's card lines and hovers the "
          "notes quote, as the GUI says them");

    // The protocol notes (jumps against walk-offs) and GoC's, put right.
    check(has(proto, "**Jumps against walk-offs** (2026.11.05b, for the jump up).") &&
              has(proto, "in `air_walkoffs`, beside it: a walk-off stays among the jumps") &&
              has(proto, "kept in a ring of " + n(PacketWorld::kOwnTraceSize) + " (`PacketWorld::own_since`)") &&
              has(proto, "a state 1 and then a state 6 within " + n(Hunter::kJumpUpSecondSeenMs) + " ms") &&
              has(goc, "GhostCore 0x1800478B0 -- earlier versions of this page said 0x180046A80") &&
              !has(goc, "More than 484 px above is not tried this way") &&
              has(goc, "More than 484 px above, it tries \"stairs\" first") &&
              has(goc, "The 341 ms is counted from the first key's release") &&
              has(goc, "the double jump's `DJ_` edges are never built") &&
              has(goc, "It goes by |dy|, so a monster below qualifies too; there is no ground check and no gate check") &&
              has(goc, "**None of this ran in the owner's play**"),
          "protocol-findings.md: the walk-offs among the jumps, the own-report ring, ↑↑'s second within "
          "kJumpUpSecondSeenMs; goc-reference.md: HandleUseSkill's address, the stairs over 484 px, the 341 ms from "
          "the release, no DJ_ edge, the jump attack by |dy|, and none of it run in the owner's profile");
#else
    skip("GOBOT_SOURCE_DIR is not set\n");
#endif
}

} // namespace

void test_map_tab(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_phase0();
    test_sketch();
    test_no_drop_marks();
    test_camera();
    test_grips();
    test_gesture();
    test_snap();
    test_zone_check();
    test_zone_walk();
    test_zone_t52s2();
    test_answers();
    test_zone_book();
    test_identity();
    test_view_job();
    test_tab_rules();
    test_dots();
    test_dots_t52s2();
    test_dots_drawn();
    test_gui_source();
    test_package_notes();
    test_notes_04d();
    test_notes_05();
    test_notes_05b();
}
