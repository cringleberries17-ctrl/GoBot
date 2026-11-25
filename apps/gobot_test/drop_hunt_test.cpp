// gobot_test -- the hunter's drop to a monster on a floor below, Ctrl + ↓
// ("Nhảy xuống bậc dưới", HunterConfig::drop_down): the drop plan's §10 B to
// D, with the player's answers of 2026-11-04.
//
// drop_rule on its own, each rule one at a time, over t52_s2's floors and
// gates as t52_s2_planar.txt reads them -- written as made-up map files to
// this run's own temp folder, never the game's. Then the hunter on a pretend
// game over those files: plates and fixes in the packets' world pixels, a
// pretend Ctrl + ↓ that writes down what went and has the character fall,
// stay, rise or change map as the scenario says, and the test's own clock.
// Then hunt.ini's four keys, and GoBot's window around the drop, read off its
// source. Nothing here presses a real key or reads the game's install.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_book.hpp"
#include "gobot/mob_detector.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
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

using Why = Hunter::DropWhy;

// ------------------------------------------------------------- the files

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
    /// A floor at y from x0 to x1 in `layer`, its cells' attribute `attr`.
    void flat(int layer, int x0, int x1, int y, std::uint8_t attr) {
        for (int x = x0; x <= x1; ++x) column(layer, x, y, attr);
    }
    /// One column of a floor: its top at y.
    void column(int layer, int x, int y, std::uint8_t attr) {
        const std::size_t r = 8 + static_cast<std::size_t>(layer) * static_cast<std::size_t>(w) *
                                      static_cast<std::size_t>(h) * 36 +
                              (static_cast<std::size_t>(x / 32) * static_cast<std::size_t>(h) +
                               static_cast<std::size_t>(y / 32)) * 36;
        b[r + 2] = attr;
        b[r + 3 + static_cast<std::size_t>(x % 32)] = static_cast<std::uint8_t>(y % 32);
    }
};

/// A .prj as the game's: the kinds with their sprites, then each placing of
/// one -- a name of 20 bytes, [i32 kind][u8][f32 1.0][i32 x][i32 y].
struct PrjMaker {
    Bytes b;

    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void text(const char* s) {
        for (const char* p = s; *p; ++p) b.push_back(static_cast<std::uint8_t>(*p));
        b.push_back(0);
    }
    void junk(std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(0x5A + i * 7));
    }
    void kind(std::int32_t id, const char* path) {
        u32(4);
        u32(static_cast<std::uint32_t>(id));
        u32(2);
        text(path);
    }
    void place(std::int32_t kind, std::int32_t x, std::int32_t y) {
        for (int i = 0; i < 20; ++i) b.push_back(i < 6 ? static_cast<std::uint8_t>(0xC0 + i) : 0);
        u32(static_cast<std::uint32_t>(kind));
        b.push_back(0);
        float one = 1.0f;
        std::uint32_t u = 0;
        std::memcpy(&u, &one, 4);
        u32(u);
        u32(static_cast<std::uint32_t>(x));
        u32(static_cast<std::uint32_t>(y));
    }
};

/// t52_s2's floors where the player's two live cases were, flat at the
/// height t52_s2_planar.txt reads at the cases' columns, and its bottom.
struct Seg {
    int          layer;
    int          x0, x1, y;
    std::uint8_t attr;
};
const Seg kFloors[] = {
    { 1, 2225, 2556, 238, 1 },  // the small floor up right (234-240)
    { 1, 2300, 2900, 372, 1 },  // the long floor (339-543): 372 at 2675
    { 1, 2418, 2541, 628, 1 },  // the small floor under it (592-634): 628 at 2513
    { 0, 2658, 2965, 633, 1 },  // the 전투존 gate's floor (625-635): 633 at 2675
    { 0, 2200, 2800, 984, 1 },  // the low floor (531-987): 984 at 2545
    { 0, 472, 1104, 275, 1 },   // the 피스존 gate's floor (271-281)
    { 0, 1800, 2000, 850, 1 },  // under 2026-11-04 07:59:20's monster at 1929,819 (850 there)
    { 0, 0, 3199, 1120, 2 },    // the bottom ground, solid (1116-1124)
};
constexpr int kJeontu = 3900008;    // 전투존, 2812,622
constexpr int kPiseu = 3900000;     // 피스존, 881,267

/// The made-up data folder: t52_s2's .map and .prj, and a decoy map that
/// has only the bottom ground, for naming. Written once a run.
std::string fixture_folder() {
    static std::string dir;
    if (!dir.empty()) return dir;
    dir = temp_path("drop_hunt");
    make_folder(dir);
    make_folder(dir + "\\Map");
    make_folder(dir + "\\Project");
    MapMaker map(100, 40);
    for (const Seg& s : kFloors) map.flat(s.layer, s.x0, s.x1, s.y, s.attr);
    PrjMaker prj;
    prj.junk(12);
    prj.kind(kPiseu, "\\Data\\OBJ\\Object\\warpzone.spr");
    prj.kind(kJeontu, "\\Data\\OBJ\\Object\\warpzone.spr");
    prj.kind(4100001, "\\Data\\OBJ\\Object\\tree.spr");
    prj.junk(9);
    prj.place(kJeontu, 2812, 622);
    prj.place(kPiseu, 881, 267);
    prj.place(4100001, 2600, 300);
    prj.junk(5);
    MapMaker decoy(100, 40);
    decoy.flat(0, 0, 3199, 1120, 2);
    // 2026.11.12: t23_s14 at 22:22:00 -- the take-off floor 853 from x 135
    // right, sloping down left to 932 at x 30 (follow.py of the real file:
    // 853, 882 at 90, 906 at 60, 932 at 30), floor 1014 under it, a gate at
    // 500,600 on floor 633; and t62_s5's spawn 265,5104, 33 px over floor 5137,
    // by its gate at 265,5125.
    MapMaker slope(20, 40);
    for (int x = 0; x <= 400; ++x)
        slope.column(0, x, x >= 135 ? 853 : 853 + static_cast<int>(std::lround((135 - x) * 0.75)), 1);
    slope.flat(0, 0, 400, 1014, 1);
    slope.flat(0, 450, 600, 633, 1);
    slope.flat(1, 0, 639, 1250, 2);
    PrjMaker slope_prj;
    slope_prj.junk(12);
    slope_prj.kind(kJeontu, "\\Data\\OBJ\\Object\\warpzone.spr");
    slope_prj.junk(9);
    slope_prj.place(kJeontu, 500, 600);
    slope_prj.junk(5);
    MapMaker spawn(20, 180);
    spawn.flat(0, 0, 639, 5137, 1);
    spawn.flat(1, 0, 639, 5700, 2);
    PrjMaker spawn_prj;
    spawn_prj.junk(12);
    spawn_prj.kind(kPiseu, "\\Data\\OBJ\\Object\\warpzone.spr");
    spawn_prj.junk(9);
    spawn_prj.place(kPiseu, 265, 5125);
    spawn_prj.junk(5);
    // 2026.11.12, the owner's Q7: t91_s1, two platforms 40 px apart -- 500
    // and 540, x 0..1200 -- over the bottom at 1100, its gate at 2000,1092.
    MapMaker close(80, 40);
    close.flat(0, 0, 1200, 500, 1);
    close.flat(1, 0, 1200, 540, 1);
    close.flat(0, 0, 2559, 1100, 2);
    PrjMaker close_prj;
    close_prj.junk(12);
    close_prj.kind(kPiseu, "\\Data\\OBJ\\Object\\warpzone.spr");
    close_prj.junk(9);
    close_prj.place(kPiseu, 2000, 1092);
    close_prj.junk(5);
    const bool ok = write_bytes(dir + "\\Map\\t91_s1.map", close.b) &&
                    write_bytes(dir + "\\Project\\t91_s1.prj", close_prj.b) &&
                    write_bytes(dir + "\\Map\\t52_s2.map", map.b) &&
                    write_bytes(dir + "\\Project\\t52_s2.prj", prj.b) &&
                    write_bytes(dir + "\\Map\\t99_s9.map", decoy.b) &&
                    write_bytes(dir + "\\Map\\t23_s14.map", slope.b) &&
                    write_bytes(dir + "\\Project\\t23_s14.prj", slope_prj.b) &&
                    write_bytes(dir + "\\Map\\t62_s5.map", spawn.b) &&
                    write_bytes(dir + "\\Project\\t62_s5.prj", spawn_prj.b);
    if (!ok) dir.clear();
    return dir;
}

/// The game's own t23_s14 (PhòngSinhTử[4]), copied from build\handoff --
/// never the game's install -- with a .prj written for it: one gate, by the
/// entry's spawn at 300,1001, far from the owner's box of 2026-11-11. "" with
/// no copy here. Written once a run.
std::string real_folder() {
    static std::string dir;
    static bool tried = false;
    if (tried) return dir;
    tried = true;
#ifdef GOBOT_SOURCE_DIR
    const std::string real =
        std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data\\Map\\t23_s14.map";
    std::ifstream in(real, std::ios::binary);
    if (!in) return dir;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Bytes b(text.size());
    if (!text.empty()) std::memcpy(b.data(), text.data(), text.size());
    dir = temp_path("drop_real");
    make_folder(dir);
    make_folder(dir + "\\Map");
    make_folder(dir + "\\Project");
    PrjMaker prj;
    prj.junk(12);
    prj.kind(kPiseu, "\\Data\\OBJ\\Object\\warpzone.spr");
    prj.junk(9);
    prj.place(kPiseu, 300, 994);
    prj.junk(5);
    if (text.empty() || !write_bytes(dir + "\\Map\\t23_s14.map", b) || !write_bytes(dir + "\\Project\\t23_s14.prj", prj.b))
        dir.clear();
#endif
    return dir;
}

// ------------------------------------------------------------- the log

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

int lines_with(const std::vector<std::string>& all, const char* a, const char* b = "") {
    int n = 0;
    for (const std::string& l : all)
        if (l.find(a) != std::string::npos && l.find(b) != std::string::npos) ++n;
    return n;
}

std::string line_with(const std::vector<std::string>& all, const char* a) {
    for (const std::string& l : all)
        if (l.find(a) != std::string::npos) return l;
    return {};
}

// ------------------------------------------------------------- the rule

/// A monster's plate from the packets standing at x, y.
Nameplate mob_at(int x, int y, int slot) {
    Nameplate n;
    n.bar = ScreenRect{ x - 20, y, 40, 3 };
    n.plate = ScreenRect{ x - 30, y - 14, 60, 12 };
    n.hp = 1.0f;
    n.slot = slot;
    n.gen = 1;
    n.actor_checked = true;
    return n;
}

HunterConfig rule_config(int gate_px) {
    HunterConfig c;
    c.drop_down = true;
    c.packet_positions = PacketPositions::On;
    c.reach = 450;
    c.jump_key = 38;
    c.drop_gate_px = gate_px;
    return c;
}

/// The player's 06:48:49 at the press: you at 2675,371 on floor 372, the
/// monster at 2673,638 -- the live numbers -- the walk's stop reported.
Hunter::DropFacts owner_case(const std::shared_ptr<const GameMapBook::Page>& page, std::vector<MapGate> gates) {
    Hunter::DropFacts f;
    f.press = true;
    f.now = 100'000;
    f.wired = true;
    f.packet_target = true;
    f.dx = -2;
    f.dy = 267;
    f.target_wx = 2673.0f;
    f.target_wy = 638.0f;
    f.fix.state = WorldFix::State::Known;
    f.fix.x = f.fix.rx = 2675.0f;
    f.fix.y = f.fix.ry = 371.0f;
    f.fix.report_ms = 99'960;
    f.fix.map_known = true;
    f.fix.map = 52;
    f.fix.area = 2;
    f.entry_ms = 10'000;
    f.settle_from_ms = 99'940;
    f.walk_held = true;
    f.page = page;
    f.gates = std::move(gates);
    return f;
}

