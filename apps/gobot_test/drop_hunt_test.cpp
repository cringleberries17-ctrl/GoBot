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
        for (int x = x0; x <= x1; ++x) {
            const std::size_t r = 8 + static_cast<std::size_t>(layer) * static_cast<std::size_t>(w) *
                                          static_cast<std::size_t>(h) * 36 +
                                  (static_cast<std::size_t>(x / 32) * static_cast<std::size_t>(h) +
                                   static_cast<std::size_t>(y / 32)) * 36;
            b[r + 2] = attr;
            b[r + 3 + static_cast<std::size_t>(x % 32)] = static_cast<std::uint8_t>(y % 32);
        }
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
    const bool ok = write_bytes(dir + "\\Map\\t52_s2.map", map.b) &&
                    write_bytes(dir + "\\Project\\t52_s2.prj", prj.b) &&
                    write_bytes(dir + "\\Map\\t99_s9.map", decoy.b);
    if (!ok) dir.clear();
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
        { "drops off for the run", [](HunterConfig&, Hunter::DropFacts& f) { f.latched = true; }, Why::Latched },
        { "a key-up owed", [](HunterConfig&, Hunter::DropFacts& f) { f.owed = true; }, Why::KeysOwed },
        { "the jump key Ctrl", [](HunterConfig& c, Hunter::DropFacts&) { c.jump_key = 0x11; }, Why::KeyClash },
        { "the left key right Ctrl", [](HunterConfig& c, Hunter::DropFacts&) { c.left_key = 0xA3; }, Why::KeyClash },
        { "the attack key left Ctrl", [](HunterConfig& c, Hunter::DropFacts&) { c.attack_key = 0xA2; }, Why::KeyClash },
        { "no map book or veto", [](HunterConfig&, Hunter::DropFacts& f) { f.wired = false; }, Why::NotWired },
        { "a screen plate", [](HunterConfig&, Hunter::DropFacts& f) { f.packet_target = false; },
          Why::NotPacketTarget },
        { "a monster on the level", [](HunterConfig&, Hunter::DropFacts& f) { f.level_monster = true; },
          Why::LevelMonster },
        { "100 px below", [](HunterConfig&, Hunter::DropFacts& f) { f.dy = 100; }, Why::NotBelow },
        { "140 px below with down() 150", [](HunterConfig& c, Hunter::DropFacts& f) { c.down_reach = 150; f.dy = 140; },
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
        { "the floor below 52 px down, as a settle begins",
          [](HunterConfig&, Hunter::DropFacts& f) {
              f.press = false;
              f.fix.y = 320.0f;
          }, Why::ShortFall },
        { "the landing 13 px past the monster", [](HunterConfig&, Hunter::DropFacts& f) { f.target_wy = 620.0f; },
          Why::PastMonster },
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
        { "101 px below", [](HunterConfig&, Hunter::DropFacts& f) { f.dy = 101; }, Why::None },
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
        f.area.x = 2675.0f;
        f.area.y = 371.0f;
        f.area.half_w = 300;
        f.area.half_h = 300;
        check(Hunter::drop_rule(c, f).ok(), "the spot in use, its area 300 px either way: it goes");
        f.area.half_h = 100;
        check(Hunter::drop_rule(c, f).why == Why::OutsideArea, "100 px up and down: the monster outside it -- refused");
        f.area.half_h = 300;
        f.area.x = 2400.0f;
        f.area.half_w = 200;
        check(Hunter::drop_rule(c, f).why == Why::OutsideArea, "standing outside its width -- refused");
        f.area.x = 2675.0f;
        f.area.half_w = 300;
        f.area.half_h = 256;
        f.target_wy = 625.0f;
        const Hunter::DropCall land_out = Hunter::drop_rule(c, f);
        check(land_out.why == Why::OutsideArea && land_out.land_y == 633,
              "the monster inside it 254 px down, the landing on 633 outside it: refused");
        f.spot_use = false;
        check(Hunter::drop_rule(c, f).why == Why::Spot, "the spot on but standing for want of a fix: refused");
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
        std::vector<Nameplate> shallow = mobs;
        shallow[1].bar.y = 371 + 90;
        shallow[1].plate.y = shallow[1].bar.y - 14;
        check(Hunter::choose_target(c, shallow, &me, free2, nullptr, true) == 0,
              "90 px below -- not past drop_min_px 100: the nearest, as ever");
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
};