void test_drop_rule() {
    section("Hunter::drop_rule: each rule of the drop plan's 3.2, one at a time, over t52_s2's floors and gates");

    const std::string dir = fixture_folder();
    check(!dir.empty(), "the made-up t52_s2 written to this run's temp folder");
    if (dir.empty()) return;
    GameMapBook book(dir);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    const std::vector<MapGate> gates = book.gates("t52_s2");
    check(page && gates.size() == 2 && gates[0].x == 2812 && gates[0].y == 622 && gates[1].x == 881,
          fmt("the fixture reads: t52_s2's floors and its two gates, 2812,622 and 881,267 (%zu gates)", gates.size()));
    if (!page) return;
    const Hunter::DropFacts base = owner_case(page, gates);

    // The player's own case, refused for the 전투존 gate 137 px across at
    // the default 150 -- dropped to at 136, landing on the monster's floor.
    {
        const Hunter::DropCall at150 = Hunter::drop_rule(rule_config(150), base);
        check(at150.why == Why::Gate && at150.px == 137 && at150.land_y == 633 && at150.direct,
              fmt("06:48:49 at drop_gate_px 150: refused, the 전투존 gate %d px across (landing %d)", at150.px,
                  at150.land_y));
        const Hunter::DropCall at136 = Hunter::drop_rule(rule_config(136), base);
        check(at136.ok() && at136.land_y == 633 && at136.direct && at136.stand_y == 372 && at136.gate_px == 137 &&
                  at136.x0 == 2675 && at136.y0 == 371,
              "at 136: it goes -- from 2675,371 on floor 372 to floor 633, the monster's, the gate 137 px across");
        Hunter::DropFacts settle = base;
        settle.press = false;
        check(Hunter::drop_rule(rule_config(136), settle).ok(), "and as a settle begins, by the fix worked out");
    }

    // 06:48:26: from the small floor up right to the long floor, then to the
    // small floor by the monster; from further right, past it.
    {
        Hunter::DropFacts f = base;
        f.fix.x = f.fix.rx = 2513.0f;
        f.fix.y = f.fix.ry = 237.0f;
        f.target_wx = 2556.0f;
        f.target_wy = 633.0f;
        f.dx = 43;
        f.dy = 396;
        const Hunter::DropCall one = Hunter::drop_rule(rule_config(150), f);
        check(one.ok() && one.land_y == 372 && !one.direct && one.stand_y == 238,
              fmt("06:48:26 from 2513,237: goes, to floor %d, one between", one.land_y));
        f.fix.y = f.fix.ry = 371.0f;
        f.dy = 262;
        const Hunter::DropCall two = Hunter::drop_rule(rule_config(150), f);
        check(two.ok() && two.land_y == 628 && two.direct, fmt("then from 2513,371: goes, to floor %d, the monster's",
                                                                two.land_y));
        f.fix.x = f.fix.rx = 2550.0f;
        f.dx = 6;
        const Hunter::DropCall past = Hunter::drop_rule(rule_config(150), f);
        check(past.why == Why::PastMonster && past.land_y == 984 && past.px == 351,
              fmt("from 2550,371: refused, it would land on %d, %d px past the monster", past.land_y, past.px));
    }

    const HunterConfig cfg = rule_config(136);
    struct Case {
        const char*                            what;
        std::function<void(HunterConfig&, Hunter::DropFacts&)> change;
        Why                                    why;
    };
    const auto at = [](int x, int y) {
        return [x, y](HunterConfig&, Hunter::DropFacts& f) {
            f.fix.x = f.fix.rx = static_cast<float>(x);
            f.fix.y = f.fix.ry = static_cast<float>(y);
        };
    };
    const std::vector<Case> cases = {
        { "drop_down off", [](HunterConfig& c, Hunter::DropFacts&) { c.drop_down = false; }, Why::Off },
        { "the packets only watched",
          [](HunterConfig& c, Hunter::DropFacts&) { c.packet_positions = PacketPositions::Watch; }, Why::NoPackets },
        { "Đứng yên một chỗ", [](HunterConfig& c, Hunter::DropFacts&) { c.stay_put = true; }, Why::StayPut },
        { "the spot on, standing for want of a fix", [](HunterConfig& c, Hunter::DropFacts&) { c.spot = true; },
          Why::Spot },
        // No "drops off for the run" since 2026.11.24 (DropWhy::Latched gone):
        // nothing switches the drops off.
        { "a key-up owed", [](HunterConfig&, Hunter::DropFacts& f) { f.owed = true; }, Why::KeysOwed },
        { "the jump key Ctrl", [](HunterConfig& c, Hunter::DropFacts&) { c.jump_key = 0x11; }, Why::KeyClash },
        { "the left key right Ctrl", [](HunterConfig& c, Hunter::DropFacts&) { c.left_key = 0xA3; }, Why::KeyClash },
        { "the attack key left Ctrl", [](HunterConfig& c, Hunter::DropFacts&) { c.attack_key = 0xA2; }, Why::KeyClash },
        { "no map book or veto", [](HunterConfig&, Hunter::DropFacts& f) { f.wired = false; }, Why::NotWired },
        { "a screen plate", [](HunterConfig&, Hunter::DropFacts& f) { f.packet_target = false; },
          Why::NotPacketTarget },
        { "a monster on the level", [](HunterConfig&, Hunter::DropFacts& f) { f.level_monster = true; },
          Why::LevelMonster },
        // The lower bar alone (2026.11.12, the owner's Q7): 60, height_tolerance's.
        { "60 px below", [](HunterConfig&, Hunter::DropFacts& f) { f.dy = 60; }, Why::NotBelow },
        { "140 px below with down() 150", [](HunterConfig& c, Hunter::DropFacts& f) { c.down_reach = 150; f.dy = 140; },
          Why::NotBelow },
        { "20 px below with down() 20", [](HunterConfig& c, Hunter::DropFacts& f) { c.down_reach = 20; f.dy = 20; },
          Why::NotBelow },
        { "141 px across at the press", [](HunterConfig&, Hunter::DropFacts& f) { f.dx = 141; }, Why::TooFarSide },
        { "121 px across as a settle begins", [](HunterConfig&, Hunter::DropFacts& f) { f.press = false; f.dx = -121; },
          Why::TooFarSide },
        { "the fix Lost", [](HunterConfig&, Hunter::DropFacts& f) { f.fix.state = WorldFix::State::Lost; },
          Why::NotKnown },
        { "the fix uncertain", [](HunterConfig&, Hunter::DropFacts& f) { f.fix.uncertain = true; }, Why::NotKnown },
        { "a map entry 4 s ago", [](HunterConfig&, Hunter::DropFacts& f) { f.entry_ms = f.now - 4000; },
          Why::AfterEntry },
        { "moving at the press", [](HunterConfig&, Hunter::DropFacts& f) { f.fix.moving = true; }, Why::NotSettled },
        { "in the air at the press", [](HunterConfig&, Hunter::DropFacts& f) { f.fix.airborne = true; },
          Why::NotSettled },
        { "the walk's stop not reported since the settle",
          [](HunterConfig&, Hunter::DropFacts& f) { f.fix.report_ms = f.settle_from_ms - 1; }, Why::NotSettled },
        { "a hop or a drop in the air", [](HunterConfig&, Hunter::DropFacts& f) { f.in_air = true; }, Why::InAir },
        { "the last press 1 s ago", [](HunterConfig&, Hunter::DropFacts& f) { f.last_press_ms = f.now - 1000; },
          Why::TooSoon },
        { "a step back not judged", [](HunterConfig&, Hunter::DropFacts& f) { f.step_judge = true; }, Why::StepJudge },
        { "the left key down at the press", [](HunterConfig&, Hunter::DropFacts& f) { f.left_held = true; },
          Why::Walking },
        { "4 presses this minute", [](HunterConfig&, Hunter::DropFacts& f) { f.presses_minute = 4; }, Why::MinuteCap },
        { "2 presses for this monster", [](HunterConfig&, Hunter::DropFacts& f) { f.presses_target = 2; },
          Why::TargetCap },
        { "a hold", [](HunterConfig&, Hunter::DropFacts& f) { f.held = true; }, Why::Held },
        { "the game behind", [](HunterConfig&, Hunter::DropFacts& f) { f.focused = false; }, Why::Unfocused },
        { "pick-ups owed", [](HunterConfig&, Hunter::DropFacts& f) { f.loot_owed = true; }, Why::Loot },
        { "the veto", [](HunterConfig&, Hunter::DropFacts& f) { f.veto = "HP low"; }, Why::Veto },
        { "no map trusted", [](HunterConfig&, Hunter::DropFacts& f) { f.page = nullptr; }, Why::MapUnknown },
        { "4 px off the floor at the press", at(2675, 367), Why::OffFloor },
        { "the bottom ground", [](HunterConfig&, Hunter::DropFacts& f) {
              f.fix.x = f.fix.rx = 3100.0f;
              f.fix.y = f.fix.ry = 1119.0f;
              f.target_wx = 3100.0f;
              f.target_wy = 1300.0f;
          }, Why::Bottom },
        { "the landing another floor 8 px to the side", at(2659, 371), Why::NoLanding },
        { "walking as a settle begins: another floor 24 px to the side",
          [](HunterConfig&, Hunter::DropFacts& f) {
              f.press = false;
              f.fix.moving = true;
          }, Why::NoLanding },
        // Its last report there too: as a settle begins the height is the
        // report's floor followed (2026.11.04d), and here there is none. Under
        // kDropMinFallPx -- 25 since 2026.11.12, 60 until then -- a landing
        // is not told from standing still.
        { "the floor below 20 px down, as a settle begins",
          [](HunterConfig&, Hunter::DropFacts& f) {
              f.press = false;
              f.fix.y = f.fix.ry = 352.0f;
          }, Why::ShortFall },
        // The monster's point 113 px over the landing, no floor within 100 px
        // under it: its point is all there is, and the landing past it.
        { "the landing 113 px past the monster, on no floor", [](HunterConfig&, Hunter::DropFacts& f) {
              f.target_wy = 520.0f;
          }, Why::PastMonster },
        { "a gate in the fall", [](HunterConfig&, Hunter::DropFacts& f) { f.gates.push_back({ 2700, 500, 0 }); },
          Why::Gate },
        { "a gate where it lands", [](HunterConfig&, Hunter::DropFacts& f) { f.gates.push_back({ 2690, 690, 0 }); },
          Why::Gate },
        { "a no-drop point", [](HunterConfig&, Hunter::DropFacts& f) { f.no_drop_here = true; }, Why::NoDropHere },
    };
    for (const Case& k : cases) {
        HunterConfig c = cfg;
        Hunter::DropFacts f = base;
        k.change(c, f);
        const Hunter::DropCall call = Hunter::drop_rule(c, f);
        check(call.why == k.why, fmt("%s: %s (got %s)", k.what, Hunter::drop_why_name(k.why),
                                     Hunter::drop_why_name(call.why)));
    }

    // And what each of those lets through.
    const std::vector<Case> goes = {
        { "61 px below: past the band's 60 -- drop_min_px's 100 gone (2026.11.12)",
          [](HunterConfig&, Hunter::DropFacts& f) { f.dy = 61; }, Why::None },
        { "40 px below with down() 20", [](HunterConfig& c, Hunter::DropFacts& f) { c.down_reach = 20; f.dy = 40; },
          Why::None },
        { "the floor below 52 px down, as a settle begins (60 px at least until 2026.11.12)",
          [](HunterConfig&, Hunter::DropFacts& f) {
              f.press = false;
              f.fix.y = f.fix.ry = 320.0f;
          }, Why::None },
        { "140 px across at the press", [](HunterConfig&, Hunter::DropFacts& f) { f.dx = 140; }, Why::None },
        { "120 px across as a settle begins", [](HunterConfig&, Hunter::DropFacts& f) { f.press = false; f.dx = 120; },
          Why::None },
        { "no walk held as the settle began: no stop to wait for",
          [](HunterConfig&, Hunter::DropFacts& f) {
              f.walk_held = false;
              f.fix.report_ms = f.settle_from_ms - 500;
          }, Why::None },
        { "the left key down as a settle begins", [](HunterConfig&, Hunter::DropFacts& f) {
              f.press = false;
              f.left_held = true;
          }, Why::None },
        { "a map entry 5 s ago", [](HunterConfig&, Hunter::DropFacts& f) { f.entry_ms = f.now - 5000; }, Why::None },
        { "the last press 1.5 s ago", [](HunterConfig&, Hunter::DropFacts& f) { f.last_press_ms = f.now - 1500; },
          Why::None },
        { "3 this minute, 1 for it", [](HunterConfig&, Hunter::DropFacts& f) {
              f.presses_minute = 3;
              f.presses_target = 1;
          }, Why::None },
        { "the landing 12 px past the monster", [](HunterConfig&, Hunter::DropFacts& f) { f.target_wy = 621.0f; },
          Why::None },
        // 2026.11.04d: a monster's point is often in the air -- its floor is
        // the first under it within 100 px, the landing's.
        { "the monster 13 px over the landing's floor, in the air", [](HunterConfig&, Hunter::DropFacts& f) {
              f.target_wy = 620.0f;
          }, Why::None },
        { "the monster 90 px over it", [](HunterConfig&, Hunter::DropFacts& f) { f.target_wy = 543.0f; }, Why::None },
        { "as a settle begins, the fix 52 px over the floor its report stood on: the floor's height",
          [](HunterConfig&, Hunter::DropFacts& f) {
              f.press = false;
              f.fix.y = 320.0f;
          }, Why::None },
        { "a gate past the landing's band",
          [](HunterConfig&, Hunter::DropFacts& f) { f.gates.push_back({ 2690, 700, 0 }); }, Why::None },
        { "a gate above the standing point's band", [](HunterConfig&, Hunter::DropFacts& f) {
              f.gates.push_back({ 2690, 300, 0 });
          }, Why::None },
        { "the jump key Up, no Ctrl", [](HunterConfig& c, Hunter::DropFacts&) { c.jump_key = 0x26; }, Why::None },
    };
    for (const Case& k : goes) {
        HunterConfig c = cfg;
        Hunter::DropFacts f = base;
        k.change(c, f);
        const Hunter::DropCall call = Hunter::drop_rule(c, f);
        check(call.ok(), fmt("%s: it goes (got %s)", k.what, Hunter::drop_why_name(call.why)));
    }

    // The player's answer 2: with the hunting spot on, only inside its area --
    // where it stands, where it lands and the target.
    {
        HunterConfig c = cfg;
        c.spot = true;
        Hunter::DropFacts f = base;
        f.spot_use = true;
        f.area.set = true;
        f.area.set_half(2675.0f, 371.0f, 300, 300);
        check(Hunter::drop_rule(c, f).ok(), "the spot in use, its area 300 px either way: it goes");
        f.area.set_half(2675.0f, 371.0f, 300, 100);
        check(Hunter::drop_rule(c, f).why == Why::OutsideArea, "100 px up and down: the monster outside it -- refused");
        f.area.set_half(2400.0f, 371.0f, 200, 300);
        check(Hunter::drop_rule(c, f).why == Why::OutsideArea, "standing outside its width -- refused");
        f.area.set_half(2675.0f, 371.0f, 300, 256);
        f.target_wy = 625.0f;
        const Hunter::DropCall land_out = Hunter::drop_rule(c, f);
        check(land_out.why == Why::OutsideArea && land_out.land_y == 633,
              "the monster inside it 254 px down, the landing on 633 outside it: refused");
        f.spot_use = false;
        check(Hunter::drop_rule(c, f).why == Why::Spot, "the spot on but standing for want of a fix: refused");
    }

    // A hunting area drawn on the map tab is its own box, not even round its
    // home: one running 50 px up and 300 down holds a 135-px drop from the
    // small floor up right (238) to the long floor (372) -- one the spot's
    // +-100 round where the character stands refuses.
    {
        HunterConfig c = cfg;
        c.spot = true;
        Hunter::DropFacts f = base;
        f.fix.x = f.fix.rx = 2513.0f;
        f.fix.y = f.fix.ry = 237.0f;
        f.target_wx = 2520.0f;
        f.target_wy = 371.0f;
        f.dx = 7;
        f.dy = 134;
        f.spot_use = true;
        f.area.set = true;
        f.area.set_half(2513.0f, 237.0f, 300, 100);
        const Hunter::DropCall even = Hunter::drop_rule(c, f);
        check(even.why == Why::OutsideArea && even.land_y == 372,
              fmt("the spot's +-100 round 2513,237: the landing on 372, 135 px down, outside it -- refused (%s)",
                  Hunter::drop_why_name(even.why)));
        f.area.from_map = true;
        f.area.stem = "t52_s2";
        f.area.top = 187.0f;
        f.area.bottom = 537.0f;
        const Hunter::DropCall tall = Hunter::drop_rule(c, f);
        check(tall.ok() && tall.land_y == 372 && tall.direct && tall.stand_y == 238,
              fmt("the map tab's area, 50 px up and 300 down: it goes, 238 to 372 (%s)",
                  Hunter::drop_why_name(tall.why)));
        f.area.bottom = 360.0f;
        check(Hunter::drop_rule(c, f).why == Why::OutsideArea, "its bottom edge above the landing: refused");
    }

    // Rule 5's helper: one on the level that may be attacked, from the packets.
    {
        HunterConfig c = cfg;
        const Nameplate me = mob_at(2675, 371, -1);
        std::vector<Nameplate> mobs = { mob_at(2673, 632, 5), mob_at(2900, 371, 6) };
        std::vector<Hunter::Listed> listed(2, Hunter::Listed::Free);
        check(Hunter::level_monster(c, mobs, me, listed), "a monster on the level: one to fight there");
        listed[1] = Hunter::Listed::Skip;
        check(!Hunter::level_monster(c, mobs, me, listed), "one the lists rule out is none");
        listed[1] = Hunter::Listed::Free;
        mobs[1].hp = 0.01f;
        check(!Hunter::level_monster(c, mobs, me, listed), "nor one finished");
        mobs[1].hp = 1.0f;
        mobs[1].slot = -1;
        check(!Hunter::level_monster(c, mobs, me, listed), "nor a screen plate");
        mobs[1].slot = 6;
        listed[0] = Hunter::Listed::First;
        check(Hunter::level_monster(c, mobs, me, listed),
              "the one below on Đánh trước, the one on the level not: the level's all the same -- Đánh trước is "
              "first among those on its level");
    }

    // choose_target with nothing on the level (the player's answer 1): with a
    // drop able to go, the one below before one above that the plates'
    // measure -- height three times over -- puts nearer.
    {
        HunterConfig c = cfg;
        const Nameplate me = mob_at(2675, 371, -1);
        std::vector<Nameplate> mobs = { mob_at(2755, 241, 6), mob_at(2673, 632, 5) };
        MobDetector::nearest_first(mobs, me.x(), me.y());
        const std::vector<Hunter::Listed> free2(2, Hunter::Listed::Free);
        const int above = mobs[0].slot == 6 ? 0 : 1;
        check(above == 0 && Hunter::choose_target(c, mobs, &me, free2) == 0,
              "80 px across and 130 up comes first by the plates' measure, and is the pick as ever without a drop");
        check(Hunter::choose_target(c, mobs, &me, free2, nullptr, true) == 1,
              "with a drop able to go: the one 261 px below is the target");
        std::vector<Nameplate> level = mobs;
        level.push_back(mob_at(2300, 371, 7));
        const std::vector<Hunter::Listed> free3(3, Hunter::Listed::Free);
        check(Hunter::choose_target(c, level, &me, free3, nullptr, true) == 2,
              "one on the level as well: that one, below_first or not");
        // The lower bar alone (2026.11.12): within it on the level, past it
        // the one below -- drop_min_px's 100 held one 90 px down back.
        std::vector<Nameplate> shallow = mobs;
        shallow[1].bar.y = 371 + 60;
        shallow[1].plate.y = shallow[1].bar.y - 14;
        check(Hunter::choose_target(c, shallow, &me, free2, nullptr, true) == 1 &&
                  Hunter::choose_target(c, shallow, &me, free2, nullptr, false) == 1,
              "60 px below, the lower bar at 60: on the level -- fought where it stands, a drop able to go or not");
        std::vector<Nameplate> past = mobs;
        past[1].bar.y = 371 + 90;
        past[1].plate.y = past[1].bar.y - 14;
        check(Hunter::choose_target(c, past, &me, free2, nullptr, true) == 1 &&
                  Hunter::choose_target(c, past, &me, free2, nullptr, false) == 0,
              "90 px below, past the lower bar: the one below with a drop able to go, the nearest without");
        std::vector<Nameplate> screen = mobs;
        screen[1].slot = -1;
        check(Hunter::choose_target(c, screen, &me, free2, nullptr, true) == 0,
              "a screen plate below is never dropped to: the nearest, as ever");
        std::vector<Hunter::Listed> skip = free2;
        skip[1] = Hunter::Listed::Skip;
        check(Hunter::choose_target(c, mobs, &me, skip, nullptr, true) == 0, "nor one the lists refuse");
        check(Hunter::choose_target(c, mobs, nullptr, free2, nullptr, true) == 0,
              "where the character stands not known: the nearest, as ever");
    }
}

// ------------------------------------------------------------- the pretend game

/// What the pretend Ctrl + ↓ does in the game.
enum class Fall {
    Floor,          ///< drops to the first floor below, as the file says
    Solid,          ///< nothing: no take-off, no landing
    JumpInPlace,    ///< takes off and lands where it stood
    Rise,           ///< takes off and lands 120 px higher
    Overshoot,      ///< lands 300 px below where the file says
    Gate,           ///< the map changes: a new entry, a view change, map 53/2
    LateGate,       ///< nothing; then the map changes 2.5 s later
    /// No take-off; 700 ms after Down a report on the ground at slide_x,
    /// slide_y -- a skill's move during the press (t23_s14, 22:22:00).
    Slide,
    Past,           ///< drops past the file's first floor below onto the next
    /// As Floor, with a report at y 0 on the ground 100 ms after Down.
    Zero,
    /// As Floor, with a report on the ground in mid-air -- on no floor of the
    /// file, 110 px under the take-off -- 100 ms after Down: a hit's state 0 at
    /// the top of an arc (the owner's 00:27:54, as a jump up's), the landing on
    /// the floor the file says coming on.
    Apex,
    /// As Apex, with no landing after it: reported there, and stays.
    ApexStays,
};

/// The pretend game, in the packets' world pixels: the plates are laid one
/// to one over them.
struct DropWorld {
    std::uint64_t t = 5'000'000;
    bool          focused = true;
    int           me_x = 2675, me_y = 371;
    std::vector<Nameplate> mobs;
    /// Monsters' frames beyond the plates' box: standing points only, once
    /// two of a monster's agree (MapTrust). And whether the plates' own
    /// monsters' frames go too -- all of one time, stand_ms: one frame each
    /// until it moves.
    std::vector<MapTrust::Mob> far_stands;
    std::uint64_t stand_ms = 4'990'000;
    bool          mobs_stand = true;
    int           step_px = 0;          ///< walked per pass while a direction is held

    std::set<std::uint32_t> held;
    struct Key {
        std::uint64_t at;
        std::uint32_t vk;
        int           hold;             ///< -1: put down and left down
    };
    std::vector<Key>           keys;
    std::vector<std::uint32_t> ups;

    // what the packets say
    WorldFix::State state = WorldFix::State::Known;
    bool          airborne = false;
    float         report_x = 0.0f, report_y = 0.0f;
    std::uint64_t report_ms = 0;
    bool          map_known = true;
    std::uint16_t map = 52, area = 2;
    std::uint32_t epoch = 7;
    float         entry_x = 2812.0f, entry_y = 622.0f;
    std::uint32_t view = 1;
    std::uint64_t takeoffs = 40;
    int           wheres = 0;

    // the drop's key
    Fall          mode = Fall::Floor;
    int           drop_calls = 0;
    int           aborted = 0;
    std::vector<std::uint64_t> drops;       ///< when ↓ went down
    std::uint32_t held_ms = 90;             ///< how long ↓ is held, as the press says
    bool          not_sent = false;         ///< every press: NotSent, nothing sent at all
    bool          hold_at_check = false;    ///< a hold begins between Ctrl and ↓: once
    bool          owed = false;             ///< settle_owed says an up is still owed
    const GameMapBook::Page* page = nullptr;
    bool          falling = false;
    std::uint64_t land_at = 0;
    int           land_y = 0;
    std::uint64_t landed_at = 0;
    std::uint64_t late_gate_at = 0;
    int           slide_x = 0, slide_y = 0;
    std::uint64_t slide_at = 0;
    std::uint64_t zero_at = 0;
    std::uint64_t apex_at = 0;

    void report() {
        report_x = static_cast<float>(me_x);
        report_y = static_cast<float>(me_y);
        report_ms = t;
    }
    void place(int x, int y) {
        me_x = x;
        me_y = y;
        report();
    }
    bool walking() const { return held.count(VK_LEFT) != 0 || held.count(VK_RIGHT) != 0; }
    void change_map() {
        ++epoch;
        ++view;
        map = 53;
        area = 2;
        entry_x = 2635.0f;
        entry_y = 1188.0f;
        place(2635, 1188);
    }
    /// Keys that went down -- a press, or put down -- of `vk`, from `from` to `to`.
    int downs_of(std::uint32_t vk, std::uint64_t from = 0, std::uint64_t to = UINT64_MAX) const {
        int n = 0;
        for (const Key& k : keys)
            if (k.vk == vk && k.at >= from && k.at <= to) ++n;
        return n;
    }
};

/// A hunter on the pretend game, over the made-up files, started without a
/// thread: every seam its own, nothing reaching the desktop.
struct DropRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MobDetector          detector;
    Hunter               hunter{ input, coord, detector };
    GameMapBook          book;
    DropWorld            w;
    const char*          veto = nullptr;

    /// `data`: another folder's files -- the game's own t23_s14 (real_folder)
    /// -- and `stem` the map the pretend game is on; the made-up t52_s2 else.
    explicit DropRig(const HunterConfig& cfg, bool wired = true, const std::string& data = std::string(),
                     const char* stem = "t52_s2")
        : book(data.empty() ? fixture_folder() : data) {
        w.page = book.get(stem).get();
        Hunter::Eyes eyes;
        eyes.view = [this] { return w.view; };
        eyes.where = [this](WorldFix& f) {
            ++w.wheres;
            f = WorldFix{};
            f.state = w.state;
            f.x = static_cast<float>(w.me_x);
            f.y = static_cast<float>(w.me_y);
            f.rx = w.report_x;
            f.ry = w.report_y;
            f.report_ms = w.report_ms;
            f.airborne = w.airborne;
            // A walk that moves nothing (step_px 0) leaves the character
            // standing where it is, as the scenario says it stays.
            f.moving = w.walking() && w.step_px > 0;
            f.map_known = w.map_known;
            f.map = w.map;
            f.area = w.area;
            f.epoch = w.epoch;
            f.entry_x = w.entry_x;
            f.entry_y = w.entry_y;
            return true;
        };
        eyes.player = [this](Nameplate& out, bool* fresh) {
            if (fresh) *fresh = true;
            out = mob_at(w.me_x, w.me_y, -1);
            out.is_player = true;
            out.is_self = true;
            return true;
        };
        eyes.mobs = [this] {
            std::vector<Nameplate> out = w.mobs;
            MobDetector::nearest_first(out, w.me_x, w.me_y);
            return out;
        };
        eyes.takeoffs = [this]() -> std::optional<std::uint64_t> { return w.takeoffs; };
        eyes.stands = [this] {
            std::vector<MapTrust::Mob> out;
            for (const Nameplate& n : w.mobs)
                if (n.slot >= 0 && w.mobs_stand)
                    out.push_back(MapTrust::Mob{ MapPoint{ n.x(), n.y() }, static_cast<std::uint32_t>(n.slot),
                                                 w.stand_ms, n.gen });
            for (const MapTrust::Mob& m : w.far_stands) out.push_back(m);
            return out;
        };
        hunter.set_eyes(eyes);

        Hunter::Hands hands;
        hands.focused = [this] { return w.focused; };
        hands.key = [this](std::uint32_t vk, int hold) {
            w.keys.push_back({ w.t, vk, hold });
            return true;
        };
        hands.key_down = [this](std::uint32_t vk) {
            w.keys.push_back({ w.t, vk, -1 });
            w.held.insert(vk);
            if (vk == VK_LEFT || vk == VK_RIGHT) w.report();
            return true;
        };
        hands.key_up = [this](std::uint32_t vk) {
            w.ups.push_back(vk);
            if (w.held.erase(vk) != 0 && (vk == VK_LEFT || vk == VK_RIGHT)) w.report();
        };
        hands.held = [this](std::uint32_t vk) { return w.held.count(vk) != 0; };
        hands.release_all = [this] {
            if (w.walking()) w.report();
            w.held.clear();
        };
        hands.settle_owed = [this] { return !w.owed; };
        // The pretend Ctrl + ↓: asked before Ctrl, again before ↓, as
        // InputManager::drop_ctrl_down asks; its time the real press's.
        hands.drop = [this](const std::function<bool()>& go_on) {
            InputManager::DropPress p;
            ++w.drop_calls;
            if (w.not_sent || !go_on()) return p;
            w.t += 50;                          // Ctrl alone
            if (w.hold_at_check) {
                w.hold_at_check = false;
                coord.engage_critical("a hold between Ctrl and Down");
            }
            if (!go_on()) {
                w.t += 5;
                ++w.aborted;
                p.sent = InputManager::DropSent::Aborted;
                return p;
            }
            p.down_ms = w.t;
            w.drops.push_back(w.t);
            w.t += w.held_ms;                   // ↓ held
            p.held_ms = w.held_ms;
            w.t += 50;                          // then Ctrl up
            p.sent = InputManager::DropSent::Sent;
            const std::uint64_t down = p.down_ms;
            const auto fall_to = [&](int y) {
                ++w.takeoffs;
                w.airborne = true;
                w.report_x = static_cast<float>(w.me_x);
                w.report_y = static_cast<float>(w.me_y);
                w.report_ms = down + 30;
                w.falling = true;
                w.land_y = y;
                // GoC's fall: sqrt(d / 1200) s for d px.
                const double d = std::max(1, std::abs(y - w.me_y));
                w.land_at = down + static_cast<std::uint64_t>(std::sqrt(d / 1200.0) * 1000.0);
            };
            const std::optional<MapFloor> below = w.page ? w.page->floor_below(w.me_x, w.me_y, 8) : std::nullopt;
            switch (w.mode) {
                case Fall::Floor:       if (below) fall_to(below->y - 1); break;
                case Fall::Solid:       break;
                case Fall::JumpInPlace: fall_to(w.me_y); break;
                case Fall::Rise:        fall_to(w.me_y - 120); break;
                case Fall::Overshoot:   if (below) fall_to(below->y - 1 + 300); break;
                case Fall::Gate:        w.change_map(); break;
                case Fall::LateGate:    w.late_gate_at = down + 2500; break;
                case Fall::Slide:       w.slide_at = down + 700; break;
                case Fall::Past:
                    if (below) {
                        const std::optional<MapFloor> next = w.page->floor_below(w.me_x, below->y, 8);
                        fall_to((next ? next->y : below->y) - 1);
                    }
                    break;
                case Fall::Zero:
                    if (below) fall_to(below->y - 1);
                    w.zero_at = down + 100;
                    break;
                case Fall::Apex:
                    if (below) fall_to(below->y - 1);
                    w.apex_at = down + 100;
                    break;
                case Fall::ApexStays:
                    ++w.takeoffs;
                    w.apex_at = down + 100;
                    break;
            }
            return p;
        };
        hunter.set_hands(hands);

        Hunter::Clock clock;
        clock.now_ms = [this] { return w.t; };
        clock.sleep_ms = [this](std::uint32_t ms) { w.t += ms; };
        hunter.set_clock(clock);

        if (wired) {
            hunter.set_map_book(&book);
            hunter.set_drop_veto([this] { return veto; });
        }
        hunter.set_config(cfg);
        w.report();
        hunter.start_stepped();
    }
    ~DropRig() { hunter.stop(); }

    void pass() {
        hunter.step();
        if (w.step_px > 0 && !w.falling) {
            if (w.held.count(VK_LEFT)) w.me_x -= w.step_px;
            if (w.held.count(VK_RIGHT)) w.me_x += w.step_px;
        }
        w.t += 60;
        if (w.falling && w.t >= w.land_at) {
            w.falling = false;
            w.airborne = false;
            w.me_y = w.land_y;
            w.landed_at = w.t;
            w.report();
        }
        if (w.late_gate_at != 0 && w.t >= w.late_gate_at) {
            w.late_gate_at = 0;
            w.change_map();
        }
        if (w.slide_at != 0 && w.t >= w.slide_at) {
            w.slide_at = 0;
            w.place(w.slide_x, w.slide_y);
        }
        if (w.zero_at != 0 && w.t >= w.zero_at) {
            // On the ground by its frame, at y 0: no place.
            w.zero_at = 0;
            w.airborne = false;
            w.report_x = static_cast<float>(w.me_x);
            w.report_y = 0.0f;
            w.report_ms = w.t;
        }
        if (w.apex_at != 0 && w.t >= w.apex_at) {
            // On the ground by its frame, in mid-air: a hit's, at the top of
            // an arc -- no floor of the file under it.
            w.apex_at = 0;
            w.airborne = false;
            w.report_x = static_cast<float>(w.me_x);
            w.report_y = static_cast<float>(w.me_y + 110);
            w.report_ms = w.t;
        }
    }
    void run(std::uint64_t ms, const std::function<void()>& each = nullptr) {
        for (const std::uint64_t until = w.t + ms; w.t < until;) {
            pass();
            if (each) each();
        }
    }
    /// Passes until `done`, for `ms` at most: whether it came.
    bool until(const std::function<bool()>& done, std::uint64_t ms) {
        for (const std::uint64_t end = w.t + ms; w.t < end;) {
            pass();
            if (done()) return true;
        }
        return false;
    }
    /// The map entry 5 s old, nothing in view meanwhile: drops may go.
    void age_entry() { run(Hunter::kDropAfterEntryMs + 200); }
    Hunter::Status st() const { return hunter.status(); }
};

HunterConfig drop_config(int gate_px = 120) { return rule_config(gate_px); }

// ------------------------------------------------------------- scenarios

void test_drop_owner_cases() {
    section("Hunter: the drop on the pretend game -- the player's 06:48:49 and 06:48:26 on t52_s2");
    if (fixture_folder().empty()) return;

    // 06:48:49, as it stood: the 전투존 gate 137 px across at 150 -- never.
    {
        Hunter::Status st;
        int drops = -1;
        const std::vector<std::string> log = info_lines([&] {
            DropRig r(drop_config(150));
            r.age_entry();
            r.w.mobs = { mob_at(2673, 632, 5) };
            r.run(9000);
            st = r.st();
            drops = r.w.drop_calls;
        });
        check(drops == 0 && st.drops == 0 && st.drop_wait == Why::Gate && st.drop_wait_px == 137,
              fmt("06:48:49 at drop_gate_px 150: no press in 9 s -- the gate 137 px across (wait %s, %d px)",
                  Hunter::drop_why_name(st.drop_wait), st.drop_wait_px));
        check(lines_with(log, "not dropping to the monster 261 px below -- a gate is near (137 px across; "
                              "drop_gate_px is 150)") == 1,
              "said once, at INFO: \"" + line_with(log, "not dropping") + "\"");
        check(st.drop_map.why == MapTrust::Why::Trusted && st.drop_map.stem == "t52_s2" &&
                  st.drop_map.how == MapTrust::How::Entry,
              "the map trusted all along: t52_s2, its entry by a gate");
    }

    // At 136 it drops, lands on the monster's floor, and fights it.
    {
        DropRig r(drop_config(136));
        std::uint64_t settle_at = 0;
        std::vector<std::string> log = info_lines([&] {
            r.age_entry();
            r.w.mobs = { mob_at(2673, 632, 5) };
            // The pass the settle began in: its keys went at the time before
            // the pass's 60 ms.
            r.run(4000, [&] {
                if (settle_at == 0 && r.st().dropping) settle_at = r.w.t - 60;
            });
        });
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && r.w.me_y == 632 && st.drops == 1 && st.drops_worked == 1,
              fmt("at 136: one Ctrl + Down (%zu), the character on 632, the monster's floor -- drops %llu, worked %llu",
                  r.w.drops.size(), static_cast<unsigned long long>(st.drops),
                  static_cast<unsigned long long>(st.drops_worked)));
        check(r.w.downs_of('Z', r.w.landed_at) > 0 && st.state == Hunter::State::Attacking,
              "then attacked it where it landed");
        const std::uint64_t down = r.w.drops.empty() ? 0 : r.w.drops.front();
        check(down >= 5'000'000 + Hunter::kDropAfterEntryMs, "not before the map entry was 5 s old");
        // From the settle's first pass to the landing: no walk, no jump, and
        // no turn -- nothing of the drop's turns the character.
        int taps = 0;
        std::string seen;
        for (const DropWorld::Key& k : r.w.keys) {
            if (k.at < settle_at || k.at > r.w.landed_at) continue;
            if (k.hold == Hunter::kTurnTapMs && (k.vk == VK_LEFT || k.vk == VK_RIGHT)) ++taps;
            seen += fmt(" %u@%llu/%d", k.vk, static_cast<unsigned long long>(k.at - settle_at), k.hold);
        }
        check(settle_at != 0 && settle_at < down && r.w.downs_of(VK_LEFT, settle_at, r.w.landed_at) == 0 &&
                  r.w.downs_of(VK_RIGHT, settle_at, r.w.landed_at) == 0 &&
                  r.w.downs_of(38, settle_at, r.w.landed_at) == 0 && taps == 0,
              fmt("no direction key, no jump key and no turn tap from the settle to the landing (settle %llu ms "
                  "before Down; keys:%s)",
                  static_cast<unsigned long long>(down - settle_at), seen.empty() ? " none" : seen.c_str()));
        check(r.w.downs_of(VK_DOWN) == 0 && r.w.downs_of(VK_CONTROL) == 0,
              "and neither Down nor Ctrl through any other key of the hunter's");
        const std::string press = line_with(log, "Hunter: drop (Ctrl+Down) toward ");
        check(press.find("2 px left, 261 px down -- you at 2675,371 on floor 372 of t52_s2 (trusted: map entry by a "
                         "gate), landing on floor 633 (the monster's), nearest gate 137 px; nothing on your level in "
                         "view; 1st for it, 1 this minute; Down held 90 ms") != std::string::npos,
              "the press's line: \"" + press + "\"");
        check(lines_with(log, "Hunter: drop worked: landed at 2675,632, 261 px down") == 1,
              "and the verdict's: \"" + line_with(log, "drop worked") + "\"");
        // The minute line, a minute on.
        const std::vector<std::string> more = info_lines([&] { r.run(55'000); });
        const std::string minute = line_with(more, " fights, ");
        const std::size_t given = minute.find(" given up");
        check(minute.find("; drops 1 (1 worked, 0 did nothing, 0 jumps in place, 0 rose, 0 off the map, 0 on another "
                          "floor, 0 map changes, 0 unknown)") != std::string::npos,
              "the minute line: \"" + (given == std::string::npos ? minute : minute.substr(given)) + "\"");
        const Hunter::Status after = r.st();
        check(after.targets_dropped == after.targets_repicked,
              "and nothing of the drop among the given up: every give-up a re-pick in reach, as ever");
    }

    // The monster 85 px off to the right on the floor below: from the landing
    // on, the turns are those of a character that stood there with drops off
    // -- the in-reach branch's, nothing of the drop's.
    {
        const auto turn_taps = [](const DropWorld& w, std::uint64_t from, std::uint64_t to) {
            int n = 0;
            for (const DropWorld::Key& k : w.keys)
                if (k.at >= from && k.at < to && k.hold == Hunter::kTurnTapMs && (k.vk == VK_LEFT || k.vk == VK_RIGHT))
                    ++n;
            return n;
        };
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mobs = { mob_at(2760, 632, 5) };
        r.until([&] { return r.w.landed_at != 0; }, 4000);
        const std::uint64_t landed = r.w.landed_at;
        const int before = turn_taps(r.w, 0, landed == 0 ? UINT64_MAX : landed);
        r.run(3000);
        const int dropped = turn_taps(r.w, landed, UINT64_MAX);

        HunterConfig off = drop_config(136);
        off.drop_down = false;
        DropRig s(off);
        s.w.place(2675, 632);
        s.w.mobs = { mob_at(2760, 632, 5) };
        const std::uint64_t from = s.w.t;
        s.run(3000);
        const int stood = turn_taps(s.w, from, UINT64_MAX);
        check(landed != 0 && r.w.drops.size() == 1 && r.w.me_y == 632 && before == 0 && dropped == stood &&
                  stood >= 1,
              fmt("landed with the monster 85 px to the right: %d turn tap(s) after, as %d standing there with drops "
                  "off; none before the landing (%d)",
                  dropped, stood, before));
    }

    // From the right with drop_gate_px 120: refused while the gate is within
    // 120 px, dropped at the first stop past it.
    {
        DropRig r(drop_config(120));
        r.w.step_px = 10;
        r.w.place(2780, 371);
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(5000);
        const int x0 = r.w.drops.empty() ? -1 : static_cast<int>(r.w.report_x);
        check(r.w.drops.size() == 1 && r.w.me_y == 632 && std::abs(2812 - r.w.me_x) > 120 && r.w.me_x <= 2700,
              fmt("from the right, 2780: walked left past the gate's 120 px, one drop at %d, on 632 (%d)", r.w.me_x,
                  x0));
    }
    // From the left: past the floors that would land past the monster and
    // the edge of its floor -- walking, 24 px either side -- until it stands
    // above the monster (the owner's Q6 of 04d, kDropStopAbovePx), and judged
    // there settled, 8 px either side.
    {
        DropRig r(drop_config(120));
        r.w.step_px = 10;
        r.w.place(2560, 371);
        r.age_entry();
        r.w.mobs = { mob_at(2690, 632, 5) };
        r.run(5000);
        check(r.w.drops.size() == 1 && r.w.me_y == 632 && r.w.me_x >= 2658 + Hunter::kDropMarginPressPx &&
                  std::abs(r.w.me_x - 2690) <= Hunter::kDropStopAbovePx && r.w.me_x < 2812 - 120,
              fmt("from the left, 2560: stood above the monster, one drop at %d, on 632 -- never over the floor 984 "
                  "below",
                  r.w.me_x));
    }

    // 06:48:26: from the small floor up right to the long floor, a moment,
    // then to the small floor by the monster -- and no third. The monster on
    // that small floor, where the made-up file has it end.
    {
        DropRig r(drop_config(150));
        r.w.place(2513, 237);
        r.age_entry();
        r.w.mobs = { mob_at(2530, 627, 7) };
        r.run(8000);
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 2 && r.w.me_y == 627 && st.drops == 2 && st.drops_worked == 2,
              fmt("06:48:26: two drops (%zu), 237 -> 371 -> 627, the monster's floor", r.w.drops.size()));
        check(r.w.drops.size() == 2 && r.w.drops[1] - r.w.drops[0] >= Hunter::kDropGapMs,
              "the second no sooner than 1.5 s after the first, stood for where the first landed");
        check(r.w.downs_of('Z') > 0 && r.w.drop_calls == 2, "then it fought, and pressed no third");
    }

    // 2026-11-04 07:56-08:02, the player's first hunt with drops on: map
    // entry 52/2 at 880,274, by the 피스존 gate 881,267 -- and 61 ms after the
    // file was trusted, a monster's frame at 1929,819, 31 px above floor 850,
    // took it away for the stay: four monsters below, never dropped to. Now
    // the frame is said and ignored, the monster lands and stands on 850, and
    // the drop goes.
    {
        DropRig r(drop_config(136));
        r.w.entry_x = 880.0f;
        r.w.entry_y = 274.0f;
        const std::vector<std::string> log = info_lines([&] {
            r.w.far_stands = { MapTrust::Mob{ MapPoint{ 1929, 819 }, 30u, r.w.t, 1u } };
            r.run(400);
            r.w.far_stands = { MapTrust::Mob{ MapPoint{ 1929, 849 }, 30u, r.w.t, 1u } };
            r.run(2000);
            r.w.far_stands = { MapTrust::Mob{ MapPoint{ 1929, 849 }, 30u, r.w.t, 1u } };
            r.age_entry();
            r.w.mobs = { mob_at(2673, 632, 5) };
            r.run(4000);
        });
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && r.w.me_y == 632 && st.drops_worked == 1 &&
                  st.drop_map.why == MapTrust::Why::Trusted && st.drop_map.how == MapTrust::How::Entry &&
                  st.drop_map.mob_off == 1,
              fmt("07:59:20's monster frame at 1929,819, then on floor 850: the entry by the 피스존 gate trusted all "
                  "along (%s), and the drop to the monster 261 px below went (%zu, on %d)",
                  MapTrust::why_text(st.drop_map.why), r.w.drops.size(), r.w.me_y));
        check(lines_with(log, "monster frame(s) off t52_s2's floors ignored",
                         "slot 30 at 1929,819, 31 px above floor 850 (attr 1)") == 1 &&
                  lines_with(log, "does not match") == 0 && lines_with(log, "the map is not known") == 0,
              "said once, with the floor under it -- no \"does not match\", no \"map is not known\": " +
                  line_with(log, "monster frame(s)"));
    }
}

void test_drop_choosing() {
    section("Hunter: the drop and the monsters on the character's own level (the player's answer 1)");
    if (fixture_folder().empty()) return;

    // Nothing on the level: the one below is the target, and dropped to.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.pass();
        const Hunter::Status st = r.st();
        check(st.has_target && st.target.slot == 5 && st.dy == 261,
              "nothing on the level: choose_target makes the one below the target");
        r.run(3000);
        check(r.w.drops.size() == 1 && r.w.me_y == 632, "and it is dropped to");
    }
    // One on the level as well: fought there, never dropped from.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5), mob_at(2350, 371, 6) };
        r.run(6000);
        const Hunter::Status st = r.st();
        check(r.w.drop_calls == 0 && st.target.slot == 6 && r.w.downs_of('Z') > 0,
              "one on the level 325 px off as well: it is fought, nothing dropped to");
    }
    // One coming onto the level while the one below waits its turn: the one
    // below is let go of -- not passed over, not given up on -- for it.
    {
        DropRig r(drop_config(136));
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(2000);
        const bool below_first = r.st().target.slot == 5;
        std::vector<std::string> log = info_lines([&] {
            r.w.mobs.push_back(mob_at(2400, 371, 6));
            r.run(5000);
        });
        const Hunter::Status st = r.st();
        check(below_first && r.w.drop_calls == 0 && st.target.slot == 6 && st.targets_dropped == st.targets_repicked,
              "the one below first, then one on the level: the target switches to it, nothing dropped, none given up "
              "out of reach");
        check(lines_with(log, "not dropping to the monster 261 px below -- a monster on your own level") == 0,
              "said at DEBUG, not INFO");
    }
    // With drop_down off, the one below is walked under as ever, and the
    // eyes asked nothing new.
    {
        HunterConfig off = drop_config(136);
        off.drop_down = false;
        DropRig r(off);
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(6000);
        check(r.w.drop_calls == 0 && r.w.wheres == 0 && r.st().drop_map.why == MapTrust::Why::NoBook,
              fmt("drop_down off: no drop, and Eyes::where never asked (%d)", r.w.wheres));
    }

    // Nothing on the level, one above that the plates' measure puts nearer --
    // 80 px across, 130 up -- and one below: the one below is the target and
    // dropped to, never hopped up after the other. The plates are no standing
    // points here: the one above stands on no floor of the made-up file.
    const auto above_and_below = [](const HunterConfig& c, bool known, int& slot, std::size_t& drops, int& me_y,
                                    int& jumps) {
        DropRig r(c);
        r.w.mobs_stand = false;
        if (!known) {
            r.w.map_known = false;
            r.w.map = r.w.area = 0;
        }
        r.age_entry();
        r.w.mobs = { mob_at(2755, 241, 6), mob_at(2673, 632, 5) };
        r.pass();
        slot = r.st().has_target ? r.st().target.slot : -1;
        r.run(3000);
        drops = r.w.drops.size();
        me_y = r.w.me_y;
        jumps = r.w.downs_of(38, 0, r.w.drops.empty() ? UINT64_MAX : r.w.drops.front());
    };
    {
        int slot = -1, me_y = 0, jumps = -1;
        std::size_t drops = 0;
        above_and_below(drop_config(136), true, slot, drops, me_y, jumps);
        check(slot == 5 && drops == 1 && me_y == 632 && jumps == 0,
              fmt("one above nearer by the plates' measure, one 261 px below: the one below is the target (%d), "
                  "dropped to (%zu, on %d), no jump before it (%d)",
                  slot, drops, me_y, jumps));
        HunterConfig off = drop_config(136);
        off.drop_down = false;
        above_and_below(off, true, slot, drops, me_y, jumps);
        check(slot == 6 && drops == 0, fmt("drop_down off: the nearest as ever, the one above (%d)", slot));
        above_and_below(drop_config(136), false, slot, drops, me_y, jumps);
        check(slot == 6 && drops == 0,
              fmt("the map not known (\"map 0/0\", not named): no drop could go -- the nearest as ever (%d)", slot));
    }
    // The same with the plates as standing points, the one above on the small
    // floor up left: still the one below.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mobs = { mob_at(2540, 238, 6), mob_at(2673, 632, 5) };
        r.pass();
        const int slot = r.st().has_target ? r.st().target.slot : -1;
        r.run(3000);
        check(slot == 5 && r.w.drops.size() == 1 && r.w.me_y == 632 &&
                  r.st().drop_map.why == MapTrust::Why::Trusted,
              fmt("one on the small floor up left, one below: the one below (%d), dropped to", slot));
    }
    // The target below on "Đánh trước", then one comes onto the level that is
    // not: let go of for the one on the level, as choose_target has it --
    // "Đánh trước" is first among those on its level -- and nothing dropped.
    {
        HunterConfig c = drop_config(136);
        c.first_names = { "Ma Truoc" };
        DropRig r(c);
        Nameplate first = mob_at(2673, 632, 5);
        first.name = "Ma Truoc";
        r.w.mobs = { first };
        r.run(2000);
        const bool picked = r.st().target.slot == 5;
        r.w.mobs.push_back(mob_at(2400, 371, 6));
        r.run(6000);
        const Hunter::Status st = r.st();
        check(picked && r.w.drop_calls == 0 && st.target.slot == 6 && r.w.downs_of('Z') > 0,
              fmt("the one below on Đánh trước, then one on the level not on it: the target switches to the level's "
                  "(%d), fought, nothing dropped (%d)",
                  st.target.slot, r.w.drop_calls));
    }
}