/// The pretend game, in the packets' world pixels: the plates are laid one
/// to one over them.
struct DropWorld {
    std::uint64_t t = 5'000'000;
    bool          focused = true;
    int           me_x = 2675, me_y = 371;
    std::vector<Nameplate> mobs;
    /// Monsters standing beyond the plates' box: standing points only. And
    /// whether the plates' own monsters are standing points too.
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

    explicit DropRig(const HunterConfig& cfg, bool wired = true) : book(fixture_folder()) {
        w.page = book.get("t52_s2").get();
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
                    out.push_back(
                        MapTrust::Mob{ MapPoint{ n.x(), n.y() }, static_cast<std::uint32_t>(n.slot), w.stand_ms });
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
        check(minute.find("; drops 1 (1 worked, 0 did nothing, 0 jumps in place, 0 rose, 0 off the map, 0 map "
                          "changes, 0 unknown)") != std::string::npos,
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
    // the edge of its floor, to where the file has the same floor 24 px
    // either side.
    {
        DropRig r(drop_config(120));
        r.w.step_px = 10;
        r.w.place(2560, 371);
        r.age_entry();
        r.w.mobs = { mob_at(2690, 632, 5) };
        r.run(5000);
        check(r.w.drops.size() == 1 && r.w.me_y == 632 && r.w.me_x >= 2658 + Hunter::kDropMarginSettlePx &&
                  r.w.me_x < 2812 - 120,
              fmt("from the left, 2560: one drop at %d, on 632 -- never over the floor 984 below", r.w.me_x));
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
    section("Hunter: what came of a drop -- did nothing, jumped in place, rose, off the map, the map changed");
    if (fixture_folder().empty()) return;

    // Ctrl + ↓ did nothing: judged at 1 s, the place remembered, the monster
    // passed over -- and not tried there again.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Solid;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.drops.empty(); }, 3000);
            r.run(1200);
        });
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && r.book.no_drop_near("t52_s2", { 2675, 371 }) && st.drops == 1 &&
                  lines_with(log, "Hunter: drop did nothing in 1000 ms at 2675,371 -- no drops within 64 px of it "
                                  "this session; the monster passed over for 8 s (1 in a row)") == 1,
              "Solid: one press, judged Nothing at 1 s, 2675,371 learned as a no-drop point");
        check(st.targets_dropped == 0 && st.state == Hunter::State::NoTargets && st.passed_over,
              "the monster passed over -- not counted as given up: the minute line counts it among the drops");
        // Another monster below, past where the first is passed over: the
        // character, standing where it pressed, does not press there again.
        r.w.mobs = { mob_at(2730, 632, 8) };
        log = info_lines([&] { r.run(3000); });
        check(r.w.drops.size() == 1 && r.st().drop_wait == Why::NoDropHere,
              fmt("another beneath the same place: not pressed there again (%s)",
                  Hunter::drop_why_name(r.st().drop_wait)));
    }
    // Three in a row did nothing: off until the next start.
    {
        DropRig r(drop_config(40));
        r.w.mode = Fall::Solid;
        r.w.place(2600, 371);
        r.age_entry();
        const struct {
            int x, mx, slot;
        } spots[] = { { 2600, 2610, 11 }, { 2350, 2355, 12 }, { 2480, 2485, 13 } };
        std::vector<std::string> log;
        for (const auto& s : spots) {
            r.w.place(s.x, 371);
            r.w.mobs = { mob_at(s.mx, 983, s.slot) };
            const std::size_t before = r.w.drops.size();
            const std::vector<std::string> l =
                info_lines([&] { r.until([&] { return r.w.drops.size() > before; }, 4000); r.run(1200); });
            log.insert(log.end(), l.begin(), l.end());
        }
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 3 && st.drop_off == Hunter::DropOff::Fails,
              fmt("three places, three presses that did nothing (%zu): drops off (%s)", r.w.drops.size(),
                  Hunter::drop_off_name(st.drop_off)));
        check(lines_with(log, "3 presses of Ctrl+Down in a row did not drop -- drops OFF until the next BẮT ĐẦU") == 1,
              "said once");
        r.w.place(2700, 371);
        r.w.mobs = { mob_at(2705, 983, 14) };
        r.run(4000);
        check(r.w.drops.size() == 3 && r.st().drop_wait == Why::Latched, "a fourth place: no press");
        // A new start clears it.
        r.hunter.stop();
        r.hunter.start_stepped();
        r.run(Hunter::kDropAfterEntryMs + 3000);
        check(r.w.drops.size() == 4 && r.st().drop_off == Hunter::DropOff::None,
              "BẮT ĐẦU again: the latch is gone, and the fourth place is pressed");
    }
    // Five in a run did nothing, never three in a row -- a drop that worked
    // between each two: off until the next start all the same. Each place
    // that did nothing more than 64 px from the others, a fresh monster
    // each time, and no more than 4 presses a minute: over two minutes.
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
        check(got == "SSFSSFS" && st.drops_worked == 2 && st.drop_off == Hunter::DropOff::Fails,
              fmt("did nothing, did nothing, worked, twice more, worked, did nothing (%s): drops off (%s)",
                  got.c_str(), Hunter::drop_off_name(st.drop_off)));
        check(lines_with(log, "5 presses of Ctrl+Down this run did not drop -- drops OFF until the next") == 1 &&
                  lines_with(log, "in a row did not drop") == 0,
              "said once: 5 this run, never 3 in a row");
    }
    // Down held past kDropHeldMaxMs: off until the next start, with a WARN.
    {
        DropRig r(drop_config(136));
        r.w.held_ms = 200;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        std::vector<std::string> log = info_lines([&] { r.run(3000); });
        const bool off = r.w.drop_calls == 1 && r.st().drop_off == Hunter::DropOff::HeldLong &&
                         lines_with(log, "Hunter: Down was held 200 ms (over 180) -- drops OFF until the next") == 1;
        // Back up on the long floor, a fresh monster below: no press.
        r.w.place(2675, 371);
        r.w.mobs = { mob_at(2673, 632, 6) };
        r.run(6000);
        const int calls = r.w.drop_calls;
        const Hunter::DropWhy wait = r.st().drop_wait;
        r.hunter.stop();
        r.hunter.start_stepped();
        r.run(Hunter::kDropAfterEntryMs + 3000);
        check(off && calls == 1 && wait == Why::Latched && r.w.drop_calls == 2,
              fmt("Down held 200 ms: drops off (%s), no press in 6 s after (%d; %s) -- BẮT ĐẦU again, pressed",
                  Hunter::drop_off_name(Hunter::DropOff::HeldLong), calls, Hunter::drop_why_name(wait)));
    }
    // A jump in place: remembered, a fail.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::JumpInPlace;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(3000); });
        check(r.w.drops.size() == 1 && r.book.no_drop_near("t52_s2", { 2675, 371 }) &&
                  lines_with(log, "Hunter: drop was a jump in place (landed at 2675,371)") == 1 &&
                  r.st().drop_off == Hunter::DropOff::None,
              "a jump in place: one press, the place learned, drops still on");
    }
    // A rise: off at once.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Rise;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(5000); });
        check(r.w.drops.size() == 1 && r.st().drop_off == Hunter::DropOff::Rose &&
                  lines_with(log, "Hunter: drop ROSE to 2675,251") == 1,
              "a rise: one press, drops off at once");
    }
    // Off the map: the file said 633 and it landed 300 px lower.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::Overshoot;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(5000); });
        check(r.w.drops.size() == 1 && r.st().drop_off == Hunter::DropOff::OffMap && r.book.distrusted("t52_s2") &&
                  lines_with(log, "not on floor 633 the map file gave -- drops OFF") == 1,
              "off the map: drops off, and t52_s2's file not trusted again this session");
        check(r.st().drop_map.why != MapTrust::Why::Trusted, "the map no longer trusted");
    }
    // The map changed: the hunter halts for the window to stop it.
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
        check(r.w.drops.size() == 1 && st.drop_map_changed && r.hunter.drop_stop() == Hunter::DropStop::MapChanged &&
                  st.drop_off == Hunter::DropOff::MapChanged,
              "the map changed at the press: drop_stop() MapChanged, drops off");
        check(learned("t52_s2", 2675, 371) && learned("t53_s2", 2635, 1188),
              "both ends learned as gates: 2675,371 on t52_s2, 2635,1188 on t53_s2");
        check(lines_with(log, "Hunter: MAP CHANGED ", "after Ctrl+Down at 2675,371 on t52_s2 -> map 53/2 at "
                                                     "2635,1188 -- hunting STOPS") == 1,
              "the WARN: \"" + line_with(log, "MAP CHANGED") + "\"");
        const std::size_t keys = r.w.keys.size();
        r.w.mobs = { mob_at(2700, 1450, 9), mob_at(2900, 1188, 10) };
        r.run(10'000);
        check(r.w.keys.size() == keys && r.w.drops.size() == 1 && r.st().state == Hunter::State::Idle,
              "halted: not one key more in 10 s, a monster on the level in view or not");
    }
    // Late: nothing at first, then the map changes 2.5 s on -- the press's
    // all the same, and what Nothing learned is undone.
    {
        DropRig r(drop_config(136));
        r.w.mode = Fall::LateGate;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.until([&] { return !r.w.drops.empty(); }, 3000);
        r.run(1500);
        const bool nothing = r.book.no_drop_near("t52_s2", { 2675, 371 });
        r.run(2000);
        const Hunter::Status st = r.st();
        check(nothing && !r.book.no_drop_near("t52_s2", { 2675, 371 }) && st.drop_map_changed &&
                  r.hunter.drop_stop() == Hunter::DropStop::MapChanged,
              "Nothing at 1 s learned a no-drop point; the map change at 2.5 s overrode it and took it back");
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
        check(r.w.drops.size() == 1 && !r.st().drop_map_changed && r.hunter.drop_stop() == Hunter::DropStop::None,
              "a map change 13 s after the press: a door walked through, not the drop's");
    }
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
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(3000);
        const bool unknown = r.w.drop_calls == 0 && r.st().drop_wait == Why::MapUnknown;
        // A dozen more monsters standing about the map's floors.
        for (int i = 0; i < 12; ++i)
            r.w.far_stands.push_back(MapTrust::Mob{ MapPoint{ 2210 + 40 * i, 983 }, static_cast<std::uint32_t>(100 + i),
                                                    r.w.t });
        r.run(4000);
        const Hunter::Status st = r.st();
        check(unknown && st.drop_map.how == MapTrust::How::Named && st.drop_map.stem == "t52_s2" &&
                  r.w.drops.size() == 1,
              fmt("no map entry seen: no drop (%s), until 12 standing points named t52_s2 -- then one",
                  unknown ? "map not known" : Hunter::drop_why_name(st.drop_wait)));
    }
    // An entry by no gate: trusted after three standing points apart.
    {
        DropRig r(drop_config(136));
        r.w.entry_x = 1000.0f;
        r.w.entry_y = 274.0f;
        r.age_entry();
        r.w.mobs = { mob_at(2673, 632, 5) };
        r.run(3000);
        const bool waits = r.w.drop_calls == 0 && r.st().drop_map.why == MapTrust::Why::Confirming;
        r.w.far_stands.push_back(MapTrust::Mob{ MapPoint{ 2350, 983 }, 200u, r.w.t });
        r.run(3000);
        check(waits && r.st().drop_map.how == MapTrust::How::Points && r.w.drops.size() == 1,
              "a map entry by no gate: no drop on two standing points, the drop after a third");
    }
    // A key-up owed: no drop, then the hunter halts.
    {
        DropRig r(drop_config(136));
        r.age_entry();
        r.w.owed = true;
        r.w.mobs = { mob_at(2673, 632, 5) };
        const std::vector<std::string> log = info_lines([&] { r.run(2000); });
        check(r.w.drop_calls == 0 && r.hunter.drop_stop() == Hunter::DropStop::KeysStuck && r.st().drop_keys_stuck &&
                  lines_with(log, "Hunter: keys owed after a drop") == 1,
              "a drop's key-up owed three passes: no drop, the hunter halts for the window to stop the hunt");
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
        const std::vector<std::string> log = info_lines([&] {
            r.run(15'000, [&] {
                if (gave_at == 0 && r.st().targets_dropped > 0) gave_at = r.w.t;
            });
        });
        const Hunter::Status st = r.st();
        check(r.w.drop_calls == 0 && st.targets_dropped >= 1 && lines_with(log, "Hunter: gave up on ") == 1 &&
                  st.drop_wait == Why::TargetCap,
              fmt("in the air at every press: no press, given up on %llu time(s), said %d time(s) in 15 s, the "
                  "settles for it used up (%s)",
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
    section("HunterConfig: drop_down and its three numbers in hunt.ini");
    HunterConfig d;
    check(!d.drop_down && d.drop_min_px == 100 && d.drop_side_px == 120 && d.drop_gate_px == 150,
          "the code's defaults: off, 100, 120, 150");
    const std::string path = temp_path("drop_hunt.ini");
    {
        std::ofstream(path) << "[Hunt]\nreach=150\n";
        HunterConfig old;
        check(old.load(path) && !old.drop_down && old.drop_min_px == 100 && old.drop_side_px == 120 &&
                  old.drop_gate_px == 150,
              "a hunt.ini without them: off, at the defaults");
    }
    {
        HunterConfig c;
        c.drop_down = true;
        c.drop_min_px = 130;
        c.drop_side_px = 90;
        c.drop_gate_px = 60;
        HunterConfig back;
        check(c.save(path) && back.load(path) && back.drop_down && back.drop_min_px == 130 &&
                  back.drop_side_px == 90 && back.drop_gate_px == 60,
              "saved and read back");
    }
    {
        std::ofstream(path) << "[Hunt]\ndrop_down=true\ndrop_min_px=5\ndrop_side_px=9999\ndrop_gate_px=1\n";
        HunterConfig lo;
        const bool read_lo = lo.load(path);
        std::ofstream(path) << "[Hunt]\ndrop_min_px=900\ndrop_side_px=3\ndrop_gate_px=4000\n";
        HunterConfig hi;
        check(read_lo && hi.load(path) && lo.drop_min_px == 60 && lo.drop_side_px == 300 && lo.drop_gate_px == 40 &&
                  hi.drop_min_px == 470 && hi.drop_side_px == 20 && hi.drop_gate_px == 400,
              "kept within 60-470, 20-300 and 40-400");
    }
    DeleteFileA(path.c_str());

#ifdef GOBOT_SOURCE_DIR
    const std::string shipped = std::string(GOBOT_SOURCE_DIR) + "\\configs\\hunt.ini";
    HunterConfig ship;
    IniFile ini;
    check(ship.load(shipped) && ini.load(shipped) && ini.has_key("Hunt", "drop_down") &&
              ini.has_key("Hunt", "drop_min_px") && ini.has_key("Hunt", "drop_side_px") &&
              ini.has_key("Hunt", "drop_gate_px") && !ship.drop_down && ship.drop_min_px == 100 &&
              ship.drop_side_px == 120 && ship.drop_gate_px == 150,
          "the shipped hunt.ini: drop_down=false, 100, 120, 150 (the player's answer 8)");
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
    const std::size_t stop = dtick.find("const Hunter::DropStop stop = hunter_->drop_stop();");
    const std::size_t halt = stop == npos ? npos : dtick.find("stop_hunt();", stop);
    const std::size_t changed = halt == npos ? npos : dtick.find("if (stop == Hunter::DropStop::MapChanged) {", halt);
    const std::size_t untick = changed == npos ? npos : dtick.find("hunt_config_.drop_down = false;", changed);
    const std::size_t save =
        untick == npos ? npos : dtick.find("hunt_config_.save(exe_relative(\"configs/hunt.ini\"))", untick);
    check(gtick.find("drop_tick();") != npos && stop != npos && halt != npos && changed != npos && untick != npos &&
              save != npos && dtick.find("GB_WARN(\"Hunter: Nhảy xuống unticked") != npos &&
              dtick.find("drop_stopped_vi_ = std::string(\"Lần nhảy lúc \")") != npos,
          "tick(): a map change after a press stops the hunt, unticks drop_down and saves hunt.ini, with a WARN "
          "and a red line (the player's answer 3)");
    check(dtick.find("GB_WARN(\"Hunter: Nhảy xuống: a drop's Down or Ctrl up stayed owed") != npos,
          "and a key-up stuck stops it too");
    const std::string veto = body(gui, "const char* GuiApp::drop_veto_now() const {");
    check(veto.find("hp_percent_now()") != npos && veto.find("chat_hp_floor_") != npos &&
              veto.find("drop_veto_.load()") != npos,
          "the veto: HP under the chat's floor or not known, and what tick() saw -- the macro held, a walk, the "
          "keeper crossing, a GoBot key on Ctrl or Down");
    check(keys.find("if (hunt_config_.drop_down) k.push_back({ VK_CONTROL, \"nhảy xuống (Ctrl + ↓)\" });") != npos,
          "Ctrl among the keys GoBot presses, with drop_down on");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_drop_hunt(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_drop_rule();
    test_drop_owner_cases();
    test_drop_choosing();
    test_drop_verdicts();
    test_drop_safety();
    test_drop_spot_and_limits();
    test_drop_config();
    test_drop_window();
}