void test_drop_verdicts() {
    section("Hunter: what came of a drop -- did nothing, jumped in place, rose, off the map, the map changed; since "
            "2026.11.24 never a pause, an \"off\" or a stop");
    if (fixture_folder().empty()) return;

    // Ctrl + ↓ did nothing: judged at 1 s. The first of a row is no evidence
    // (2026.11.15, the game loses a share of GoBot's presses at random): the
    // place is noted, not learned, and the monster passed over only
    // kRetryPassOverMs -- the next look presses again from the same place. The
    // second near it is: the place remembered for kLearnedForgetMs, the
    // monster passed over one fight turn, kRetryFightMs (2026.11.24; 8 s, the
    // place for the session, until then) -- and not tried there meanwhile.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Solid;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.drops.empty(); }, 3000);
            r.run(1200);
        });
        Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && !r.book.no_drop_near("t52_s2", { 2675, 371 }) && st.drops == 1 &&
                  lines_with(log, "Hunter: drop did nothing in 1000 ms at 2675,371 -- not held against the place yet "
                                  "(a second near it within 120 s is); the monster passed over for 1.2 s "
                                  "(1 in a row)") == 1,
              "Solid: one press, judged Nothing at 1 s -- 2675,371 noted, not learned as a no-drop point");
        check(st.targets_dropped == 0 && st.state == Hunter::State::NoTargets && st.passed_over,
              "the monster passed over -- not counted as given up: the minute line counts it among the drops");
        // 1.2 s after the verdict it is picked again and pressed from the same
        // place: the second press that did nothing near it.
        log = info_lines([&] {
            r.until([&] { return r.w.drops.size() > 1; }, 4000);
            r.run(1200);
        });
        st = r.st();
        const std::uint64_t gap = r.w.drops.size() > 1 ? r.w.drops[1] - r.w.drops[0] : 0;
        check(r.w.drops.size() == 2 && gap >= Hunter::kDropGapMs && gap < 4000 &&
                  r.book.no_drop_near("t52_s2", { 2675, 371 }) &&
                  lines_with(log, "Hunter: drop did nothing in 1000 ms at 2675,371 -- no drops within 64 px of it for "
                                  "30 s, another column first; the monster passed over for 3 s: a fight turn, then "
                                  "tried again (2 in a row)") == 1,
              fmt("the second press %llu ms after the first (not the 8 s of before): 2675,371 learned as a no-drop "
                  "point for 30 s, the monster passed over one fight turn, 3 s",
                  static_cast<unsigned long long>(gap)));
        check(st.targets_dropped == 0 && st.state == Hunter::State::NoTargets && st.passed_over,
              "passed over, still not counted as given up");
        // Another monster below, past where the first is passed over: the
        // character, standing where it pressed, does not press there again.
        r.w.mobs = { mob_at(2730, 632, 8) };
        log = info_lines([&] { r.run(3000); });
        check(r.w.drops.size() == 2 && r.st().drop_wait == Why::NoDropHere,
              fmt("another beneath the same place: not pressed there again (%s)",
                  Hunter::drop_why_name(r.st().drop_wait)));
    }
    // Three in a row did nothing -- and a fourth place is pressed at once:
    // never a pause (2026.11.24, the owner: "khi hỏng thì thử lại luôn ... chứ
    // không phải là dừng 90s"; paused 30 s from 2026.11.15, off until the next
    // start before).
    {
        DropRig r(drop_config(40));
        r.w.mode = Fall::Solid;
        r.w.place(2600, 371);
        r.age_entry();
        const struct {
            int x, mx, slot;
        } spots[] = { { 2600, 2610, 11 }, { 2350, 2355, 12 }, { 2480, 2485, 13 }, { 2700, 2705, 14 } };
        std::vector<std::string> log;
        for (const auto& s : spots) {
            r.w.place(s.x, 371);
            r.w.mobs = { mob_at(s.mx, 983, s.slot) };
            const std::size_t before = r.w.drops.size();
            const std::vector<std::string> l =
                info_lines([&] { r.until([&] { return r.w.drops.size() > before; }, 4000); r.run(1200); });
            log.insert(log.end(), l.begin(), l.end());
        }
        check(r.w.drops.size() == 4 && r.w.drops[3] - r.w.drops[2] >= Hunter::kDropGapMs &&
                  r.w.drops[3] - r.w.drops[2] < 4000,
              fmt("four places, four presses that did nothing (%zu): the fourth %llu ms after the third -- no pause",
                  r.w.drops.size(),
                  r.w.drops.size() == 4 ? static_cast<unsigned long long>(r.w.drops[3] - r.w.drops[2]) : 0ull));
        check(lines_with(log, "did not drop -- drops paused") == 0 && lines_with(log, "OFF until the next") == 0 &&
                  lines_with(log, "Hunter: drop did nothing in 1000 ms") == 4,
              "each said as a press that did nothing; never a pause, never \"OFF\"");
    }
    // Five in a run did nothing, never three in a row -- a drop that worked
    // between each two -- and more of them failed than worked: no pause
    // (2026.11.24; a pause all the same from 2026.11.15). Each place that did
    // nothing more than 64 px from the others, a fresh monster each time --
    // and the minute's 4 presses count only the ones that moved the character
    // (the two that worked): never waited for, where the seven took over two
    // minutes before.
    {
        DropRig r(drop_config(40));
        r.age_entry();
        struct Step {
            Fall mode;
            int  x, y, mx, my;
        };
        // Did nothing: from the long floor, a monster on the low floor. Worked:
        // from the small floor up left to the long floor, its monster there.
        const Step steps[] = {
            { Fall::Solid, 2350, 371, 2355, 983 }, { Fall::Solid, 2480, 371, 2485, 983 },
            { Fall::Floor, 2400, 237, 2405, 371 }, { Fall::Solid, 2600, 371, 2605, 983 },
            { Fall::Solid, 2700, 371, 2705, 983 }, { Fall::Floor, 2400, 237, 2405, 371 },
            { Fall::Solid, 2770, 371, 2775, 983 },
        };
        std::vector<std::string> log;
        std::string got;
        int slot = 20;
        const std::uint64_t t0 = r.w.t;
        for (const Step& s : steps) {
            r.w.mode = s.mode;
            r.w.place(s.x, s.y);
            r.w.mobs = { mob_at(s.mx, s.my, ++slot) };
            const std::size_t before = r.w.drops.size();
            const std::vector<std::string> l = info_lines([&] {
                r.until([&] { return r.w.drops.size() > before; }, 75'000);
                r.run(1500);
            });
            log.insert(log.end(), l.begin(), l.end());
            got += r.w.drops.size() > before ? (s.mode == Fall::Solid ? "S" : "F") : "-";
        }
        const Hunter::Status st = r.st();
        check(got == "SSFSSFS" && st.drops_worked == 2 && r.w.t - t0 < 60'000,
              fmt("did nothing, did nothing, worked, twice more, worked, did nothing (%s) in %llu s: every press at "
                  "once",
                  got.c_str(), static_cast<unsigned long long>((r.w.t - t0) / 1000)));
        check(lines_with(log, "drops paused") == 0 && lines_with(log, "minute's used up") == 0,
              "never paused, and the minute's cap never in the way of a press after one that did nothing");
    }
    // Down held past kDropHeldMaxMs: a WARN, the press judged as any other --
    // and the next press goes (2026.11.24; off until the next start until
    // then).
    {
        DropRig r(drop_config(136));
        r.w.held_ms = 200;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        std::vector<std::string> log = info_lines([&] { r.run(3000); });
        const bool warned = r.w.drop_calls == 1 && r.st().drops_worked == 1 &&
                            lines_with(log, "Hunter: Down was held 200 ms (over 180) -- a slow PC; the press is judged "
                                            "as any other") == 1 &&
                            lines_with(log, "OFF until") == 0;
        // Back up on the long floor, a fresh monster below: pressed again.
        r.w.place(2675, 371);
        r.w.mobs = { mob_at(2673, 632, 6) };
        r.run(6000);
        check(warned && r.w.drop_calls == 2 && r.st().drops_worked == 2,
              fmt("Down held 200 ms: said, the drop judged (worked) -- and a fresh monster below dropped to again (%d "
                  "presses)",
                  r.w.drop_calls));
    }
    // A jump in place: a fail, like a press that did nothing (2026.11.15: a
    // ↓ the game lost leaves a Ctrl alone, which jumps in place) -- the place
    // learned at the second near it; the monster passed over 1.2 s at the
    // first, one fight turn at the second (2026.11.24; 8 s each until then).
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::JumpInPlace;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.drops.empty(); }, 4000);
            r.run(300);
        });
        const bool first = r.w.drops.size() == 1 && !r.book.no_drop_near("t52_s2", { 2675, 371 }) &&
                           lines_with(log, "Hunter: drop was a jump in place (landed at 2675,371) -- not held against "
                                           "the place yet (a second near it within 120 s is); the monster passed over "
                                           "for 1.2 s") == 1;
        log = info_lines([&] {
            r.until([&] { return r.w.drops.size() > 1; }, 6000);
            r.run(300);
        });
        const std::uint64_t gap = r.w.drops.size() > 1 ? r.w.drops[1] - r.w.drops[0] : 0;
        check(first && r.w.drops.size() == 2 && gap >= Hunter::kDropGapMs && gap < 4000 &&
                  r.book.no_drop_near("t52_s2", { 2675, 371 }) &&
                  lines_with(log, "Hunter: drop was a jump in place (landed at 2675,371) -- remembered as no drop for "
                                  "30 s, another column first; the monster passed over for 3 s: a fight turn, then "
                                  "tried again") == 1,
              fmt("a jump in place: the first press noted, not learned; the second, %llu ms later, the place learned "
                  "for 30 s -- drops still on",
                  static_cast<unsigned long long>(gap)));
    }
    // A rise: a failed press like any other -- the place learned at once,
    // skipped kLearnedForgetMs -- and the drops go on from another column
    // (2026.11.24; off until the next start at once until then).
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Rise;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(3000); });
        const bool rose = r.w.drops.size() == 1 && r.book.no_drop_near("t52_s2", { 2675, 371 }) &&
                          lines_with(log, "Hunter: drop ROSE to 2675,251 -- Ctrl+Down acts as a jump here; this place "
                                          "skipped 30 s, another column first; drops go on") == 1 &&
                          lines_with(log, "OFF until") == 0;
        // Back on the long floor 75 px left -- another column -- a fresh
        // monster below: dropped to.
        r.w.mode = Fall::Floor;
        r.w.place(2600, 371);
        r.w.mobs = { mob_at(2605, 983, 6) };
        r.until([&] { return r.w.drops.size() > 1; }, 5000);
        r.run(1500);
        check(rose && r.w.drops.size() == 2 && r.st().drops_worked == 1,
              fmt("a rise: one press, the place learned, said as \"%s\" -- the next column dropped from at once (%zu "
                  "presses, %llu worked)",
                  line_with(log, "drop ROSE").c_str(), r.w.drops.size(),
                  static_cast<unsigned long long>(r.st().drops_worked)));
    }
    // Off the map: the file said 633 and it landed 300 px lower. MapTrust lets
    // the file go -- never the book's distrust -- and three standing points on
    // its floors take it again, no clock; then drops go on (2026.11.24; the
    // file distrusted, drops off, until then).
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Overshoot;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.drops.empty(); }, 4000);
            r.run(2400);
            r.w.mobs.clear();
            r.run(600);
        });
        check(r.w.drops.size() == 1 && !r.book.distrusted("t52_s2") &&
                  lines_with(log, "not on floor 633 the map file gave -- t52_s2's file let go of until 3 standing "
                                  "points on its floors agree again (no clock); drops go on then") == 1 &&
                  lines_with(log, "drops OFF") == 0,
              "off the map: the file let go of until its standing points agree again -- never distrusted, no drops "
              "off");
        check(r.st().drop_map.why != MapTrust::Why::Trusted, "the map no longer trusted");
        // Three standing points of yours on its floors, apart: trusted again
        // at once -- and the drop goes.
        r.w.mode = Fall::Floor;
        r.w.mobs.clear();
        const std::vector<std::string> back = info_lines([&] {
            for (const int x : { 2350, 2450, 2600 }) {
                r.w.place(x, 371);
                r.run(700);
            }
        });
        const MapTrust::Report t = r.st().drop_map;
        r.w.mobs = { mob_at(2605, 983, 6) };
        r.until([&] { return r.w.drops.size() > 1; }, 5000);
        check(t.why == MapTrust::Why::Trusted && t.how == MapTrust::How::Points &&
                  lines_with(back, "Hunter: t52_s2 trusted by 3 standing points on its floors") == 1 &&
                  r.w.drops.size() == 2,
              fmt("three standing points on its floors: trusted again (%s), no clock -- and a drop again (%zu presses)",
                  MapTrust::why_text(t.why), r.w.drops.size()));
    }
    // The map changed: both ends learned as gates, said once for the window
    // (PressGate::Drop), and the hunt goes on where the character came --
    // nothing stopped, drop_down left ticked (2026.11.24; the hunter halted
    // for the window to stop it and untick drop_down until then).
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Gate;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(4000); });
        const Hunter::Status st = r.st();
        const auto learned = [&](const char* stem, int x, int y) {
            for (const MapGate& g : r.book.learned_gates(stem))
                if (g.x == x && g.y == y) return true;
            return false;
        };
        const Hunter::PressGate said = r.hunter.take_press_gate();
        check(r.w.drops.size() == 1 && st.press_gates == 1 && said == Hunter::PressGate::Drop &&
                  r.hunter.take_press_gate() == Hunter::PressGate::None && r.hunter.running() &&
                  r.hunter.config().drop_down,
              "the map changed at the press: PressGate::Drop once, then None; the hunter running, drop_down on");
        check(learned("t52_s2", 2675, 371) && learned("t53_s2", 2635, 1188),
              "both ends learned as gates: 2675,371 on t52_s2, 2635,1188 on t53_s2");
        check(lines_with(log, "Hunter: MAP CHANGED ", "after Ctrl+Down at 2675,371 on t52_s2 -> map 53/2 at "
                                                     "2635,1188 -- both ends learned as gates (no drop or jump near "
                                                     "them again this session); hunting goes on, Nhảy xuống stays "
                                                     "ticked") == 1 &&
                  lines_with(log, "hunting STOPS") == 0,
              "the WARN: \"" + line_with(log, "MAP CHANGED") + "\"");
        const std::size_t keys = r.w.keys.size();
        r.w.mobs = { mob_at(2700, 1450, 9), mob_at(2900, 1188, 10) };
        r.run(10'000);
        check(r.w.keys.size() > keys && r.w.drops.size() == 1 && r.st().state != Hunter::State::Idle,
              fmt("hunting on: %zu keys in 10 s for the monsters there (%s)", r.w.keys.size() - keys,
                  Hunter::to_string(r.st().state)));
    }
    // Late: nothing at first, then the map changes 2.5 s on -- the press's
    // all the same, and what Nothing learned is undone. A first press that
    // did nothing learns none (2026.11.15): the second near it does -- its
    // key, the second press, is the late one.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Solid;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.until([&] { return !r.w.drops.empty(); }, 3000);
        r.w.mode = Fall::LateGate;
        r.until([&] { return r.w.drops.size() > 1; }, 6000);
        r.run(1500);
        const bool nothing = r.book.no_drop_near("t52_s2", { 2675, 371 });
        r.run(2000);
        const Hunter::Status st = r.st();
        check(nothing && !r.book.no_drop_near("t52_s2", { 2675, 371 }) && st.press_gates == 1 &&
                  r.hunter.take_press_gate() == Hunter::PressGate::Drop,
              "the second Nothing at 1 s learned a no-drop point; the map change at 2.5 s overrode it and took it "
              "back -- learned as a gate instead");
    }
    // A map change after kPressWatchMs is no press's.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(3000);
        r.run(Hunter::kPressWatchMs);
        r.w.change_map();
        r.run(1000);
        check(r.w.drops.size() == 1 && r.st().press_gates == 0 && r.hunter.take_press_gate() == Hunter::PressGate::None,
              "a map change 13 s after the press: a door walked through, not the drop's");
    }
    // 2026.11.21 (the audit's B3): a map change the expedition said it would
    // make -- its gate's ↓ 3 s after a trip walk's drop -- is no press's:
    // nothing learned, nothing for the window to say.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(3000);
        const std::size_t gates = r.book.learned_gates("t52_s2").size();
        r.hunter.expect_map_change(15'000);
        r.w.change_map();
        const std::vector<std::string> log = info_lines([&] { r.run(1000); });
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && st.press_gates == 0 && r.hunter.take_press_gate() == Hunter::PressGate::None &&
                  r.book.learned_gates("t52_s2").size() == gates &&
                  lines_with(log, "the expedition's own map change, the press forgotten") == 1,
              "the expedition's map change 3 s after a drop: the press forgotten -- no gate learned, nothing said");
    }
    // Its time over, a map change after a press is the press's again.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.hunter.expect_map_change(1000);
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(3000);
        r.w.change_map();
        r.run(1000);
        check(r.w.drops.size() == 1 && r.hunter.take_press_gate() == Hunter::PressGate::Drop && r.hunter.running(),
              "the expedition's 1 s over before the press's map change: learned as a gate, as any press's -- the "
              "hunt going on");
    }
}

/// t23_s14 at 22:22:00 (2026.11.12): pressed at 135,852 on floor 853, the
/// character was reported 700 ms on at 30,931 -- on the floor it stood on, 105
/// px down its slope, moved by a skill -- and the drop was judged off the map:
/// the right file dropped for the run. Now the file goes only for a landing
/// on none of its floors; on the take-off floor further across than a no-drop
/// point covers it is not known, and on another floor of the file it is its
/// own verdict. The skills held through the settle and the fall.
void test_drop_landings_2026_11_12() {
    section("Hunter (2026.11.12): a landing on the take-off floor, or on another floor of the file, never lets the "
            "file go -- only one on none of its floors; skills held through the settle and the fall");
    if (fixture_folder().empty()) return;

    // The rig on the slope of t23_s14's: the entry by its gate at 500,600.
    const auto on_slope = [](DropRig& r) {
        r.w.page = r.book.get("t23_s14").get();
        r.w.map = 23;
        r.w.area = 14;
        r.w.entry_x = 500.0f;
        r.w.entry_y = 600.0f;
        r.w.place(135, 852);
    };
    {
        DropRig r(drop_config(120));
        on_slope(r);
        r.age_entry();
        r.w.mode = Fall::Slide;
        r.w.slide_x = 30;
        r.w.slide_y = 931;
        r.w.mobs = { mob_at(140, 1013, 5) };
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.drops.empty(); }, 4000);
            r.run(1500);
        });
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && !r.book.distrusted("t23_s14") && st.drop_map.why == MapTrust::Why::Trusted &&
                  !r.book.no_drop_near("t23_s14", { 135, 852 }),
              fmt("pressed at 135,852, reported at 30,931 700 ms on: not off the map -- t23_s14 still trusted (%s), no "
                  "no-drop point learned",
                  MapTrust::why_text(st.drop_map.why)));
        check(lines_with(log, "Hunter: drop at 135,852 -- reported at 30,931, 105 px left along the floor it stood on "
                              "with no arrow held: something else moved you") == 1 &&
                  lines_with(log, "no longer trusted") == 0 && lines_with(log, "let go of") == 0 &&
                  lines_with(log, "drops OFF") == 0,
              "said once at INFO; nothing let go of");
    }
    {
        // Within kNoDropXPx of where it pressed: Ctrl + ↓ did nothing there.
        DropRig r(drop_config(120));
        on_slope(r);
        r.age_entry();
        r.w.mode = Fall::Slide;
        r.w.slide_x = 90;
        r.w.slide_y = 886;
        r.w.mobs = { mob_at(140, 1013, 5) };
        r.until([&] { return !r.w.drops.empty(); }, 4000);
        r.run(1500);
        // The first such press is only noted (2026.11.15); back where it
        // pressed, the second is the place's second -- learned.
        const bool noted = r.w.drops.size() == 1 && !r.book.no_drop_near("t23_s14", { 135, 852 });
        r.w.place(135, 852);
        r.until([&] { return r.w.drops.size() > 1; }, 4000);
        r.run(1500);
        check(noted && r.w.drops.size() == 2 && r.book.no_drop_near("t23_s14", { 135, 852 }) &&
                  !r.book.distrusted("t23_s14") && r.st().drop_map.why == MapTrust::Why::Trusted,
              "reported 45 px down its slope: did nothing there -- noted, and at the second the point learned, the "
              "file still trusted");
    }
    {
        // On none of the file's floors: the file let go of, as ever -- by
        // MapTrust since 2026.11.24, never the book's distrust, no drops off.
        DropRig r(drop_config(120));
        on_slope(r);
        r.age_entry();
        r.w.mode = Fall::Slide;
        r.w.slide_x = 30;
        r.w.slide_y = 980;
        r.w.mobs = { mob_at(140, 1013, 5) };
        // The verdict kLandSettleMs after the report 700 ms on; MapTrust's
        // report at the pass after it.
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.drops.empty(); }, 4000);
            r.run(2500);
        });
        check(r.w.drops.size() == 1 && r.st().drop_map.why != MapTrust::Why::Trusted && !r.book.distrusted("t23_s14") &&
                  lines_with(log, "t23_s14's file let go of until 3 standing points on its floors agree again") == 1,
              fmt("reported at 30,980, on no floor of the file: off the map -- the file let go of (%s), never "
                  "distrusted: \"%s\" / \"%s\" / \"%s\"",
                  MapTrust::why_text(r.st().drop_map.why), line_with(log, "30,980").c_str(),
                  line_with(log, "let go of").c_str(), line_with(log, "trusted by").c_str()));
    }
    {
        // Past the floor the file gave, onto the next one it has: its own.
        DropRig r(drop_config(136));
        r.w.mode = Fall::Past;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(5000); });
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && st.drop_map.why == MapTrust::Why::Trusted && !r.book.distrusted("t52_s2") &&
                  lines_with(log, "Hunter: drop landed at 2675,983, 612 px down, on floor 984 (attr 1) -- not floor 633 "
                                  "the map file gave from 2675,371; t52_s2's file still gone by") == 1,
              fmt("landed on floor 984, not 633: on another floor -- the file still trusted (%s)",
                  MapTrust::why_text(st.drop_map.why)));
        const std::string minute = line_with(info_lines([&] { r.run(56'000); }), " fights, ");
        check(minute.find("1 off the map") == std::string::npos &&
                  minute.find("0 off the map, 1 on another floor, 0 map changes") != std::string::npos,
              "the minute line counts it apart: \"" + minute.substr(std::min(minute.size(), minute.find("; drops"))) +
                  "\"");
    }
    {
        // A report at y 0 mid-fall is no landing: no rise, no latch.
        DropRig r(drop_config(136));
        r.w.mode = Fall::Zero;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(5000); });
        check(r.w.drops.size() == 1 && r.st().drops_worked == 1 && lines_with(log, "drop ROSE") == 0,
              "a report at 2675,0 100 ms after Down: not a rise -- the landing on 633 worked");
    }
    {
        // The skills held (Hunter::skills_held) from the settle to the
        // verdict, and not before or after.
        DropRig r(drop_config(136));
        r.age_entry();
        const bool before = r.hunter.skills_held();
        r.w.mobs = { mob_at(2673, 632, 5) };
        bool settle = false, fall = false, after_held = true;
        std::uint64_t judged = 0;
        r.run(4000, [&] {
            const bool h = r.hunter.skills_held();
            if (r.w.drops.empty() && h) settle = true;
            if (!r.w.drops.empty() && r.w.falling && h) fall = true;
            if (r.st().drops_worked == 1 && judged == 0) judged = r.w.t;
            if (judged != 0 && r.w.t > judged + 120 && h) after_held = false;
        });
        check(!before && settle && fall && judged != 0 && after_held,
              "skills_held: not before, while it settles and falls, and not once the landing is judged");
    }
}

/// t62_s5 at 21:48:17 (2026.11.12): the spot taken at the map entry's spawn,
/// 265,5104, 33 px over floor 5137. With the file trusted, onto that floor.
void test_drop_spot_on_floor() {
    section("Hunter (2026.11.12): the spot taken at the spawn, the map's file trusted -- onto the floor under it");
    if (fixture_folder().empty()) return;

    HunterConfig c = drop_config(136);
    c.spot = true;
    Hunter::Spot s;
    const std::vector<std::string> log = info_lines([&] {
        DropRig r(c);
        r.w.page = r.book.get("t62_s5").get();
        r.w.map = 62;
        r.w.area = 5;
        r.w.entry_x = 265.0f;
        r.w.entry_y = 5104.0f;
        r.w.place(265, 5104);
        r.run(1000);
        s = r.hunter.spot();
    });
    check(s.set && s.x == 265.0f && s.y == 5136.0f,
          fmt("the spot at 265,%d: on floor 5137, not 33 px over it", static_cast<int>(s.y)));
    check(lines_with(log, "Hunter: hunting spot set where the character stands: 265,5136 (+-300 / +-100) -- on the "
                          "floor under 265,5104, by the map file") == 1,
          "said, with where the character stood");
}

/// The owner's Q6 and Q7 of 2026-11-11 (2026.11.12): the two bars -- up()
/// and down() -- the only heights a drop goes by. Until then a drop wanted
/// more than max(down(), drop_min_px 100) below, and a floor 60 px under at
/// the least (kDropMinFallPx).
void test_drop_band_2026_11_12() {
    section("Hunter (2026.11.12, the owner's Q6 and Q7): past the lower bar a drop -- 40 px down with the bar at 20; "
            "on PhòngSinhTử[4]'s two levels a drop, then another, never a walk to the ledge");
    if (fixture_folder().empty()) return;

    // t91_s1: you on the platform at 500, the monster on the one 40 px under.
    const auto on_close = [](DropRig& r) {
        r.w.page = r.book.get("t91_s1").get();
        r.w.map = 91;
        r.w.area = 1;
        r.w.entry_x = 2000.0f;
        r.w.entry_y = 1099.0f;
        r.w.place(600, 499);
    };
    {
        HunterConfig c = drop_config(120);
        c.up_reach = 10;
        c.down_reach = 20;
        DropRig r(c);
        on_close(r);
        r.age_entry();
        r.w.mobs = { mob_at(600, 539, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(4000); });
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && r.w.me_y == 539 && st.drops_worked == 1 &&
                  st.drop_map.why == MapTrust::Why::Trusted && r.w.downs_of('Z', r.w.landed_at) > 0,
              fmt("the monster 40 px below, down() 20, the floor 40 px under: one drop (%zu), landed on 539 (%d), "
                  "fought there -- drop_min_px and the 60-px fall refused it until 2026.11.12",
                  r.w.drops.size(), r.w.me_y));
        check(lines_with(log, "Hunter: drop (Ctrl+Down) toward ", "40 px down") == 1,
              "the press said: \"" + line_with(log, "Hunter: drop (Ctrl+Down)") + "\"");
    }
    {
        // The lower bar at 40: the monster between the bars -- fought from
        // where you stand, nothing dropped.
        HunterConfig c = drop_config(120);
        c.down_reach = 40;
        DropRig r(c);
        on_close(r);
        r.age_entry();
        r.w.mobs = { mob_at(600, 539, 5) };
        r.run(3000);
        check(r.w.drop_calls == 0 && r.w.me_y == 499 && r.w.downs_of('Z') > 0,
              "the lower bar at 40: the monster 40 px below between the bars -- fought where you stand, no drop");
    }

    // PhòngSinhTử[4] (t23_s14), the game's own file: the owner's box of
    // 22:23:02, 746..2844 x 1145..1549, home 1987,1245 -- the home's floor
    // (1247 at x 2010), one 68 px under it (1315) and the lower one 60 px
    // under that (1375). The monster on the lower floor, the bars at 10 and
    // 20: a drop onto the floor between, then one onto its own -- each of
    // them under drop_min_px's 100 the second, never dropped to until then,
    // and walked toward along the floor between instead.
    const std::string real = real_folder();
    if (real.empty()) {
        skip("no copy of the game's t23_s14.map here (build\\handoff\\drop\\testdata): PhòngSinhTử[4]'s two levels\n");
        return;
    }
    HunterConfig c = drop_config(120);
    c.spot = true;
    c.up_reach = 10;
    c.down_reach = 20;
    c.drop_side_px = 120;
    DropRig r(c, true, real, "t23_s14");
    r.w.map = 23;
    r.w.area = 14;
    r.w.entry_x = 300.0f;
    r.w.entry_y = 1001.0f;
    r.w.step_px = 10;
    r.w.place(2010, 1246);
    Hunter::Zone z;
    z.stem = "t23_s14";
    z.epoch = r.w.epoch;
    z.home_x = 1987.0f;
    z.home_y = 1245.0f;
    z.left = 746.0f;
    z.top = 1145.0f;
    z.right = 2844.0f;
    z.bottom = 1549.0f;
    r.hunter.set_zone(z);
    r.age_entry();
    const bool used = r.hunter.spot().set && r.hunter.spot().from_map && r.st().drop_map.why == MapTrust::Why::Trusted;
    r.w.mobs = { mob_at(2020, 1374, 5) };
    int min_x = r.w.me_x, max_x = r.w.me_x;
    const std::vector<std::string> log = info_lines([&] {
        r.run(9000, [&] {
            min_x = std::min(min_x, r.w.me_x);
            max_x = std::max(max_x, r.w.me_x);
        });
    });
    const Hunter::Status st = r.st();
    check(used && r.w.drops.size() == 2 && r.w.me_y == 1374 && st.drops_worked == 2,
          fmt("the map tab's box in use, t23_s14 trusted (%s): two drops (%zu), 1246 -> 1314 -> on the monster's floor "
              "(%d), both worked (%llu)",
              MapTrust::why_text(st.drop_map.why), r.w.drops.size(), r.w.me_y,
              static_cast<unsigned long long>(st.drops_worked)));
    check(min_x >= 1987 && max_x <= 2034 && r.w.downs_of('Z', r.w.landed_at) > 0,
          fmt("never walked toward a ledge -- x stayed %d..%d, the home's floor running on to 776 and the floor between "
              "ending at 2111 -- and the monster fought where it landed",
              min_x, max_x));
    check(lines_with(log, "Hunter: drop (Ctrl+Down) toward ", "60 px down") == 1,
          "the second press, 60 px down: \"" + line_with(log, "60 px down") + "\"");
}

void test_drop_safety() {
    section("Hunter: when the drop never goes -- Đứng yên, the spot, the screen, holds, the bottom, the map not known");
    if (fixture_folder().empty()) return;

    // Đứng yên: never (the player's answer 7).
    {
        HunterConfig c = drop_config(136);
        c.stay_put = true;
        DropRig r(c);
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(9000);
        check(r.w.drop_calls == 0 && r.st().drops == 0, "Đứng yên một chỗ: no drop in 9 s");
    }
    // The screen's plates: never.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, -1) };
        r.run(4000);
        check(r.w.drop_calls == 0 && r.st().drop_wait == Why::NotPacketTarget,
              "the screen's plates (no slot): never dropped to");
    }
    // Watched only: never, and the eyes asked nothing new.
    {
        HunterConfig c = drop_config(136);
        c.packet_positions = PacketPositions::Watch;
        DropRig r(c);
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(9000);
        check(r.w.drop_calls == 0 && r.w.wheres == 0, "packet_positions=watch: no drop, Eyes::where never asked");
    }
    // Not wired -- the command line's hunter: never.
    {
        DropRig r(drop_config(136), false);
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(9000);
        check(r.w.drop_calls == 0 && r.st().drop_wait == Why::NotWired, "no map book and no veto: never");
    }
    // GoBot's veto, and a key on Ctrl.
    {
        DropRig r(drop_config(136));
        r.veto = "HP low";
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(3000);
        const bool vetoed = r.w.drop_calls == 0 && r.st().drop_wait == Why::Veto;
        r.veto = nullptr;
        r.run(3000);
        check(vetoed && r.w.drops.size() == 1, "GoBot's veto: none while it says no, the drop once it does not");
    }
    {
        HunterConfig c = drop_config(136);
        c.jump_key = 0xA2;
        DropRig r(c);
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(9000);
        check(r.w.drop_calls == 0 && r.st().drop_wait == Why::KeyClash, "the jump key on left Ctrl: never");
    }
    // The bottom ground: no press at all.
    {
        DropRig r(drop_config(136));
        r.w.place(3100, 1119);
        r.age_entry();
        r.w.mobs_stand = false;
        r.w.mobs = { mob_at(3100, 1300, 5) };
        r.run(4000);
        check(r.w.drop_calls == 0 && r.st().drop_wait == Why::Bottom,
              fmt("standing on the bottom ground (attribute 2): no press (%s)",
                  Hunter::drop_why_name(r.st().drop_wait)));
    }
    // A hold as the settle begins: the settle is let go of, no key.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.until([&] { return r.st().dropping; }, 3000);
        r.coord.engage_critical("a revive");
        r.run(1000);
        const bool none = r.w.drop_calls == 0 && !r.st().dropping;
        r.coord.disengage_critical();
        r.run(4000);
        check(none && r.w.drops.size() == 1, "a hold as the settle begins: nothing pressed under it, the drop after");
    }
    // A hold between Ctrl and ↓: Ctrl up again, no ↓, nothing counted.
    {
        DropRig r(drop_config(136));
        r.w.hold_at_check = true;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(1500);
        const bool aborted = r.w.drop_calls == 1 && r.w.aborted == 1 && r.w.drops.empty() && r.st().drops == 0;
        r.coord.disengage_critical();
        r.run(4000);
        check(aborted && r.w.drops.size() == 1, "a hold at the last look: Aborted -- no Down, not counted; pressed "
                                                "once the hold is over");
    }
    // The game behind as the settle waits.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.until([&] { return r.st().dropping; }, 3000);
        r.w.focused = false;
        r.run(1500);
        check(r.w.drop_calls == 0, "the game behind: nothing pressed");
    }
    // A hold that begins mid-fall: Unknown, and in the air the whole 1.8 s --
    // no jump, no walk.
    {
        HunterConfig c = drop_config(136);
        c.keep_distance = true;
        DropRig r(c);
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.until([&] { return !r.w.drops.empty(); }, 3000);
        r.coord.engage_critical("a chat line");
        r.run(120);
        r.coord.disengage_critical();
        const std::uint64_t down = r.w.drops.empty() ? 0 : r.w.drops.front();
        std::vector<std::string> log = info_lines([&] { r.run(5000); });
        const std::uint64_t end = down + Hunter::kDropFallMaxMs;
        int walks = 0;
        for (const DropWorld::Key& k : r.w.keys)
            if (k.at >= down && k.at < end && k.hold < 0 && (k.vk == VK_LEFT || k.vk == VK_RIGHT)) ++walks;
        check(down != 0 && r.w.downs_of(38, down, end) == 0 && walks == 0 && r.st().drops_worked == 0,
              "a hold mid-fall: not judged; no jump and no walk until 1.8 s after Down");
    }
    // Map not known ("map 0/0"), named by its standing points.
    {
        DropRig r(drop_config(136));
        r.w.map_known = false;
        r.w.map = r.w.area = 0;
        r.age_entry();
        // A dozen more monsters about the map's floors: one frame each is no
        // standing point (2026.11.04b) ...
        for (int i = 0; i < 12; ++i)
            r.w.far_stands.push_back(MapTrust::Mob{ MapPoint{ 2210 + 40 * i, 983 }, static_cast<std::uint32_t>(100 + i),
                                                    r.w.t, 1u });
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(2000);
        const bool unknown = r.w.drop_calls == 0 && r.st().drop_wait == Why::MapUnknown &&
                             r.st().drop_map.why == MapTrust::Why::Naming && r.st().drop_map.points == 1;
        // ... their next frames at the same height, two seconds on, are.
        for (MapTrust::Mob& m : r.w.far_stands) m.at_ms = r.w.t;
        r.run(4000);
        const Hunter::Status st = r.st();
        check(unknown && st.drop_map.how == MapTrust::How::Named && st.drop_map.stem == "t52_s2" &&
                  r.w.drops.size() == 1,
              fmt("no map entry seen: no drop (%s) on yours and 12 monsters' single frames, until the monsters stood "
                  "and named t52_s2 -- then one (%zu)",
                  unknown ? "map not known" : Hunter::drop_why_name(st.drop_wait), r.w.drops.size()));
    }
    // An entry by no gate: trusted after three standing points apart -- the
    // character's, and monsters' that stand: two frames at one height.
    {
        DropRig r(drop_config(136));
        r.w.entry_x = 1000.0f;
        r.w.entry_y = 274.0f;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(1000);
        const MapTrust::Report one = r.st().drop_map;
        const bool waits = r.w.drop_calls == 0 && one.why == MapTrust::Why::Confirming && one.points == 1;
        r.w.stand_ms = r.w.t;       // the monster below's next frame, where it was: it stands
        r.run(400);
        const MapTrust::Report two = r.st().drop_map;
        r.w.far_stands.push_back(MapTrust::Mob{ MapPoint{ 2350, 983 }, 200u, r.w.t, 1u });
        r.run(400);
        const bool still = r.w.drop_calls == 0 && r.st().drop_map.why == MapTrust::Why::Confirming &&
                           r.st().drop_map.points == 2;
        r.w.far_stands.back().at_ms = r.w.t;
        r.run(3000);
        check(waits && two.why == MapTrust::Why::Confirming && two.points == 2 && still &&
                  r.st().drop_map.how == MapTrust::How::Points && r.w.drops.size() == 1,
              fmt("a map entry by no gate: no drop on you and the monster's one frame (%d point), nor on two "
                  "standing points (%d), nor on another monster's single frame -- the drop once it stands, the third "
                  "(%zu)",
                  one.points, two.points, r.w.drops.size()));
    }
    // A key-up owed: no drop, and from kDropOwedPasses on every key let go of
    // once, nothing walked or pressed while it is owed -- then, the key-ups
    // out, the hunt goes on by itself and drops (2026.11.24: until then the
    // hunter halted for the window to stop the hunt).
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.owed = true;
        r.w.mobs = { mob_at(2673, 632, 5) };
        std::size_t keys_held = 0;
        bool stuck = false;
        const std::vector<std::string> log = info_lines([&] {
            r.run(600);
            stuck = r.st().drop_keys_stuck;
            const std::size_t keys0 = r.w.keys.size();
            r.run(3000);
            keys_held = r.w.keys.size() - keys0;
        });
        check(r.w.drop_calls == 0 && stuck && r.st().drop_keys_stuck && keys_held == 0 && r.hunter.running() &&
                  lines_with(log, "Hunter: keys owed after a drop (Down or Ctrl up not sent, 3 passes) -- every key let "
                                  "go of; nothing pressed until Windows takes the key-ups, then hunting goes on by "
                                  "itself") == 1,
              fmt("a drop's key-up owed three passes: no drop, every key let go of once, nothing pressed while owed "
                  "(%zu keys in 3 s), the hunter running",
                  keys_held));
        r.w.owed = false;
        const std::vector<std::string> after = info_lines([&] {
            r.until([&] { return !r.w.drops.empty(); }, 4000);
            r.run(200);
        });
        check(!r.st().drop_keys_stuck && r.w.drops.size() == 1 &&
                  lines_with(after, "Hunter: the owed key-ups went out -- hunting goes on") == 1,
              fmt("the key-ups out: hunting on by itself, the drop pressed (%zu)", r.w.drops.size()));
    }
    // The 2026.11.24 review: the key-ups owed three passes, then Nhảy xuống
    // unticked at the amber note -- no spot, no jump area, the press's watch
    // run out. The owed count is cleared all the same once the key-ups went
    // out, and the hunter fights again (it stayed idle for the rest of the run
    // when the owed keys were looked at only with something ticked).
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.owed = true;
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(600);
        const bool stuck = r.st().drop_keys_stuck;
        HunterConfig off = drop_config(136);
        off.drop_down = false;
        off.spot = false;
        r.hunter.set_config(off);
        r.run(Hunter::kPressWatchMs + 1000);
        r.w.owed = false;
        // A monster on its own floor, in reach: something to fight once the
        // hunter goes on.
        r.w.mobs = { mob_at(2720, 371, 6) };
        const std::size_t keys0 = r.w.keys.size();
        r.run(3000);
        check(stuck && !r.st().drop_keys_stuck && r.w.keys.size() > keys0 && r.hunter.running(),
              fmt("owed three passes, then drop_down unticked with no spot: the key-ups out, the count cleared, the "
                  "hunter pressing keys again (%zu keys in 3 s)",
                  r.w.keys.size() - keys0));
    }
}

void test_drop_spot_and_limits() {
    section("Hunter: the drop with Chỗ đánh quái (the player's answer 2), the give-up clock, and the limits");
    if (fixture_folder().empty()) return;

    // The spot's area 100 px up and down: the monster below is outside it.
    {
        HunterConfig c = drop_config(136);
        c.spot = true;
        DropRig r(c);
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(4000);
        check(r.w.drop_calls == 0, "the spot on, its area 100 px up and down: the monster outside it, no drop");
    }
    // 300 px: inside it, standing, landing and the monster -- dropped to.
    {
        HunterConfig c = drop_config(136);
        c.spot = true;
        c.spot_half_h = 300;
        DropRig r(c);
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(4000);
        check(r.w.drops.size() == 1 && r.w.me_y == 632 && r.hunter.spot().set,
              "300 px: where it stands, lands and the monster all inside -- dropped to");
    }
    // The spot standing for want of a fix: never.
    {
        HunterConfig c = drop_config(136);
        c.spot = true;
        c.spot_half_h = 300;
        DropRig r(c);
        r.w.state = WorldFix::State::Lost;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(4000);
        check(r.w.drop_calls == 0, "the spot standing, the packets having lost the character: no drop");
    }
    // No give-up while it settles and falls.
    {
        HunterConfig c = drop_config(136);
        c.give_up_ms = 600;
        DropRig r(c);
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.until([&] { return r.w.landed_at != 0; }, 3000);
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && st.targets_dropped == 0,
              "give_up_ms 600: the settle and the fall gave nothing up -- the drop landed first");
    }
    // A press that never goes out (NotSent): two settles let go of without a
    // press, then no more for that monster -- and the give-up clock, held
    // still for each settle, runs out between them as it would have.
    {
        DropRig r(drop_config(136));
        r.w.not_sent = true;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(12'000); });
        const Hunter::Status st = r.st();
        check(r.w.drop_calls == Hunter::kDropSettlesPerTarget && r.w.drops.empty() && st.targets_dropped >= 1 &&
                  lines_with(log, "Hunter: gave up on ") >= 1,
              fmt("NotSent at every press: %d tries in 12 s (2 settles for a monster at most), given up on %llu "
                  "time(s)",
                  r.w.drop_calls, static_cast<unsigned long long>(st.targets_dropped)));
    }
    // Every press refused NotSettled -- the packets say in the air all along:
    // never pressed, and given up on out of reach once in 15 s, not held back
    // by settle after settle. At 2700, where the floor below is the same 24 px
    // either side, a settle begins with the fix not settled.
    {
        DropRig r(drop_config(100));
        r.w.place(2700, 371);
        r.age_entry();
        r.w.airborne = true;
        r.w.mobs = { mob_at(2702, 632, 5) };
        const std::uint64_t t0 = r.w.t;
        std::uint64_t gave_at = 0;
        bool capped = false;
        const std::vector<std::string> log = info_lines([&] {
            r.run(15'000, [&] {
                if (gave_at == 0 && r.st().targets_dropped > 0) gave_at = r.w.t;
                if (r.st().drop_wait == Why::TargetCap) capped = true;
            });
        });
        const Hunter::Status st = r.st();
        // The settles used up for it at some look -- for kTargetCountMs since
        // 2026.11.24, then counted afresh (the rest of the run until then).
        check(r.w.drop_calls == 0 && st.targets_dropped >= 1 && lines_with(log, "Hunter: gave up on ") == 1 && capped,
              fmt("in the air at every press: no press, given up on %llu time(s), said %d time(s) in 15 s, the "
                  "settles for it used up a while (%s at the end)",
                  static_cast<unsigned long long>(st.targets_dropped), lines_with(log, "Hunter: gave up on "),
                  Hunter::drop_why_name(st.drop_wait)));
        // 4 s of walking under it, the two settles' 0.6 s each not counted --
        // not 4 s from the last settle's start (6.6 s), nor settles not begun
        // (4 s).
        check(gave_at != 0 && gave_at - t0 > 4000 + Hunter::kDropSettleMs && gave_at - t0 < 6000,
              fmt("given up %llu ms after it was picked: the settles held the clock still, they did not start it "
                  "again",
                  static_cast<unsigned long long>(gave_at - t0)));
    }
    // The minute line's "refused:" counts the refusals acted on -- a settle
    // waiting for the stop's report is none: in the air at every press, each
    // settle is refused "not settled" once, as it ends, not at every look.
    {
        DropRig r(drop_config(100));
        r.w.place(2700, 371);
        r.age_entry();
        r.w.airborne = true;
        r.w.mobs = { mob_at(2702, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(56'000); });
        const std::string m = line_with(log, " fights, ");
        const auto number_after = [&m](const char* what) {
            const std::size_t at = m.find(what);
            int n = -1;
            if (at == std::string::npos || std::sscanf(m.c_str() + at + std::strlen(what), "%d", &n) != 1) return -1;
            return n;
        };
        const int settles = number_after("(settles ");
        const int not_settled = number_after("not settled ");
        check(settles >= 1 && not_settled >= 1 && not_settled <= settles && r.w.drop_calls == 0,
              fmt("the minute line: %d settles, refused \"not settled\" %d times -- once a settle at most, not once a "
                  "look (\"%s\")",
                  settles, not_settled,
                  m.find("; drop looks") == std::string::npos ? m.c_str() : m.substr(m.find("; drop looks")).c_str()));
    }
    // A minute's and a monster's limits, on the clock.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Solid;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.until([&] { return !r.w.drops.empty(); }, 3000);
        const std::uint64_t first = r.w.drops.empty() ? 0 : r.w.drops.front();
        // A new monster at once, somewhere the first did not learn.
        r.w.place(2350, 371);
        r.w.mobs = { mob_at(2355, 983, 6) };
        r.until([&] { return r.w.drops.size() > 1; }, 4000);
        check(r.w.drops.size() == 2 && r.w.drops[1] - first >= Hunter::kDropGapMs,
              "a second press no sooner than 1.5 s after the first");
    }
}

// ------------------------------------------------------------- settings

void test_drop_config() {
    section("HunterConfig: drop_down and its two numbers in hunt.ini -- drop_min_px no longer read (2026.11.12)");
    HunterConfig d;
    check(!d.drop_down && d.drop_side_px == 120 && d.drop_gate_px == 150, "the code's defaults: off, 120, 150");
    const std::string path = temp_path("drop_hunt.ini");
    {
        std::ofstream(path) << "[Hunt]\nreach=150\n";
        HunterConfig old;
        check(old.load(path) && !old.drop_down && old.drop_side_px == 120 && old.drop_gate_px == 150,
              "a hunt.ini without them: off, at the defaults");
    }
    {
        HunterConfig c;
        c.drop_down = true;
        c.drop_side_px = 90;
        c.drop_gate_px = 60;
        HunterConfig back;
        check(c.save(path) && back.load(path) && back.drop_down && back.drop_side_px == 90 && back.drop_gate_px == 60,
              "saved and read back");
    }
    {
        std::ofstream(path) << "[Hunt]\ndrop_down=true\ndrop_side_px=9999\ndrop_gate_px=1\n";
        HunterConfig lo;
        const bool read_lo = lo.load(path);
        std::ofstream(path) << "[Hunt]\ndrop_side_px=3\ndrop_gate_px=4000\n";
        HunterConfig hi;
        check(read_lo && hi.load(path) && lo.drop_side_px == 300 && lo.drop_gate_px == 40 && hi.drop_side_px == 20 &&
                  hi.drop_gate_px == 400,
              "kept within 20-300 and 40-400");
    }
    {
        // An older file's drop_min_px: never read, left as it is -- the
        // lower bar alone says how far below a drop goes for (the owner's Q7).
        std::ofstream(path) << "[Hunt]\ndrop_down=true\ndrop_min_px=100\ndown_reach=20\n";
        HunterConfig old;
        IniFile after;
        check(old.load(path) && old.down() == 20 && old.save(path) && after.load(path) &&
                  after.get_int("Hunt", "drop_min_px", 0) == 100,
              "an older file's drop_min_px=100: not read, the band's 20 is the threshold; saved over, the line kept");
    }
    DeleteFileA(path.c_str());
    check(Hunter::kDropMinFallPx == Hunter::kDropLandPx + 1 && Hunter::kDropMinFallPx == 25,
          "the least fall: 25 px, a landing kDropLandPx (24) lower told from standing -- 60 until 2026.11.12");

#ifdef GOBOT_SOURCE_DIR
    const std::string shipped = std::string(GOBOT_SOURCE_DIR) + "\\configs\\hunt.ini";
    HunterConfig ship;
    IniFile ini;
    check(ship.load(shipped) && ini.load(shipped) && ini.has_key("Hunt", "drop_down") &&
              !ini.has_key("Hunt", "drop_min_px") && ini.has_key("Hunt", "drop_side_px") &&
              ini.has_key("Hunt", "drop_gate_px") && !ship.drop_down && ship.drop_side_px == 120 &&
              ship.drop_gate_px == 150,
          "the shipped hunt.ini: drop_down=false, 120, 150 (the player's answer 8) -- no drop_min_px");
#endif
}

// ------------------------------------------------------------- the window

#ifdef GOBOT_SOURCE_DIR
std::string source(const char* rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// The body of the function whose definition starts with `head`.
std::string body(const std::string& text, const char* head) {
    const std::size_t at = text.find(head);
    if (at == std::string::npos) return {};
    const std::size_t open = text.find('{', at);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        if (text[i] == '}' && --depth == 0) return text.substr(at, i - at + 1);
    }
    return {};
}
#endif

void test_drop_window() {
    section("GoBot's window and the drop: the tick, its reasons, the wiring, a map change (its source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string style = source("/apps/gobot_gui/simple_style.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string keys = source("/apps/gobot_gui/simple_hotkeys.cpp");
    const std::string eyes = source("/src/packet_eyes.cpp");
    if (gui.empty() || style.empty() || view.empty()) {
        skip("the GUI's source is not here to read\n");
        return;
    }
    constexpr std::size_t npos = std::string::npos;

    const std::string tick = body(style, "void GuiApp::simple_drop_tick() {");
    const std::string why = body(style, "std::string GuiApp::simple_drop_refusal_vi() {");
    const std::size_t asked = tick.find("const std::string why = simple_drop_refusal_vi();");
    const std::size_t grey = tick.find("ImGui::BeginDisabled(!why.empty());");
    const std::size_t box = tick.find("ImGui::Checkbox(\"Nhảy xuống bậc dưới\", &hunt_config_.drop_down)");
    check(asked != npos && grey != npos && box != npos && asked < grey && grey < box &&
              tick.find("ImGui::SetItemTooltip(\"%s\", tip.c_str());") != npos &&
              tick.find("trial_hint(kTrialDrop,") != npos,
          "the simple screen's tick \"Nhảy xuống bậc dưới\": greyed out while it may not be ticked, its reason as "
          "its tooltip");
    const std::size_t still = why.find("if (hunt_config_.stay_put) return \"Không nhảy xuống khi Đứng yên một chỗ: "
                                       "bỏ dấu Đứng yên trước.\";");
    const std::size_t packets = why.find("if (hunt_config_.packet_positions != PacketPositions::On) return \"Cần dấu "
                                         "Vị trí từ gói tin.\";");
    check(still != npos && packets != npos && still < packets && why.find("đang là ") != npos &&
              why.find("hunt_config_.spot") == npos,
          "its reasons: Đứng yên first (the player's answer 7), then no packets, then a key on Ctrl -- never the "
          "spot (answer 2)");
    check(view.find("simple_drop_tick();") != npos && view.find("simple_drop_hints();") != npos &&
              view.find("hs.dropping ? \"Đang nhảy xuống tới quái ở bậc dưới (Ctrl + ↓).\"") != npos,
          "drawn on the Tự đánh quái row, its hints under it, and the card says when it drops");
    check(gui.find("ImGui::Checkbox(\"Drop to things below you (Ctrl+Down)\", &hunt_config_.drop_down)") != npos &&
              gui.find("const std::string why = simple_drop_refusal_vi();") != npos,
          "the full window's checkbox, greyed out for the same reasons");

    const std::string init = body(gui, "void GuiApp::initialise() {");
    check(init.find("hunter_->set_map_book(&game_maps_);") != npos && init.find("hunter_->set_drop_veto(") != npos &&
              init.find("in.takeoffs = [this] { return packet_takeoffs(); };") != npos,
          "the hunter given the game's map files, GoBot's veto and the take-offs as GoBot starts");
    check(eyes.find("e.stands = [this] { return stands(); };") != npos &&
              eyes.find("e.takeoffs = inputs_.takeoffs;") != npos,
          "PacketEyes hands on the monsters' stops and the take-offs");
    const std::size_t stray = init.find("input_.release_stray_drop_keys()");
    const std::string start = body(gui, "void GuiApp::start_hunt() {");
    check(stray != npos && init.find("hunt_config_.drop_down", stray > 200 ? stray - 200 : 0) != npos &&
              start.find("if (hunt_config_.drop_down) input_.release_stray_drop_keys();") != npos,
          "a ↓ or Ctrl left down let go of at GoBot's start and at each start of the hunt, with drop_down on");

    const std::string gtick = body(gui, "void GuiApp::tick() {");
    const std::string dtick = body(gui, "void GuiApp::drop_tick() {");
    // 2026.11.24, the owner: "khi hỏng thì thử lại luôn ... chứ không phải là
    // dừng" -- a map change after a press is said once and the hunt goes on:
    // no stop_hunt(), nothing unticked, hunt.ini not saved (the player's
    // answer 3 of 2026-11-04 had it stop, untick and save until then).
    const std::size_t take = dtick.find("const Hunter::PressGate gate = hunter_->take_press_gate();");
    const std::size_t none = take == npos ? npos : dtick.find("if (gate == Hunter::PressGate::None) return;", take);
    const std::size_t note = none == npos ? npos : dtick.find("drop_note_vi_ = std::string(\"Lần \") + what", none);
    check(gtick.find("drop_tick();") != npos && take != npos && none != npos && note != npos &&
              dtick.find("simple_note(drop_note_vi_, 15000);") != npos &&
              dtick.find("GB_INFO(\"Hunter: a %s changed the map -- learned as a gate; the hunt goes on, the ticks "
                         "unchanged\"") != npos &&
              dtick.find("stop_hunt();") == npos && dtick.find("drop_down = false") == npos &&
              dtick.find("jump_up = false") == npos && dtick.find("hunt_config_.save(") == npos &&
              dtick.find("drop_stopped_vi_") == npos,
          "drop_tick(): a map change after a press said once in amber and the log -- no stop_hunt(), nothing "
          "unticked, hunt.ini not saved (2026.11.24)");
    const std::size_t owed = gtick.find("if (input_.keys_owed() && now_ms() - owed_look_ms_ >= 100)");
    const std::size_t owed_end = owed == npos ? npos : gtick.find("drop_tick();", owed);
    const std::string owed_block = owed == npos || owed_end == npos ? std::string() : gtick.substr(owed, owed_end - owed);
    check(owed_block.find("input_.release_all();") != npos && owed_block.find("stop_hunt();") == npos &&
              dtick.find("stayed owed") == npos,
          "and a key-up stuck: every key let go of in tick() -- the hunt never stopped for it");
    const std::string veto = body(gui, "const char* GuiApp::drop_veto_now() const {");
    // HP low no longer a veto (2026.11.12b, the owner: drops and jumps up at
    // any HP); HP not known still is.
    check(veto.find("hp_percent_now()") != npos && veto.find("chat_hp_floor_") == npos &&
              veto.find("return \"HP is low\";") == npos &&
              veto.find("if (hp >= 0.0f && hp < static_cast<float>(bag::kJobDownHpPct)) return \"the character is "
                        "down\";") != npos &&
              veto.find("if (hp < 0.0f && drop_hp_watch_.load()) return \"HP is not known\";") != npos &&
              veto.find("drop_veto_.load()") != npos,
          "the veto: the character down or HP not known -- never HP low, since 2026.11.12b -- and what tick() saw: "
          "the macro held, a walk, the keeper crossing, a GoBot key on Ctrl or Down");
    check(keys.find("if (hunt_config_.drop_down) k.push_back({ VK_CONTROL, \"nhảy xuống (Ctrl + ↓)\" });") != npos,
          "Ctrl among the keys GoBot presses, with drop_down on");
#else
    skip("no source folder\n");
#endif
}

/// 2026.11.15, the owner's log of 00:27:54 from the drop's side: a report on
/// the ground in mid-air -- a hit's state 0 at the top of an arc -- is no
/// landing. Judged off the file's floors at once, it had the file distrusted
/// and every drop, jump and way back with it; the verdict waits kLandSettleMs
/// for the next report. One that stands had the file distrusted kDistrustMs
/// (120 s) from 2026.11.15, drops off; since 2026.11.24 MapTrust lets it go
/// and takes it again from kTrustPoints standing points on its floors -- no
/// clock, nothing switched off.
void test_drop_apex_report() {
    section("Hunter (2026.11.15, 2026.11.24): a drop's report in mid-air waits for the next report -- the landing "
            "comes, the file stays trusted; none comes, the file is let go of after kLandSettleMs and taken again "
            "from its standing points, no clock");
    if (fixture_folder().empty()) return;

    // A: the report in mid-air 100 ms after Down, the landing on 633 on its way.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Apex;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(5000); });
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && st.drops_worked == 1 && !r.book.distrusted("t52_s2") &&
                  lines_with(log, "no longer trusted") == 0 && lines_with(log, "let go of") == 0 &&
                  lines_with(log, "drops OFF") == 0 && st.drop_map.why == MapTrust::Why::Trusted,
              fmt("a report at 2675,481 -- on no floor -- 100 ms after Down, the landing on 633 on its way: the drop "
                  "worked (%llu), the file still trusted (%d)",
                  static_cast<unsigned long long>(st.drops_worked), r.book.distrusted("t52_s2") ? 1 : 0));
    }

    // B: nothing follows the report: judged only kLandSettleMs after it -- off
    // the file's floors, the file let go of by MapTrust -- and taken again as
    // soon as three standing points on its floors agree, no clock.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::ApexStays;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.until([&] { return !r.w.drops.empty(); }, 4000);
        const std::uint64_t reported = r.w.drops.front() + 100;
        r.until([&] { return r.w.t >= reported + Hunter::kLandSettleMs - 250; }, 3000);
        const bool waiting = r.st().drop_map.why == MapTrust::Why::Trusted;
        std::vector<std::string> log = info_lines([&] { r.run(800); });
        const bool off = r.st().drop_map.why != MapTrust::Why::Trusted && !r.book.distrusted("t52_s2") &&
                         lines_with(log, "not on floor 633 the map file gave -- t52_s2's file let go of until 3 "
                                         "standing points on its floors agree again (no clock)") == 1;
        // Put back on its ledge, the game as it should be: three standing
        // points on the file's floors, and it is gone by again at once.
        r.w.mode = Fall::Floor;
        r.w.mobs.clear();
        const std::uint64_t back_at = r.w.t;
        log = info_lines([&] {
            for (const int x : { 2675, 2575, 2475 }) {
                r.w.place(x, 371);
                r.run(700);
            }
        });
        const MapTrust::Report t = r.st().drop_map;
        check(waiting && off && t.why == MapTrust::Why::Trusted && t.how == MapTrust::How::Points &&
                  r.w.t - back_at < 5000 && lines_with(log, "Hunter: t52_s2 trusted by 3 standing points on its floors") == 1,
              fmt("nothing after the report: waiting at kLandSettleMs - 250 ms (%d), then off the map and the file let "
                  "go of (%d); trusted again by its standing points %llu ms after (%s) -- \"%s\"",
                  waiting ? 1 : 0, off ? 1 : 0, static_cast<unsigned long long>(r.w.t - back_at),
                  MapTrust::why_text(t.why), line_with(log, "trusted by").c_str()));
    }
}

/// What DOC-TRUOC.txt and README.md say of 2026.11.15's input misses (the
/// notes' words and numbers are the code's own), and where the code says them
/// -- the log's lines, the window's cards, the constants.
void test_drop_notes_2026_11_15() {
    section("2026.11.15 notes: the pause in place of \"off until the next start\", the first miss's 1.2 s, the second "
            "near a place, the way back asked every 3 s -- DOC-TRUOC.txt, README.md and the code say the same");
#ifdef GOBOT_SOURCE_DIR
    // Spaces and line ends one space: a phrase of the notes found however they wrap it.
    const auto flat = [](const std::string& text) {
        std::string out;
        out.reserve(text.size());
        bool space = false;
        for (const char c : text) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                space = true;
                continue;
            }
            if (space && !out.empty()) out += ' ';
            space = false;
            out += c;
        }
        return out;
    };
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string drop = source("/src/hunter_drop.cpp");
    const std::string jump = source("/src/hunter_jump.cpp");
    const std::string hunter = source("/src/hunter.cpp");
    const std::string header = source("/include/gobot/hunter.hpp");
    if (doc.empty() || readme.empty() || drop.empty() || jump.empty() || hunter.empty() || header.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto has = [](const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    };
    const auto n = [](auto v) { return std::to_string(v); };
    // 1200 ms as the notes say it: "1,2" in DOC-TRUOC.txt, "1.2" in README.
    const std::string retry_vi = n(Hunter::kRetryPassOverMs / 1000) + "," + n(Hunter::kRetryPassOverMs % 1000 / 100);
    const std::string retry_en = n(Hunter::kRetryPassOverMs / 1000) + "." + n(Hunter::kRetryPassOverMs % 1000 / 100);
    // 2026.11.15's pauses, history since 2026.11.24 (kFailsCoolMs is gone: no
    // pause any more) -- the entry of 2026.11.15 keeps its numbers.
    const std::string cools_vi = "30 giây; lần nghỉ thứ hai 60 giây, từ lần thứ ba 120 giây";
    const std::string cools_en = "(30 s, 60 s, 120 s";

    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    // 2026.11.15b's entry (the shop and the party again after 30 s) on top
    // of it since, and 2026.11.15c's (a report in mid-air is no landing) on
    // top of that, and 2026.11.15d's (thirty seconds for the game to react)
    // on top of that, and 2026.11.16's (lighter on weak PCs, old features
    // removed) on them, and 2026.11.16b's (the skills in turn while hunting
    // too) on those, and 2026.11.17's (the MP bar, HP/MP by the packets
    // alone, the skills even while hunting) first of all.
    const std::size_t e17 = doc.find("- Mới trong bản 2026.11.17 (");
    const std::size_t e16b = doc.find("- Mới trong bản 2026.11.16b (");
    const std::size_t e16 = doc.find("- Mới trong bản 2026.11.16 (");
    const std::size_t e15d = doc.find("- Mới trong bản 2026.11.15d (");
    const std::size_t e15c = doc.find("- Mới trong bản 2026.11.15c (");
    const std::size_t e15b = doc.find("- Mới trong bản 2026.11.15b (");
    const std::size_t e15 = doc.find("- Mới trong bản 2026.11.15 (");
    const std::size_t e14b = doc.find("- Mới trong bản 2026.11.14b (");
    const std::size_t r14b = readme.find("### The shop after a pass, ESC after 10 s stuck");
    const std::size_t r15 = readme.find("### One lost key press, pauses instead of \"off until the next start\", and the way "
                                        "back asked sooner (2026.11.15)");
    const std::size_t rmacro = readme.find("## Keyboard macro");
    check(sec5 != std::string::npos && e15 != std::string::npos && e14b != std::string::npos && sec5 < e15 &&
              e15 < e14b && e15c != std::string::npos && e15b != std::string::npos &&
              e17 != std::string::npos && e16b != std::string::npos && e16 != std::string::npos &&
              e15d != std::string::npos && doc.find("- Mới trong bản", sec5) == doc.find("- Mới trong bản 2026.11.24 (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.24 (") + 1) == doc.find("- Mới trong bản 2026.11.17c (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17c (") + 1) == doc.find("- Mới trong bản 2026.11.17b (") &&
              doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17b (") + 1) == e17 &&
              doc.find("- Mới trong bản", e17 + 1) == e16b && doc.find("- Mới trong bản", e16b + 1) == e16 &&
              doc.find("- Mới trong bản", e16 + 1) == e15d && doc.find("- Mới trong bản", e15d + 1) == e15c && doc.find("- Mới trong bản", e15c + 1) == e15b &&
              doc.find("- Mới trong bản", e15b + 1) == e15 &&
              has(doc, "- Mới trong bản 2026.11.15 (game bỏ sót phím không còn làm GoBot tắt nhảy lên/xuống hay cấm một "
                       "chỗ, mà chỉ nghỉ một lúc rồi tự thử lại; hỏi lại đường về sau " +
                           n(Hunter::kStrandedReaskMs / 1000) + " giây; theo log của bạn đêm 14/11 sang 15/11; chưa thử "
                           "trên game thật).") &&
              r14b != std::string::npos && r15 != std::string::npos && rmacro != std::string::npos && r14b < r15 &&
              r15 < rmacro && has(readme, "Since 2026.11.15 (not yet run on the game)."),
          "DOC-TRUOC.txt: the 2026.11.15 entry in section 5 right under 2026.11.15b's, not yet tried on the game; "
          "README's section after 2026.11.14b's and before the macro's");
    check(has(doc, "\"check log moi nhat, co 1 so loi nhu khi ra ngoai vung thi khong tu quay vao lai, khi danh quai o "
                   "duoi nguong px thi khong chiu nhay xuong hoac nhay len\"") &&
              has(readme, "*\"check log moi nhat, co 1 so loi nhu khi ra ngoai vung thi khong tu quay vao lai, khi danh "
                          "quai o duoi nguong px thi khong chiu nhay xuong hoac nhay len\"*"),
          "the owner's words of 2026-11-15, as written, in the notes and in README");
    // The numbers the notes quote are the constants' -- the pauses' (3 in a
    // row, 5 in a run, 30 / 60 / 120 s) 2026.11.15's own, gone since 2026.11.24.
    check(Hunter::kNoDropSuspectMs == 120'000 && Hunter::kRetryPassOverMs == 1200 && Hunter::kStrandedReaskMs == 3000 &&
              has(doc, "ghi nhớ là Ctrl+↓ không nhảy xuống được ở đó khi lần Ctrl+↓ THỨ HAI không làm gì (hay chỉ nhảy "
                       "tại chỗ) ở gần chỗ lần trước (không quá " +
                           n(GameMapBook::kNoDropXPx) + " px ngang, " + n(GameMapBook::kNoDropYPx) + " px dọc) trong "
                           "vòng " + n(Hunter::kNoDropSuspectMs / 1000) + " giây") &&
              has(doc, "chỉ bị bỏ qua " + retry_vi + " giây (trước là 8 giây)") &&
              has(doc, "3 lần liền (hay 5 lần trong một lượt) không ăn không còn tắt nhảy xuống hay nhảy lên tới lần BẮT "
                       "ĐẦU sau: chỉ nghỉ " + cools_vi) &&
              has(doc, "GoBot hỏi lại bản đồ mỗi " + n(Hunter::kStrandedReaskMs / 1000) + " giây (trước là 10 giây)") &&
              has(doc, "bot hỏi lại bản đồ mỗi 10 giây (từ bản 2026.11.15: mỗi " + n(Hunter::kStrandedReaskMs / 1000) +
                           " giây)") &&
              has(readme, "`Hunter::kNoDropSuspectMs` (" + n(Hunter::kNoDropSuspectMs / 1000) + " s)") &&
              has(readme, "`kRetryPassOverMs` (" + retry_en + " s)") &&
              has(readme, "`kFailsCoolMs` " + cools_en + ": the first, the second, the third and every later one") &&
              has(readme, "`kStrandedReaskMs` is " + n(Hunter::kStrandedReaskMs / 1000) + " s (10 s before)") &&
              has(readme, "`kStrandedReaskMs` (" + n(Hunter::kStrandedReaskMs / 1000) + " s since 2026.11.15; 10 s)") &&
              has(readme, "`kDropFailsInRow` (3) in a row, or `kDropFailsPerRun` and `kJumpUpFailsPerRun` (5) in the "
                          "run"),
          "the notes' numbers: a place kept " + n(Hunter::kNoDropSuspectMs / 1000) + " s and " +
              n(GameMapBook::kNoDropXPx) + " x " + n(GameMapBook::kNoDropYPx) + " px, the first miss's " + retry_en +
              " s, 2026.11.15's pauses 30, 60 and 120 s, the way back asked every " +
              n(Hunter::kStrandedReaskMs / 1000) + " s -- as hunter.hpp has them");
    // The log's words, as the notes of 2026.11.15 quote them -- the pause's
    // gone from the code since 2026.11.24, the first miss's kept.
    check(!has(drop, "\"Hunter: drops on again after %llu s\"") && !has(drop, "\"Hunter: jumps up on again after %llu s\"") &&
              has(drop, "not held against the place yet (a second near ") &&
              !has(drop, "drops paused for %llu s") && !has(jump, "jumps up paused for %llu s") &&
              has(doc, "\"Hunter: drops on again after 30 s\", \"Hunter: jumps up on again after 30 s\"") &&
              has(doc, "log ghi \"not held against the place yet\"") &&
              has(doc, "\"Hunter: 3 presses of Ctrl+Down in a row did not drop -- drops paused for 30 s (the 1st pause "
                       "since the last drop that worked; they go again by themselves)\"") &&
              has(doc, "\"Hunter: jumps up on again after 30 s\".") &&
              has(readme, "\"Hunter: drops on again after 30 s\" (`jumps up` likewise)") &&
              has(readme, "-- not held against the place yet (a second near it within 120 s is); ...\""),
          "the log's lines: 2026.11.15's notes keep the pause's words as history; the code says the first miss as "
          "not held against the place, and no pause any more (2026.11.24)");
    // Where the code does it since 2026.11.24: the pass forgets the places
    // learned kLearnedForgetMs ago first thing; no latch, no pause, no run's
    // count; the first failed press of a row is passed over kRetryPassOverMs,
    // every other kRetryFightMs.
    const std::size_t pass_at = hunter.find("void Hunter::pass() {");
    const std::size_t end_at = pass_at == std::string::npos ? pass_at : hunter.find("forget_learned(clock_now());", pass_at);
    const std::size_t view_at = pass_at == std::string::npos ? pass_at : hunter.find("eyes_.view", pass_at);
    check(end_at != std::string::npos && view_at != std::string::npos && end_at < view_at &&
              !has(hunter, "cool_downs_end(") && !has(drop, "latch_drops(") && !has(jump, "latch_jumps(") &&
              !has(drop, "cool_drops(") && !has(jump, "cool_jumps(") && !has(header, "fails_cool_ms(") &&
              !has(drop, "drop_fails_run_") && !has(jump, "jump_fails_run_") && !has(jump, "bar_fails_run_") &&
              has(drop, "const bool first_fail = drop_fails_row_ == 0;") &&
              has(drop, "const std::uint64_t over_ms = first_fail ? kRetryPassOverMs : kRetryFightMs;") &&
              has(jump, "const bool first_fail = jump_fails_row_ == 0;") &&
              has(jump, "const std::uint64_t over_ms = first_fail ? kRetryPassOverMs : kRetryFightMs;") &&
              has(hunter, "drop_fails_row_ = 0;") && has(hunter, "jump_fails_row_ = 0;") &&
              !has(drop, "OFF until the next BẮT ĐẦU") && !has(jump, "OFF until the next BẮT ĐẦU") &&
              !has(hunter, "OFF until the next BẮT ĐẦU") &&
              has(header, "static constexpr std::size_t kLearnedMax = 64;") &&
              has(drop, "while (learned_.size() >= kLearnedMax) {") &&
              has(drop, "unlearn_place(oldest.kind, oldest.stem, oldest.at, true);") &&
              has(drop, "unlearn_place(n.kind, n.stem, n.at, false);"),
          "the pass forgets the places learned kLearnedForgetMs ago before it asks anything; no latch, pause or run's "
          "count; the first failed press of a row passed over 1.2 s, every other one fight turn -- no line says "
          "\"OFF until the next BẮT ĐẦU\" any more; at 64 notes the oldest is given back to the book at once");
    // The same release's other parts (gobot-c5's): the file's walls, the fall
    // no report follows, and the plain Up's pause -- the notes say what the
    // code does, with its numbers.
    const std::string book_cpp = source("/src/map_book.cpp");
    // START-HERE's paragraph (package.ps1), after 2026.11.14b's, saying the same in short.
    const std::string pack = flat(source("/package.ps1"));
    const std::size_t p14b = pack.find("The shop after a pass, ESC after 10 s stuck, and the game's message after an invite "
                                       "(2026.11.14b, not yet tried on the game).");
    const std::size_t p15 = pack.find("Walls, a lost key press and the way back (2026.11.15, not yet tried on the game).");
    check(p14b != std::string::npos && p15 != std::string::npos && p14b < p15 &&
              has(pack, "after 3 in a row (or 5 in a run, with more misses than hits) that jump or drop pauses for 30 s, "
                        "then 60 s, then 120 s, and goes again by itself") &&
              has(pack, "the way back is asked again every " + n(Hunter::kStrandedReaskMs / 1000) + " s"),
          "package.ps1's START-HERE: the 2026.11.15 paragraph after 2026.11.14b's, with the pauses and the way back's "
          "seconds as the code has them");
    // The plain Up's pause (3 in a row, 5 in a run) is 2026.11.15's history:
    // gone from the code since 2026.11.24.
    check(!book_cpp.empty() && GameMapBook::kBodyPx == 48 && Hunter::kBlindLandMs == 700 &&
              Hunter::kBlindLandMaxPx == 600 &&
              has(doc, "dọc một sàn mà gặp tường trong " + n(GameMapBook::kBodyPx) +
                           " px phía trên chân thì DỪNG ở chân tường") &&
              has(doc, "sau 0,7 giây không có gói nào khác, thì GoBot coi nhân vật đã đáp xuống sàn đầu tiên dưới cột "
                       "đó (không quá " + n(Hunter::kBlindLandMaxPx) + " px)") &&
              has(doc, "Cú ↑ tại chỗ cho quái cao hơn vạch trên (bản 2026.11.12c) cũng nghỉ thay vì tắt") &&
              has(readme, "`GameMapBook::kBodyPx` (" + n(GameMapBook::kBodyPx) + " px)") &&
              has(readme, "`kBlindLandMs` (" + n(Hunter::kBlindLandMs) + " ms)") &&
              has(readme, "`kBlindLandMaxPx` (" + n(Hunter::kBlindLandMaxPx) + " px)") &&
              has(readme, "`kBarUpFailsPerRun` (5) with more of them failed than worked") &&
              has(book_cpp, "if (walled(last.x + step, last.y)) {") && has(book_cpp, "t.stop = t.wall = true;") &&
              has(hunter, "blind_landing(*z.page, f, clock_now())") &&
              !has(drop, "Hunter: Up for monsters above on again after %llu s") &&
              !has(jump, "bar_fails_run_ >= kBarUpFailsPerRun") && has(jump, "if (fail) ++bar_fails_row_;"),
          "the notes of the file's walls (" + n(GameMapBook::kBodyPx) + " px above the feet), the fall no report follows (" +
              n(Hunter::kBlindLandMs) + " ms, " + n(Hunter::kBlindLandMaxPx) + " px) say what map_book.cpp and "
              "hunter.cpp do; the plain Up's pause, 2026.11.15's, is gone from hunter_jump.cpp (2026.11.24)");
    // The overnight log's file distrusted on one report from the top of an arc
    // (00:27:54 on t60_s1, seven hours out of the area): a landing on no floor
    // waits for the next report -- the notes' seconds are the constants'. The
    // distrust given back after 120 s is 2026.11.15's history: since
    // 2026.11.24 a landing off the floors has MapTrust let the file go until
    // its standing points take it again (trust_.lose), with no clock.
    const std::string settle_vi = n(Hunter::kLandSettleMs / 1000) + "," + n(Hunter::kLandSettleMs % 1000 / 100);
    const std::string distrust_s = "120";
    check(Hunter::kLandSettleMs == 900 &&
              has(doc, "GoBot chờ gói đáp kế tiếp tối đa " + settle_vi + " giây") &&
              has(doc, "hết " + settle_vi + " giây mà không có gì thì kết luận như trước") &&
              has(doc, "File bị coi là sai (đáp ngoài sàn) chỉ bị bỏ " + distrust_s + " giây rồi được tin lại") &&
              has(doc, "(log \"Hunter: t60_s1's map file trusted again after " + distrust_s + " s ...\")") &&
              has(doc, "lần đáp ngoài sàn kế tiếp lại bỏ nó " + distrust_s + " giây nữa") &&
              has(readme, "waits `kLandSettleMs` (" + n(Hunter::kLandSettleMs) + " ms) for the next report") &&
              has(readme, "is given back after `kDistrustMs` (" + distrust_s + " s) by `cool_downs_end`") &&
              has(readme, "\"Hunter: t60_s1's map file trusted again after " + distrust_s + " s ...\"") &&
              has(readme, "a landing off its floors distrusts it again for another " + distrust_s + " s") &&
              has(pack, "it waits for the next report, and a file it did distrust comes back after 2 minutes or at the "
                        "next start") &&
              !has(drop, "distrusted_") && !has(drop, "undistrust(") &&
              !has(drop, "\"Hunter: %s's map file trusted again after %llu s -- ") &&
              has(drop, "trust_.lose(\"a drop landed off the file's floors\");") &&
              has(jump, "trust_.lose(\"a jump up landed off the file's floors\");") &&
              has(drop, "now < f.report_ms + kLandSettleMs") &&
              has(jump, "} else if (now < moved_ms + kLandSettleMs) {") &&
              has(jump, "if (landed && fl.landed && fl.took_off && p != nullptr && now < jump_.key_ms + "
                        "kJumpUpAirMaxMs) {") &&
              has(jump, "if (std::abs(round_px(r.x) - lx) >= kLandSamePx || std::abs(round_px(r.y) - ly) >= "
                        "kLandSamePx)") &&
              Hunter::kLandSamePx == 12 && !has(drop, "distrust_map(") && !has(jump, "distrust_map(") &&
              !has(hunter, "DistrustNote") && !has(header, "kDistrustMs ="),
          "the notes of the landing waited for (" + n(Hunter::kLandSettleMs) + " ms) say what hunter_drop.cpp and "
          "hunter_jump.cpp do; the distrust given back after " + distrust_s + " s is 2026.11.15's, gone since "
          "2026.11.24 -- MapTrust lets the file go instead, no clock");
    // The walls' figures (the review of 2026.11.15 caught t53_s2 as 10:
    // way_back_cover_test counts 47, 30 and 7 on the game's files) and the
    // ground's fill -- a record in a cell the other layer holds a surface of
    // is no wall.
    check(has(readme, "t60_s1 has 47, t49_s5 30, t53_s2 7 (3 more of its 10 attr-4 cells hold a surface, and stay "
                      "floors)") &&
              has(doc, "(t60_s1: 47 ô, t49_s5: 30, t53_s2: 7; ô có mặt sàn giữ như cũ)") &&
              has(readme, "and so does a cell the other layer holds a surface of: a layer-1 record under the ground of "
                          "layer 0 is its fill, not a wall") &&
              has(doc, "Một ô chặn nằm cùng ô với mặt sàn của lớp kia") &&
              has(book_cpp, "if (holds_surface(layer, cx, cy) || holds_surface(1 - layer, cx, cy)) continue;") &&
              !has(readme, "t53_s2 10") && !has(doc, "t53_s2: 10"),
          "the walls' figures -- 47, 30 and 7 -- and the ground's fill under the other layer's surface as no wall: "
          "the notes and map_book.cpp say the same");
    // 2026.11.15c (the first live run, 10:15:51): a report in mid-air is no
    // landing whatever its kind -- the entry on top of section 5, README's
    // section after 15b's, START-HERE's paragraph after 15b's, and the numbers
    // they quote.
    const std::size_t r15b_at = readme.find("### The shop and the party again after 30 s (2026.11.15b)");
    const std::size_t r15c = readme.find("### A report in mid-air is no landing, whatever its kind (2026.11.15c)");
    const std::size_t p15b_at = pack.find("The shop and the party again after 30 s (2026.11.15b, not yet tried on the game).");
    const std::size_t p15c = pack.find("A report in mid-air is no landing (2026.11.15c, not yet tried on the game).");
    const std::string air_vi = n(Hunter::kJumpUpAirMaxMs / 1000) + "," + n(Hunter::kJumpUpAirMaxMs % 1000 / 100);
    check(Hunter::kLandSamePx == 12 && Hunter::kLandSettleMs == 900 && Hunter::kJumpUpAirMaxMs == 2200 &&
              e15c != std::string::npos && r15b_at != std::string::npos && r15c != std::string::npos &&
              rmacro != std::string::npos && r15b_at < r15c && r15c < rmacro && p15b_at != std::string::npos &&
              p15c != std::string::npos && p15b_at < p15c &&
              has(readme, "Since 2026.11.15c (not yet run on the game).") &&
              has(doc, "- Mới trong bản 2026.11.15c (một gói tin giữa không trung không còn bị coi là lần đáp xuống của "
                       "cú nhảy lên;") &&
              has(doc, "nay chờ với mọi loại gói. Cứ mỗi gói sau đó (kể cả gói ra chiêu) đặt nhân vật cách chỗ \"đáp\" "
                       "từ " + n(Hunter::kLandSamePx) + " px trở lên thì thời gian chờ (" + settle_vi +
                           " giây) tính lại từ gói đó, tối đa " + air_vi + " giây sau lúc bấm") &&
              has(readme, "waits `kLandSettleMs` from the newest report of the flight that puts the character "
                          "`kLandSamePx` (" + n(Hunter::kLandSamePx) + " px) or more from it") &&
              has(readme, "no longer than `kJumpUpAirMaxMs` (2.2 s) after the key") &&
              has(pack, "waits for the real landing whatever kind of report came first") &&
              has(jump, "std::uint64_t moved_ms = fl.land.ms;") &&
              has(header, "static constexpr int           kLandSamePx = 12;") &&
              !has(jump, "fl.land.kind == OwnReport::Air && p != nullptr"),
          "2026.11.15c: a report of any kind in mid-air is no landing -- the entry on top of section 5, README's "
          "section and START-HERE's paragraph after 15b's, " + n(Hunter::kLandSamePx) + " px and " + settle_vi +
              " s as the constants have them, and the wait in jump_follow as the notes say");
    // A blind landing's way that starts with a press (the review): the walk
    // takes the first step -- the notes and hunter.cpp say the same.
    check(has(readme, "`ZoneHere::blind` marks the projection, and while it stands `go_home_zone` does not press but "
                      "walks") &&
              has(doc, "GoBot không bấm từ chỗ đứng đoán được") &&
              has(hunter, "z.stand = z.report = landed;") && has(hunter, "z.blind = true;") &&
              has(hunter, "const bool at_drop = !z.blind && way->drop") &&
              has(hunter, "const bool at_take_off = !z.blind && way->jump") &&
              has(hunter, "if (z.blind && dx == 0) dx = way->at.x >= at.x ? 1 : -1;") &&
              has(header, "bool                    blind = false;"),
          "a blind landing's way that starts with a jump up or a drop: no press from the projection, a walk's step "
          "first -- as the notes and hunter.cpp say");
#else
    skip("no source folder\n");
#endif
}

/// The owner's Q2 and Q5 of 2026-11-11: the dash's key held where its landing
/// would run past a floor's end of the trusted map file -- t23_s14 22:27:47
/// and 22:29:10, off the ledge by a 318 and a 213 px move with no walk; not in
/// the middle of a floor.
void test_dash_edge_floor() {
    section("Hunter (2026.11.12): the dash's edge on the trusted map file -- a floor's end within the dash, not the "
            "middle of the floor");
    if (fixture_folder().empty()) return;

    DropRig r(drop_config(120));            // spot off; t52_s2 trusted by its entry
    r.hunter.set_dash('C', 214);
    r.w.place(2600, 371);                   // on the long floor 2300..2900 at 372
    r.run(600);
    const bool trusted = r.st().drop_map.why == MapTrust::Why::Trusted;
    const bool middle = !r.hunter.dash_edge();      // 2386 .. 2814
    r.w.place(2850, 371);
    r.run(300);
    const bool end = r.hunter.dash_edge();          // 3064 past the end at 2900
    r.hunter.set_dash('V', 0);                      // learned with no length: 320 px either way
    r.w.place(2600, 371);
    r.run(300);
    const bool guess = r.hunter.dash_edge();        // 2280 and 2920: past both ends
    r.hunter.set_dash(0, 0);                        // not learned: no key held, no edge said (2026.11.17)
    r.w.place(2850, 371);
    r.run(300);
    const bool none = !r.hunter.dash_edge();
    check(trusted && middle && end && guess && none,
          std::string("the dash learned as C, 214 px: none in the floor's middle (") + (middle ? "yes" : "no") +
              "), held 50 px from its end (" + (end ? "yes" : "no") + "); learned with no length, 320 px, from the "
              "middle of the 600-px floor (" + (guess ? "yes" : "no") + "); not learned, nothing held 50 px from "
              "its end (" + (none ? "yes" : "no") + ")");
}

/// A scenario's step: the character placed at x,y with a fresh monster at
/// mx,my below it, the game answering the next Ctrl + ↓ as `mode`; waits for
/// the press (the minute's 4 presses may take a minute) and 1.5 s after it.
/// Whether a press went.
bool miss_step(DropRig& r, Fall mode, int x, int y, int mx, int my, int slot) {
    r.w.mode = mode;
    r.w.place(x, y);
    r.w.mobs = { mob_at(mx, my, slot) };
    const std::size_t before = r.w.drops.size();
    r.until([&] { return r.w.drops.size() > before; }, 75'000);
    r.run(1500);
    return r.w.drops.size() > before;
}

/// 2026.11.15: the game loses 30-40% of GoBot's short presses at random (the
/// log of 2026-11-15: Ctrl + ↓ did nothing at 2 of 6). One that did nothing
/// is no evidence: a place is learned at the second near it with none that
/// worked between; the first passes the monster over 1.2 s. Three in a row,
/// or five in the run with more failed than worked, paused the drops 30 s, 60
/// s and 120 s from 2026.11.15; since 2026.11.24 (the owner's correction:
/// "không, 2. Nhảy xuống (Ctrl+↓) khi hỏng thì thử lại luôn, hoặc dừng ra
/// skill rồi nhảy xuống chứ không phải là dừng 90s") nothing pauses them: the
/// next press comes at the next look, after one fight turn from the second in
/// a row on (kRetryFightMs), from another column while a place learned is
/// skipped (kLearnedForgetMs), and a monster's two presses count for
/// kTargetCountMs only.
void test_drop_misses_2026_11_15() {
    section("Hunter (2026.11.15, 2026.11.24): one press the game lost is no evidence -- a place learned at the second "
            "near it, skipped 30 s and tried again; never a pause: 8 lost presses in a row each at the next look, the "
            "monster passed over 1.2 s then one fight turn, the minute's cap and the monster's 2 of the last 10 s");

    // The numbers are pure: checked with the game's files or without them.
    check(Hunter::kNoDropSuspectMs == 120'000 && Hunter::kRetryPassOverMs == 1200 && Hunter::kRetryFightMs == 3000 &&
              Hunter::kLearnedForgetMs == 30'000 && Hunter::kTargetCountMs == 10'000 && Hunter::kDropGapMs == 1500 &&
              Hunter::kDropsPerMinute == 4 && Hunter::kDropsPerTarget == 2 && Hunter::kDropSettlesPerTarget == 2 &&
              Hunter::kJumpUpsPerTarget == 2 && Hunter::kBarUpDryPerTarget == 2,
          "the numbers: a note kept 120 s, the first failed press's pass-over 1.2 s, every other's one fight turn of "
          "3 s, a place learned skipped 30 s, a monster's presses counted over 10 s; 1.5 s between presses, 4 a "
          "minute, 2 a monster");
    if (fixture_folder().empty()) return;

    // A fluke: the first press did nothing, the second from the same place
    // worked -- the place is no suspect any more, and a later press that does
    // nothing there is a first again.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.mode = Fall::Solid;
        r.w.mobs = { mob_at(2673, 632, 5) };
        std::vector<std::string> log;
        bool first_noted = false;
        log = info_lines([&] {
            r.until([&] { return !r.w.drops.empty(); }, 3000);
            r.run(1200);
            first_noted = r.w.drops.size() == 1 && !r.book.no_drop_near("t52_s2", { 2675, 371 });
            r.w.mode = Fall::Floor;
            r.until([&] { return r.st().drops_worked == 1; }, 8000);
            r.run(500);
            // Back up there with another monster below: nothing again.
            r.w.mode = Fall::Solid;
            r.age_entry();
            r.w.place(2675, 371);
            r.w.mobs = { mob_at(2673, 632, 6) };
            const std::size_t before = r.w.drops.size();
            r.until([&] { return r.w.drops.size() > before; }, 8000);
            r.run(1500);
        });
        const Hunter::Status st = r.st();
        check(first_noted && st.drops_worked == 1 && r.w.drops.size() == 3 &&
                  !r.book.no_drop_near("t52_s2", { 2675, 371 }) &&
                  lines_with(log, "not held against the place yet") == 2 &&
                  lines_with(log, "no drops within 64 px of it this session") == 0 &&
                  lines_with(log, "the monster passed over for 1.2 s (1 in a row)") == 2,
              fmt("nothing, then it worked there, then nothing again: three presses, the place never learned (%d "
                  "\"not held against the place yet\", each the first of its row)",
                  lines_with(log, "not held against the place yet")));
    }
    // The note ages: a second press that did nothing more than 120 s after the
    // first is a first again.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Solid;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.until([&] { return !r.w.drops.empty(); }, 3000);
        r.run(1500);
        r.w.mobs.clear();
        r.run(Hunter::kNoDropSuspectMs + 5000);
        r.w.place(2675, 371);
        r.w.mobs = { mob_at(2673, 632, 6) };
        r.until([&] { return r.w.drops.size() > 1; }, 8000);
        r.run(1500);
        check(r.w.drops.size() == 2 && !r.book.no_drop_near("t52_s2", { 2675, 371 }),
              "nothing, 125 s on nothing again at the same place: the note is gone -- the place not learned");
    }
    // 3, 5 and 8 lost presses in a row, each from a place of its own (more
    // than kNoDropXPx apart, so no place is learned) for a fresh monster:
    // never a pause (2026.11.24; 30 s after the third, then 60 s, from
    // 2026.11.15) -- each next press at the next look, kDropGapMs or more after
    // the last; and more than kDropsPerMinute in the minute: a press that did
    // nothing leaves the minute's count, so it never holds the next back.
    {
        DropRig r(drop_config(40));
        r.age_entry();
        struct Place {
            int x, y, mx, my;
        };
        const Place places[] = {
            { 2330, 371, 2335, 983 }, { 2400, 371, 2405, 983 }, { 2470, 371, 2475, 983 },
            { 2575, 371, 2580, 983 }, { 2640, 371, 2645, 983 }, { 2705, 371, 2710, 983 },
            { 2770, 371, 2775, 983 }, { 2400, 237, 2405, 371 },
        };
        int slot = 30;
        std::string got;
        const std::uint64_t t0 = r.w.t;
        const std::vector<std::string> log = info_lines([&] {
            for (const Place& p : places)
                got += miss_step(r, Fall::Solid, p.x, p.y, p.mx, p.my, ++slot) ? "x" : "-";
        });
        bool spaced = r.w.drops.size() == 8;
        std::uint64_t widest = 0;
        for (std::size_t i = 1; i < r.w.drops.size(); ++i) {
            const std::uint64_t gap = r.w.drops[i] - r.w.drops[i - 1];
            spaced = spaced && gap >= Hunter::kDropGapMs;
            widest = std::max(widest, gap);
        }
        int in_first_minute = 0;
        for (const std::uint64_t d : r.w.drops)
            if (d < r.w.drops.front() + 60'000) ++in_first_minute;
        check(got == "xxxxxxxx" && spaced && widest < 4000 && in_first_minute > Hunter::kDropsPerMinute,
              fmt("8 places, 8 presses that did nothing (%s) in %llu s: each %llu ms or more after the last, the widest "
                  "gap %llu ms; %d in the first minute, more than kDropsPerMinute (%d)",
                  got.c_str(), static_cast<unsigned long long>((r.w.t - t0) / 1000),
                  static_cast<unsigned long long>(Hunter::kDropGapMs), static_cast<unsigned long long>(widest),
                  in_first_minute, Hunter::kDropsPerMinute));
        check(lines_with(log, "(3 in a row)") == 1 && lines_with(log, "(5 in a row)") == 1 &&
                  lines_with(log, "(8 in a row)") == 1 && lines_with(log, "drops paused") == 0 &&
                  lines_with(log, "on again after") == 0 && lines_with(log, "OFF until") == 0 &&
                  lines_with(log, "minute's used up") == 0,
              "the 3rd, the 5th and the 8th in a row said as such -- never a pause, never \"OFF\", never the minute's "
              "cap");
        check(!r.book.no_drop_near("t52_s2", { 2330, 371 }) && !r.book.no_drop_near("t52_s2", { 2575, 371 }) &&
                  !r.book.no_drop_near("t52_s2", { 2400, 237 }),
              "every place more than kNoDropXPx from the others: a first each -- none learned");
    }
    // One monster, one place: the first press passed over kRetryPassOverMs,
    // the second -- the place's second -- learns it and passes the monster
    // over one fight turn (kRetryFightMs). The monster gone 90 px along: the
    // next press from another column, kNoDropXPx or more from the place
    // learned. kLearnedForgetMs on the place is forgotten -- said -- and
    // pressed again.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Solid;
        r.w.step_px = 10;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        std::vector<std::string> log = info_lines([&] {
            r.until([&] { return r.w.drops.size() >= 2; }, 8000);
            r.run(1200);
        });
        const std::uint64_t gap = r.w.drops.size() >= 2 ? r.w.drops[1] - r.w.drops[0] : 0;
        const bool learned = r.book.no_drop_near("t52_s2", { 2675, 371 });
        check(r.w.drops.size() == 2 && gap >= Hunter::kDropGapMs && gap < Hunter::kRetryPassOverMs + 3000 && learned &&
                  lines_with(log, "the monster passed over for 1.2 s (1 in a row)") == 1 &&
                  lines_with(log, "the monster passed over for 3 s: a fight turn, then tried again (2 in a row)") == 1,
              fmt("the first press passed over 1.2 s -- the second %llu ms on, the place learned, the monster passed "
                  "over one fight turn",
                  static_cast<unsigned long long>(gap)));
        // The monster along the low floor, 90 px left: pressed from another
        // column -- not near the place learned.
        r.w.mode = Fall::Solid;
        r.w.mobs = { mob_at(2585, 983, 6) };
        int third_x = 0;
        log = info_lines([&] {
            r.until([&] {
                if (r.w.drops.size() >= 3 && third_x == 0) third_x = r.w.me_x;
                return r.w.drops.size() >= 3;
            },
                    8000);
            r.run(1200);
        });
        check(r.w.drops.size() == 3 && std::abs(third_x - 2675) > GameMapBook::kNoDropXPx,
              fmt("the next press from another column: x %d, %d px from the place learned (kNoDropXPx %d)", third_x,
                  std::abs(third_x - 2675), GameMapBook::kNoDropXPx));
        // kLearnedForgetMs after it was learned: forgotten, said -- and pressed
        // there again.
        r.w.mobs.clear();
        log = info_lines([&] { r.run(Hunter::kLearnedForgetMs); });
        const bool forgot = !r.book.no_drop_near("t52_s2", { 2675, 371 }) &&
                            lines_with(log, "Hunter: the place at 2675,371 on t52_s2 where Ctrl+Down did not work is "
                                            "tried again (30 s on)") == 1;
        r.w.place(2675, 371);
        r.w.mobs = { mob_at(2673, 632, 7) };
        r.until([&] { return r.w.drops.size() >= 4; }, 6000);
        check(forgot && r.w.drops.size() == 4,
              fmt("the place forgotten %llu s on (\"%s\") and pressed there again (%zu presses)",
                  static_cast<unsigned long long>(Hunter::kLearnedForgetMs / 1000),
                  line_with(log, "is tried again").c_str(), r.w.drops.size()));
    }
    // Two presses for one monster, from two places (neither learned): the
    // monster's kDropsPerTarget used up -- TargetCap -- until the last of them
    // is kTargetCountMs old, then tried again (2026.11.24; for the rest of the
    // run until then). give_up_ms long: no give-up between.
    {
        HunterConfig c = drop_config(40);
        c.give_up_ms = 30'000;
        DropRig r(c);
        r.w.mode = Fall::Solid;
        r.age_entry();
        r.w.place(2580, 371);
        r.w.mobs = { mob_at(2640, 983, 9) };
        r.until([&] { return r.w.drops.size() >= 1; }, 4000);
        r.run(300);
        r.w.place(2700, 371);
        r.until([&] { return r.w.drops.size() >= 2; }, 6000);
        r.run(300);
        r.w.place(2645, 371);
        bool capped = false;
        r.until([&] {
            if (r.w.drops.size() == 2 && r.st().drop_wait == Why::TargetCap) capped = true;
            return r.w.drops.size() >= 3;
        },
                Hunter::kTargetCountMs + 8000);
        const std::uint64_t again = r.w.drops.size() >= 3 ? r.w.drops[2] - r.w.drops[1] : 0;
        check(r.w.drops.size() == 3 && capped && again >= Hunter::kTargetCountMs &&
                  again < Hunter::kTargetCountMs + 4000,
              fmt("two presses for the monster: TargetCap (%s) -- the third %llu ms after the second, once they were "
                  "%llu s old",
                  capped ? "said" : "not seen", static_cast<unsigned long long>(again),
                  static_cast<unsigned long long>(Hunter::kTargetCountMs / 1000)));
    }
}

} // namespace

void test_drop_hunt(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_drop_rule();
    test_drop_owner_cases();
    test_drop_choosing();
    test_drop_verdicts();
    test_drop_misses_2026_11_15();
    test_drop_landings_2026_11_12();
    test_drop_spot_on_floor();
    test_drop_band_2026_11_12();
    test_dash_edge_floor();
    test_drop_safety();
    test_drop_spot_and_limits();
    test_drop_config();
    test_drop_window();
    test_drop_apex_report();
    test_drop_notes_2026_11_15();
}
