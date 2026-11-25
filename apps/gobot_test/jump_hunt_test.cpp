// gobot_test -- the hunter's jump up onto a floor above (2026.11.05b, the
// jump-up plan's §6-§9 and §12), over the game's own t52_s2 and a made-up
// staircase: the way back's Ctrl + ↑ from the ground to the home ledge
// (04d's walk back from 1342,772, replayed), the monster on the ledge seen
// from 600,579, falls back and what they teach, a key the game did not take
// as its jump, the latches, a map change after the key, ↑↑ where it is sure,
// the rule's refusals one at a time, and with jump_up off nothing pressed
// that 05 did not press -- one "would jump" line instead.
//
// Over copies of the game's map files in build\handoff\drop\testdata --
// never the game's install, never the registry -- and skipped when they are
// not there; the staircase is written to this run's temp folder. The hunter
// runs on a pretend game laid one to one over the packets' world: the
// character walks along the real floors, and a jump's key, through the
// hunter's Hands::jump, has it rise and come down as the model says --
// reported as the client reports it, by kind (0x002A states 1, 6, 2 and 0).
// Nothing here presses a real key: every key goes to the pretend game and is
// written down.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_book.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/packet_world.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
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
    char buf[900];
    std::snprintf(buf, sizeof buf, f, args...);
    return buf;
}

using Why = Hunter::JumpUpWhy;
using Verdict = Hunter::JumpUpVerdict;

/// The game's files copied for the tests, or "" -- never the game's install.
std::string game_data() {
#ifdef GOBOT_SOURCE_DIR
    const std::string dir = std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data";
    if (path_exists(dir + "\\Map\\t52_s2.map") && path_exists(dir + "\\Project\\t52_s2.prj")) return dir;
#endif
    return {};
}

std::vector<std::string> info_lines(const std::function<void()>& run, LogLevel level = LogLevel::Info) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(6000);
    log.clear_ring();
    log.set_level(level);
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

/// A plate from the packets standing at x, y.
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

/// The standing point on the floor of column x nearest y: its top - 1.
int stand_y(const GameMapBook::Page& p, int x, int y) {
    int best = y, off = INT_MAX;
    for (const MapFloor& f : p.floors_at(x))
        if (std::abs(f.y - 1 - y) < off) {
            off = std::abs(f.y - 1 - y);
            best = f.y - 1;
        }
    return best;
}

// ------------------------------------------------------------- the staircase

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

/// A .prj as the game's: a gate kind, and one placing of it.
struct PrjMaker {
    Bytes b;
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void text(const char* s) {
        for (const char* p = s; *p; ++p) b.push_back(static_cast<std::uint8_t>(*p));
        b.push_back(0);
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

/// t98_s1, made up: solid ground at 1000 and one platform 180 px over it --
/// higher than ↑ lands for sure (109), within ↑↑'s 201 and under its
/// uncertain band -- with a gate by the entry's spawn. Written once a run.
std::string staircase_folder() {
    static std::string dir;
    if (!dir.empty()) return dir;
    dir = temp_path("jump_hunt");
    make_folder(dir);
    make_folder(dir + "\\Map");
    make_folder(dir + "\\Project");
    MapMaker map(80, 40);
    map.flat(0, 0, 2559, 1000, 2);      // the ground, solid
    map.flat(1, 600, 1500, 820, 1);     // the platform
    PrjMaker prj;
    prj.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
    prj.place(3900000, 2000, 992);
    const bool ok = write_bytes(dir + "\\Map\\t98_s1.map", map.b) && write_bytes(dir + "\\Project\\t98_s1.prj", prj.b);
    if (!ok) dir.clear();
    return dir;
}

/// t97_s1, made up: solid ground at 1000 and one platform 100 px over it --
/// within one jump (Hunter::kOneJumpPx) and a plain ↑'s sure 109 -- with a
/// gate by the entry's spawn. Written once a run.
std::string hop_floor_folder() {
    static std::string dir;
    if (!dir.empty()) return dir;
    dir = temp_path("jump_hop");
    make_folder(dir);
    make_folder(dir + "\\Map");
    make_folder(dir + "\\Project");
    MapMaker map(80, 40);
    map.flat(0, 0, 2559, 1000, 2);      // the ground, solid
    map.flat(1, 600, 1500, 900, 1);     // the platform
    PrjMaker prj;
    prj.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
    prj.place(3900000, 2000, 992);
    const bool ok = write_bytes(dir + "\\Map\\t97_s1.map", map.b) && write_bytes(dir + "\\Project\\t97_s1.prj", prj.b);
    if (!ok) dir.clear();
    return dir;
}

/// 2026.11.12, the owner's Q4, Q5 and Q7, made up -- each with a gate by the
/// entry's spawn at 2000: t93_s1, a platform 50 px over solid ground 1000, x
/// 600..1500; t96_s1, two platforms over solid ground 1000 -- one
/// at 860, x 300..1300, and the home's at 720, x 400..1100 -- for a fall two
/// levels down; t95_s1, a floor at 1000 to x 300, a ramp down to 1200 at x 800
/// (0.4 px a column) and on at 1200, solid ground at 1270 under it all -- the
/// spot 33 px over its floor, the character 130 px lower on the ramp; t94_s1,
/// a platform at 800 to x 1000 with nothing at all under it: a pit's edge.
/// And t62_s5: the game's own file (build\handoff), with its two gates.
/// Written once a run.
std::string way_back_folder() {
    static std::string dir;
    if (!dir.empty()) return dir;
    dir = temp_path("way_back");
    make_folder(dir);
    make_folder(dir + "\\Map");
    make_folder(dir + "\\Project");
    PrjMaker gate;
    gate.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
    gate.place(3900000, 2000, 992);
    MapMaker two(80, 40);
    two.flat(0, 0, 2559, 1000, 2);
    two.flat(1, 300, 1300, 860, 1);
    two.flat(1, 400, 1100, 720, 1);
    MapMaker ramp(80, 40);
    ramp.flat(0, 0, 299, 1000, 1);
    for (int x = 300; x <= 800; ++x)
        ramp.column(0, x, 1000 + static_cast<int>(std::lround((x - 300) * 0.4)), 1);
    ramp.flat(0, 801, 2559, 1200, 1);
    ramp.flat(1, 0, 2559, 1270, 2);
    PrjMaker ramp_gate;
    ramp_gate.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
    ramp_gate.place(3900000, 2000, 1192);
    MapMaker pit(80, 40);
    pit.flat(1, 0, 1000, 800, 1);
    PrjMaker pit_gate;
    pit_gate.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
    pit_gate.place(3900000, 200, 792);
    // The owner's Q7 (2026.11.12): t93_s1, a platform at 950, x 600..1500,
    // 50 px over solid ground at 1000.
    MapMaker low(80, 40);
    low.flat(0, 0, 2559, 1000, 2);
    low.flat(1, 600, 1500, 950, 1);
    // The review of 2026.11.12: t92_s1, a lid -- a platform the whole width
    // of the map at 500 over the ground at 1000, as t62_s5's at about 3000:
    // only Ctrl + Down goes below it.
    MapMaker lid(80, 40);
    lid.flat(0, 0, 2559, 1000, 2);
    lid.flat(1, 0, 2559, 500, 1);
    // The final check of 2026.11.12: t90_s1, the lid with a ledge at 900
    // (x 1100..1300) over the ground -- a drop, then a jump up; t89_s1, the
    // lid over a second platform at 750 the whole width -- two drops.
    MapMaker lid_ledge(80, 40);
    lid_ledge.flat(0, 0, 2559, 1000, 2);
    lid_ledge.flat(1, 0, 2559, 500, 1);
    lid_ledge.flat(1, 1100, 1300, 900, 1);
    MapMaker lid_two(80, 40);
    lid_two.flat(0, 0, 2559, 1000, 2);
    lid_two.flat(1, 0, 2559, 500, 1);
    lid_two.flat(1, 0, 2559, 750, 1);
    // 2026.11.12c: t88_s1, t49_s5's step made up -- a ledge of solid ground at
    // 936 to x 1299, the ground at 1000 from 1300, a platform at 584 over
    // 1200..1500; t87_s1, t23_s14's bottom made up -- the bottom platform 1811
    // at x 1757 rising 0.25 px a column to the left (x 1500..1950), platforms
    // at 1468, 1425, 1393 and 1263, the ground 1864 under it all; its gate at
    // the area's home.
    MapMaker step(80, 40);
    step.flat(0, 0, 1299, 936, 2);
    step.flat(0, 1300, 2559, 1000, 2);
    step.flat(0, 1200, 1500, 584, 1);
    MapMaker stack(100, 70);
    for (int x = 1500; x <= 1950; ++x) stack.column(0, x, 1811 - (1757 - x) / 4, 1);
    stack.flat(0, 1604, 1797, 1468, 1);
    stack.flat(0, 1650, 1900, 1425, 1);
    stack.flat(0, 826, 1900, 1393, 1);
    stack.flat(0, 1500, 1900, 1263, 1);
    stack.flat(1, 0, 3199, 1864, 2);
    PrjMaker stack_gate;
    stack_gate.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
    stack_gate.place(3900000, 1593, 1262);
    // The review of 2026.11.12c: t86_s1, t23_s14's platforms by x 1700 made up
    // -- 1390 (x 1500..1900), the small 1425 (x 1728..1763) and 1465 (x
    // 1600..1800) -- and apart a ledge at 1250 (x 2000..2500) over a small
    // 1425 (x 2150..2185) and 1465 (x 2000..2300); the ground 1864 under all,
    // its gate at the ledge's right end.
    MapMaker few(100, 70);
    few.flat(0, 1500, 1900, 1390, 1);
    few.flat(0, 1728, 1763, 1425, 1);
    few.flat(0, 1600, 1800, 1465, 1);
    few.flat(0, 2000, 2500, 1250, 1);
    few.flat(0, 2150, 2185, 1425, 1);
    few.flat(0, 2000, 2300, 1465, 1);
    few.flat(1, 0, 3199, 1864, 2);
    PrjMaker few_gate;
    few_gate.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
    few_gate.place(3900000, 2480, 1249);
    // And t85_s1: solid ground at 1000, a platform at 700 (x 600..1400) and a
    // higher one at 500 apart from it (x 1450..1600) -- Ctrl+Up onto 700, and
    // a landing on 500 above it, within Ctrl+Up's reach: a move.
    MapMaker apart(80, 40);
    apart.flat(0, 0, 2559, 1000, 2);
    apart.flat(0, 600, 1400, 700, 1);
    apart.flat(0, 1450, 1600, 500, 1);
    bool ok = write_bytes(dir + "\\Map\\t86_s1.map", few.b) && write_bytes(dir + "\\Project\\t86_s1.prj", few_gate.b) &&
              write_bytes(dir + "\\Map\\t85_s1.map", apart.b) && write_bytes(dir + "\\Project\\t85_s1.prj", gate.b) &&
              write_bytes(dir + "\\Map\\t88_s1.map", step.b) && write_bytes(dir + "\\Project\\t88_s1.prj", gate.b) &&
              write_bytes(dir + "\\Map\\t87_s1.map", stack.b) &&
              write_bytes(dir + "\\Project\\t87_s1.prj", stack_gate.b) &&
              write_bytes(dir + "\\Map\\t92_s1.map", lid.b) && write_bytes(dir + "\\Project\\t92_s1.prj", gate.b) &&
              write_bytes(dir + "\\Map\\t90_s1.map", lid_ledge.b) &&
              write_bytes(dir + "\\Project\\t90_s1.prj", gate.b) && write_bytes(dir + "\\Map\\t89_s1.map", lid_two.b) &&
              write_bytes(dir + "\\Project\\t89_s1.prj", gate.b) &&
              write_bytes(dir + "\\Map\\t93_s1.map", low.b) && write_bytes(dir + "\\Project\\t93_s1.prj", gate.b) &&
              write_bytes(dir + "\\Map\\t96_s1.map", two.b) && write_bytes(dir + "\\Project\\t96_s1.prj", gate.b) &&
              write_bytes(dir + "\\Map\\t95_s1.map", ramp.b) &&
              write_bytes(dir + "\\Project\\t95_s1.prj", ramp_gate.b) && write_bytes(dir + "\\Map\\t94_s1.map", pit.b) &&
              write_bytes(dir + "\\Project\\t94_s1.prj", pit_gate.b);
    // t62_s5's own floors, its gates as the game's .prj places them.
    const std::string real = game_data().empty() ? std::string() : game_data() + "\\Map\\t62_s5.map";
    std::ifstream in(real, std::ios::binary);
    if (!real.empty() && in) {
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Bytes b(text.size());
        if (!text.empty()) std::memcpy(b.data(), text.data(), text.size());
        PrjMaker gates;
        gates.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
        gates.place(3900000, 265, 5125);
        gates.place(3900000, 3050, 710);
        ok = ok && write_bytes(dir + "\\Map\\t62_s5.map", b) && write_bytes(dir + "\\Project\\t62_s5.prj", gates.b);
    }
    if (!ok) dir.clear();
    return dir;
}

// ------------------------------------------------------------- the rig

/// What the pretend game makes of the next jump's key.
enum class Answer : std::uint8_t {
    Physics,    ///< up at the key's speed, down onto the highest floor its peak passed
    FallBack,   ///< up and bumped back onto where it stood, 0.5 s on
    Plain,      ///< a plain jump (state 1) whatever the key -- back where it stood
    Nothing,    ///< no take-off at all
    Outside,    ///< comes down at land_x, land_y wherever that is
    MapChange,  ///< a map entry 0.9 s after the key
    /// 2026.11.12c: as Physics, but every 0x002A in the air of state 7 -- the
    /// client of 2026-11-12 sent no 1, 2 or 6 -- at the key's own speed.
    OddState,
    /// Ctrl + ↑ off at a plain jump's -20 (state 1), down at force_x, force_y.
    PlainOnPlanned,
    /// Up, and down at force_x, force_y -- the step's top, for a wall-foot jump.
    Ledge,
    /// The 2026.11.12c review: no take-off reported at all, 0.2 s on a report
    /// standing at force_x, force_y -- a climb the packets said nothing of.
    Climb,
    /// 2026.11.15 (the owner's log of 00:27:54): as Physics, but a 0x002A of
    /// state 0 -- a "landing" -- at the top of the arc, in mid-air, a hit's;
    /// the real landing comes at its own time.
    Apex,
    /// 2026.11.15 (the first live run, 10:15:51): as Physics, but a 0x0027 --
    /// neither a cast's nor a 0x002A, so flight_of takes it for the landing --
    /// 150 ms into the flight, in mid-air; the real landing comes at its own
    /// time (move_at).
    MoveEarly,
    /// As MoveEarly, and a cast's report in mid-air 700 ms into the flight
    /// (cast_at): the character is still moving, 1 s before it lands.
    MoveRenewed,
};

/// The pretend game over a map's own floors, in the packets' world pixels:
/// the plates laid one to one over them.
struct JumpWorld {
    std::uint64_t t = 5'000'000;
    bool          focused = true;
    const GameMapBook::Page* page = nullptr;
    int           me_x = 880, me_y = 273;
    std::vector<Nameplate> mobs;
    int           step_px = 20;
    int           max_x = 0, min_x = INT_MAX;
    std::uint16_t map = 52, area = 2;
    float         entry_x = 880.0f, entry_y = 274.0f;

    std::set<std::uint32_t> held;
    struct Key {
        std::uint64_t at;
        std::uint32_t vk;
        int           hold;     ///< -1: put down and left down
    };
    std::vector<Key> keys;

    float         report_x = 0.0f, report_y = 0.0f;
    std::uint64_t report_ms = 0;
    /// The opcode of the last report (WorldFix::report_op): 0 unless a test
    /// sets it -- 0x0031 is a cast's (2026.11.15, blind_landing).
    std::uint16_t report_op = 0;
    /// The fix has the character where the last report put it, whatever it
    /// did since: a landing that sends no report of itself. Any report of the
    /// hunter's own walk ends it.
    bool          blind = false;
    std::uint32_t epoch = 7;

    // what the packets count and keep of your reports
    PacketWorld::AirCounts air;
    std::vector<PacketWorld::OwnTrace> trace;

    // the jumps
    struct Jump {
        std::uint64_t at;
        JumpKind      kind;
        int           x, y;
    };
    std::vector<Jump> jumps;
    Answer        answer = Answer::Physics;
    std::vector<Answer> answers;    ///< taken one a jump, first, before `answer`
    double        gravity = 2400.0;
    bool          second_cut = false;   ///< ↑↑'s second ↑ not sent
    int           force_x = 0, force_y = 0;
    std::uint64_t climb_at = 0;         ///< Answer::Climb's report, when it comes
    std::uint64_t apex_at = 0;          ///< Answer::Apex's mid-air report, when it comes
    std::uint64_t move_at = 0;          ///< Answer::MoveEarly's mid-air 0x0027, when it comes
    std::uint64_t cast_at = 0;          ///< Answer::MoveRenewed's mid-air cast, when it comes
    bool          land_as_move = false; ///< the landing is reported as a 0x0027, not a 0x002A of state 0
    std::uint64_t slow_ms = 0;          ///< the landing comes this much later than the arc says
    bool          flying = false;
    std::uint64_t fly_from = 0, land_at = 0, entry_at = 0;
    int           fly_x = 0, fly_y0 = 0, land_y = 0;
    double        fly_v = 0.0;
    bool          cast_in_air = false;  ///< a cast's report mid-flight: settled, at the height it is
    std::vector<std::pair<std::uint64_t, std::uint64_t>> flights;
    /// Ctrl + ↓ falls through the platform underfoot onto the first floor
    /// below (2026.11.12 review: the way back's drops); without, it does
    /// nothing, as every test before it had. Each chord's ↓, when it went.
    bool          drops_fall = false;
    std::vector<std::uint64_t> chords;
    /// Which way a 0x0027 of yours last said the character faces, and when
    /// (WorldFix::facing): 0, as every test before it, none -- a turn is
    /// taken as made at its tap.
    int           facing = 0;
    std::uint64_t facing_ms = 0;

    void report() {
        report_x = static_cast<float>(me_x);
        report_y = static_cast<float>(me_y);
        report_ms = t;
        report_op = 0;
        blind = false;
    }
    void place(int x, int y) {
        me_x = x;
        me_y = y;
        report();
        note();
    }
    void note() {
        max_x = std::max(max_x, me_x);
        min_x = std::min(min_x, me_x);
    }
    void own(OwnReport kind, std::uint32_t state, int x, int y, float vy, std::uint64_t at) {
        PacketWorld::OwnTrace r;
        r.ms = at;
        r.kind = kind;
        r.state = state;
        r.x = static_cast<float>(x);
        r.y = static_cast<float>(y);
        r.vy = vy;
        trace.push_back(r);
    }
    bool walking() const { return held.count(VK_LEFT) != 0 || held.count(VK_RIGHT) != 0; }
    int downs_of(std::uint32_t vk, std::uint64_t from = 0, std::uint64_t to = UINT64_MAX) const {
        int n = 0;
        for (const Key& k : keys)
            if (k.vk == vk && k.at >= from && k.at <= to) ++n;
        return n;
    }
    /// Arrows pressed while a jump was in the air.
    int arrows_in_air() const {
        int n = 0;
        for (const Key& k : keys)
            for (const auto& f : flights)
                if ((k.vk == VK_LEFT || k.vk == VK_RIGHT) && k.at > f.first && k.at < f.second) ++n;
        return n;
    }
    /// Along the floor it stands on -- its slope, and across a hole -- or
    /// off the floor's end onto whatever is under it.
    void walk_on() {
        if (!walking() || flying || page == nullptr) return;
        move(held.count(VK_LEFT) != 0 ? -1 : 1, step_px);
    }
    void move(int dir, int px) {
        const int to = me_x + dir * px;
        if (const std::optional<int> top = page->follow_to(me_x, me_y, to)) {
            me_x = to;
            me_y = *top - 1;
        } else {
            const MapTrail tr = page->follow(me_x, me_y, dir, px);
            if (tr.pts.empty()) return;
            const MapPoint end = tr.pts.back();
            if (tr.stop) {
                me_x = end.x;
                me_y = end.y - 1;
            } else if (const std::optional<MapPoint> land = walk_off_landing(*page, end, dir, kZoneWalkPxPerS)) {
                me_x = land->x;
                me_y = land->y;
                report();
            }
        }
        note();
    }
    /// The highest floor top in column x above the feet at y no higher than
    /// `peak` over them: where a jump straight up comes down.
    int landing(int x, int y, double peak) const {
        int best = y;
        for (const MapFloor& f : page->floors_at(x)) {
            const int rise = y + 1 - f.y;
            if (rise <= 3 || rise > peak) continue;
            if (f.y - 1 < best) best = f.y - 1;
        }
        return best;
    }
    /// The key: the take-off, by kind, reported as the client reports it.
    Hunter::JumpKeys jump(JumpKind kind, const std::function<bool()>& go_on) {
        Hunter::JumpKeys k;
        if (!go_on()) return k;
        Answer a = answer;
        if (!answers.empty()) {
            a = answers.front();
            answers.erase(answers.begin());
        }
        if (kind == JumpKind::CtrlUp) t += 50;     // Ctrl alone
        k.went = true;
        k.whole = !(kind == JumpKind::UpUp && second_cut);
        k.down_ms = k.last_ms = t;
        k.held_ms = 94;
        k.lead_ms = kind == JumpKind::CtrlUp ? 50 : 0;
        k.tail_ms = kind == JumpKind::CtrlUp ? 47 : 0;
        jumps.push_back({ t, kind, me_x, me_y });
        const std::uint64_t key = t;
        t += 94;
        if (kind == JumpKind::UpUp && k.whole) {
            k.last_ms = key + 340;
            t = key + 340 + 94;
        }
        if (kind == JumpKind::CtrlUp) t += 47;
        if (a == Answer::Nothing) return k;
        if (a == Answer::Climb) {
            climb_at = key + 200;
            return k;
        }
        // Up and away, 30 ms after the key.
        const bool high = kind == JumpKind::CtrlUp && a != Answer::Plain && a != Answer::PlainOnPlanned;
        const bool odd = a == Answer::OddState;
        const bool physics = a == Answer::Physics || a == Answer::Apex || a == Answer::MoveEarly ||
                             a == Answer::MoveRenewed || odd;
        const double v = high ? kJumpUpVHigh : kJumpUpV;
        const std::uint64_t off = key + 30;
        own(OwnReport::Air, odd ? 7u : high ? 2u : 1u, me_x, me_y, high ? -36.0f : -20.0f, off);
        // As PacketWorld counts them (2026.11.12c): an odd state by its speed.
        if (odd) ++air.other;
        if (high) ++air.high;
        else      ++air.jumps;
        flying = true;
        fly_from = off;
        fly_x = me_x;
        fly_y0 = me_y;
        fly_v = v;
        report_ms = off;
        double up_s = v / gravity, peak = v * v / (2.0 * gravity);
        if (kind == JumpKind::UpUp && k.whole && physics) {
            // The second push 340 ms after the first key: -800 again.
            const double t2 = 0.31;
            const double h2 = kJumpUpV * t2 - gravity * t2 * t2 / 2.0;
            own(OwnReport::Air, odd ? 7u : 6u, me_x, me_y - static_cast<int>(h2), -20.0f, off + 310);
            if (odd) ++air.other;
            ++air.second;
            peak = h2 + kJumpUpV * kJumpUpV / (2.0 * gravity);
            up_s = t2 + kJumpUpV / gravity;
        }
        int land = me_y;
        double flight = 0.0;
        if (physics) {
            land = landing(me_x, me_y, peak);
            flight = up_s + std::sqrt(2.0 * (peak - (me_y - land)) / gravity);
            if (a == Answer::Apex) apex_at = off + static_cast<std::uint64_t>(up_s * 1000.0);
            if (a == Answer::MoveEarly || a == Answer::MoveRenewed) move_at = off + 150;
            if (a == Answer::MoveRenewed) cast_at = off + 700;
        } else if (a == Answer::FallBack) {
            flight = 0.5;
        } else if (a == Answer::Plain) {
            flight = 2.0 * kJumpUpV / gravity;
        } else if (a == Answer::Outside || a == Answer::PlainOnPlanned || a == Answer::Ledge) {
            flight = 0.9;
        } else if (a == Answer::MapChange) {
            flight = 2.0 * up_s;
            entry_at = key + 900;
        }
        land_y = land;
        const bool forced = a == Answer::Outside || a == Answer::PlainOnPlanned || a == Answer::Ledge;
        if (forced) land_y = force_y;
        land_at = off + static_cast<std::uint64_t>(flight * 1000.0) + slow_ms;
        flights.push_back({ off, land_at });
        if (forced) force_x = force_x != 0 ? force_x : me_x;
        else                      force_x = 0;
        return k;
    }
    /// Ctrl + ↓ as the client sends it: Ctrl alone, ↓, both up; through the
    /// platform underfoot, a fall straight down onto the floor under it.
    InputManager::DropPress drop(const std::function<bool()>& go_on) {
        InputManager::DropPress p;
        if (!drops_fall || !go_on()) return p;
        keys.push_back({ t, VK_CONTROL, 50 });
        t += 50;
        p.down_ms = t;
        keys.push_back({ t, VK_DOWN, 80 });
        chords.push_back(t);
        t += 80;
        p.held_ms = 80;
        t += 50;
        p.sent = InputManager::DropSent::Sent;
        const std::optional<MapFloor> below = page != nullptr ? page->floor_below(me_x, me_y, 8) : std::nullopt;
        if (!below) return p;
        const std::uint64_t off = p.down_ms + 30;
        own(OwnReport::Air, 1u, me_x, me_y, 1.0f, off);
        ++air.jumps;
        flying = true;
        fly_from = off;
        fly_x = me_x;
        fly_y0 = me_y;
        fly_v = 0.0;
        land_y = below->y - 1;
        force_x = 0;
        report_ms = off;
        land_at = off + static_cast<std::uint64_t>(std::sqrt(2.0 * (land_y - me_y) / gravity) * 1000.0);
        flights.push_back({ off, land_at });
        return p;
    }
    /// One pass of the game: the flight's height, its landing, a map entry.
    void tick() {
        if (entry_at != 0 && t >= entry_at) {
            entry_at = 0;
            ++epoch;
            map = 53;
            area = 1;
            entry_x = 400.0f;
            entry_y = 500.0f;
            flying = false;
            place(400, 500);
        }
        if (climb_at != 0 && t >= climb_at) {
            climb_at = 0;
            place(force_x, force_y);
        }
        if (!flying) return;
        const double s = static_cast<double>(t - fly_from) / 1000.0;
        const int y = fly_y0 - static_cast<int>(std::lround(fly_v * s - gravity * s * s / 2.0));
        me_y = std::min(y, std::max(fly_y0, land_y));
        if (cast_in_air) {
            own(OwnReport::Cast, 0u, me_x, me_y, 0.0f, t);
            report();
        }
        if (apex_at != 0 && t >= apex_at) {
            // The hit's report at the top of the arc: state 0, in mid-air.
            apex_at = 0;
            own(OwnReport::Air, 0u, me_x, me_y, 0.0f, t);
            report();
        }
        if (move_at != 0 && t >= move_at) {
            // A 0x0027 in mid-air (the first live run, 10:15:51): flight_of
            // takes the first report that is neither a cast's nor a 0x002A
            // for the landing while no 0x002A of state 0 has come.
            move_at = 0;
            own(OwnReport::Move, 0u, me_x, me_y, 0.0f, t);
            report();
        }
        if (cast_at != 0 && t >= cast_at) {
            // A cast in mid-air: the character is still in flight.
            cast_at = 0;
            own(OwnReport::Cast, 0u, me_x, me_y, 0.0f, t);
        }
        if (t >= land_at) {
            flying = false;
            if (force_x != 0) me_x = force_x;
            me_y = land_y;
            own(land_as_move ? OwnReport::Move : OwnReport::Air, 0u, me_x, me_y, 0.0f, t);
            if (!land_as_move) ++air.landed;
            report();
            note();
        }
    }
};

/// A hunter on the pretend game, over a map's files, started without a
/// thread: every seam its own. `wired`: the air counts and your reports
/// handed in -- without them, the command line's hunter: no jump up ever.
struct JumpRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MobDetector          detector;
    Hunter               hunter{ input, coord, detector };
    GameMapBook          book;
    JumpWorld            w;
    const char*          veto = nullptr;

    JumpRig(const HunterConfig& cfg, int x, int y, bool wired = true, const std::string& data = game_data(),
            const char* stem = "t52_s2")
        : book(data) {
        const std::shared_ptr<const GameMapBook::Page> page = book.get(stem);
        w.page = page.get();
        w.place(x, y);
        Hunter::Eyes eyes;
        eyes.view = [] { return 1u; };
        eyes.where = [this](WorldFix& f) {
            f = WorldFix{};
            f.state = WorldFix::State::Known;
            f.x = static_cast<float>(w.me_x);
            f.y = w.walking() ? w.report_y : static_cast<float>(w.me_y);
            f.rx = w.report_x;
            f.ry = w.report_y;
            f.report_ms = w.report_ms;
            f.report_op = w.report_op;
            if (w.blind && !w.walking()) {
                f.x = w.report_x;
                f.y = w.report_y;
            }
            f.airborne = w.flying && !w.cast_in_air;
            f.moving = w.walking();
            f.map_known = true;
            f.map = w.map;
            f.area = w.area;
            f.epoch = w.epoch;
            f.entry_x = w.entry_x;
            f.entry_y = w.entry_y;
            f.facing = w.facing;
            f.facing_ms = w.facing_ms;
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
        eyes.takeoffs = [this]() -> std::optional<std::uint64_t> { return w.air.takeoffs(); };
        eyes.stands = [this] {
            std::vector<MapTrust::Mob> out;
            for (const Nameplate& n : w.mobs)
                if (n.slot >= 0)
                    out.push_back(MapTrust::Mob{ MapPoint{ n.x(), n.y() }, static_cast<std::uint32_t>(n.slot),
                                                 w.t - 400, n.gen });
            return out;
        };
        if (wired) {
            eyes.air = [this]() -> std::optional<PacketWorld::AirCounts> { return w.air; };
            eyes.own_since = [this](std::uint64_t ms) {
                std::vector<PacketWorld::OwnTrace> out;
                for (const PacketWorld::OwnTrace& r : w.trace)
                    if (r.ms >= ms) out.push_back(r);
                return out;
            };
        }
        hunter.set_eyes(eyes);

        Hunter::Hands hands;
        hands.focused = [this] { return w.focused; };
        hands.key = [this](std::uint32_t vk, int hold) {
            w.keys.push_back({ w.t, vk, hold });
            // An arrow tapped walks the character as far as it is held.
            if ((vk == VK_LEFT || vk == VK_RIGHT) && !w.flying && w.page != nullptr) {
                w.move(vk == VK_LEFT ? -1 : 1, static_cast<int>(std::lround(hold * kZoneWalkPxPerS / 1000.0)));
                w.report();
            }
            return true;
        };
        hands.key_down = [this](std::uint32_t vk) {
            w.keys.push_back({ w.t, vk, -1 });
            w.held.insert(vk);
            if (vk == VK_LEFT || vk == VK_RIGHT) w.report();
            return true;
        };
        hands.key_up = [this](std::uint32_t vk) {
            if (w.held.erase(vk) != 0 && (vk == VK_LEFT || vk == VK_RIGHT)) w.report();
        };
        hands.held = [this](std::uint32_t vk) { return w.held.count(vk) != 0; };
        hands.release_all = [this] {
            if (w.walking()) w.report();
            w.held.clear();
        };
        hands.settle_owed = [] { return true; };
        hands.drop = [this](const std::function<bool()>& go_on) { return w.drop(go_on); };
        hands.jump = [this](JumpKind kind, const std::function<bool()>& go_on) { return w.jump(kind, go_on); };
        hunter.set_hands(hands);

        Hunter::Clock clock;
        clock.now_ms = [this] { return w.t; };
        clock.sleep_ms = [this](std::uint32_t ms) { w.t += ms; };
        hunter.set_clock(clock);

        hunter.set_map_book(&book);
        hunter.set_drop_veto([this] { return veto; });
        hunter.set_config(cfg);
        hunter.start_stepped();
    }
    ~JumpRig() { hunter.stop(); }

    void pass() {
        hunter.step();
        w.walk_on();
        w.t += 60;
        w.tick();
    }
    void run(std::uint64_t ms, const std::function<void()>& each = nullptr) {
        for (const std::uint64_t until = w.t + ms; w.t < until;) {
            pass();
            if (each) each();
        }
    }
    bool until(const std::function<bool()>& done, std::uint64_t ms) {
        for (const std::uint64_t end = w.t + ms; w.t < end;) {
            pass();
            if (done()) return true;
        }
        return false;
    }
    Hunter::Status st() const { return hunter.status(); }
};

/// The owner's hunt.ini of 2026-11-04/05: ↑ the jump key, packets on, the
/// map tab's area, jump_up as it ships -- on.
HunterConfig owner_config() {
    HunterConfig c;
    c.reach = 450;
    c.height_tolerance = 60;
    c.jump_key = 38;
    c.jump_to_reach = true;
    c.packet_positions = PacketPositions::On;
    c.spot = true;
    c.spot_tolerance = 48;
    c.spot_return_delay_ms = 1500;
    c.drop_down = false;
    c.hold_walk = true;
    c.give_up_ms = 4000;
    return c;
}

Hunter::Zone zone_box(const char* stem, int home_x, int home_y, int left, int top, int right, int bottom) {
    Hunter::Zone z;
    z.stem = stem;
    z.epoch = 7;
    z.home_x = static_cast<float>(home_x);
    z.home_y = static_cast<float>(home_y);
    z.left = static_cast<float>(left);
    z.top = static_cast<float>(top);
    z.right = static_cast<float>(right);
    z.bottom = static_cast<float>(bottom);
    return z;
}
/// spots.ini's [t52_s2] as the owner keeps it: home 880,274 on the spawn ledge.
Hunter::Zone owner_zone() { return zone_box("t52_s2", 880, 274, 265, 174, 1399, 694); }

bool from_map(const JumpRig& r) { return r.hunter.spot().set && r.hunter.spot().from_map; }

/// The keys a rig pressed, as one text: what two hunters did, compared.
std::string key_log(const JumpWorld& w) {
    std::string s;
    for (const JumpWorld::Key& k : w.keys) s += fmt("%llu:%u:%d ", static_cast<unsigned long long>(k.at), k.vk, k.hold);
    return s;
}

// ------------------------------------------------------------- the rule

void test_jump_rule() {
    section("Hunter::jump_up_rule: each refusal in the plan's order, over t52_s2 -- the owner's box, the ground "
            "under the home ledge");
    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): the jump-up rule\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    check(page != nullptr, "t52_s2 read");
    if (!page) return;
    HuntZone box;
    box.home_x = 880;
    box.home_y = 274;
    box.left = 265;
    box.top = 174;
    box.right = 1399;
    box.bottom = 694;

    HunterConfig cfg = owner_config();
    // A monster on the ledge at 760,273, the character on the ground at 600,579.
    const auto facts = [&](bool press, int x) {
        Hunter::JumpUpFacts f;
        f.purpose = Hunter::JumpUpFor::Target;
        f.press = press;
        f.now = 100'000;
        f.area = true;
        f.trusted = true;
        f.wired = true;
        f.packet_target = true;
        f.slot = 11;
        f.gen = 1;
        f.dy = 273 - 579;
        f.target_wx = 760.0f;
        f.target_wy = 273.0f;
        f.fix.state = WorldFix::State::Known;
        f.fix.x = f.fix.rx = static_cast<float>(x);
        f.fix.y = f.fix.ry = static_cast<float>(stand_y(*page, x, 579));
        f.fix.report_ms = 99'960;
        f.entry_ms = 10'000;
        f.settle_from_ms = 99'940;
        f.page = page;
        f.box = box;
        f.stand = MapPoint{ x, stand_y(*page, x, 579) };
        f.rules.reach_px = 450;
        return f;
    };

    // It goes: Ctrl + ↑ from about 695 onto the ledge's floor, the monster's.
    const Hunter::JumpUpCall go = Hunter::jump_up_rule(cfg, facts(false, 600));
    check(go.ok() && go.plan && go.plan->kind == JumpKind::CtrlUp && std::abs(go.plan->x - 696) <= 4 &&
              std::abs(go.plan->land_top - 275) <= 2 && go.placed && go.target.y == stand_y(*page, 760, 273),
          fmt("from 600,579 to the monster at 760,273: Ctrl+Up at x %d onto floor %d, the monster's floor %d at x %d "
              "(%s)",
              go.plan ? go.plan->x : 0, go.plan ? go.plan->land_top : 0, go.placed ? go.target.y + 1 : -1, go.target.x,
              Hunter::jump_why_name(go.why)));
    // At the press, stood on the take-off: its column judged afresh.
    const Hunter::JumpUpCall at = Hunter::jump_up_rule(cfg, facts(true, 700));
    check(at.ok() && at.plan && at.plan->x == 700 && at.column.ok() && std::abs(at.column.land_top - 275) <= 2,
          fmt("settled at 700: the column passes, the jump goes from there (%s)", Hunter::jump_why_name(at.why)));
    // Stopped under ledge A (600): refused at the press -- the ceiling.
    const Hunter::JumpUpCall under = Hunter::jump_up_rule(cfg, facts(true, 600));
    check(under.why == Why::OffColumn && under.no == JumpNo::Ceiling,
          fmt("settled at 600, under ledge A: OffColumn (%s) -- never pressed there", jump_no_name(under.no)));

    struct Case {
        const char* what;
        std::function<void(HunterConfig&, Hunter::JumpUpFacts&)> set;
        Why why;
    };
    const Case cases[] = {
        { "jump_up off", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.jump_up = false; }, Why::Off },
        { "packets Watch", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.packet_positions = PacketPositions::Watch; },
          Why::NoPackets },
        { "stay_put", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.stay_put = true; }, Why::StayPut },
        { "no area", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.area = false; }, Why::NoArea },
        // No "latched" since 2026.11.24: nothing switches the jumps up off.
        { "keys owed", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.owed = true; }, Why::KeysOwed },
        { "jump key Space", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.jump_key = 32; }, Why::JumpKey },
        { "not wired", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.wired = false; }, Why::NotWired },
        { "a screen plate", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.packet_target = false; },
          Why::NotPacketTarget },
        { "a monster on the level", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.level_monster = true; },
          Why::LevelMonster },
        { "not above", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.dy = -40; }, Why::NotAbove },
        { "the monster outside", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.target_wx = 1500.0f; },
          Why::OutsideArea },
        { "not known", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.fix.state = WorldFix::State::Lost; },
          Why::NotKnown },
        { "after an entry", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.entry_ms = 98'000; }, Why::AfterEntry },
        { "in the air", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.in_air = true; }, Why::InAir },
        { "too soon", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.last_press_ms = 99'000; }, Why::TooSoon },
        { "after a drop", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.last_drop_worked_ms = 95'000; },
          Why::AfterDrop },
        { "a step back", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.step_judge = true; }, Why::StepJudge },
        { "the minute's", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.presses_minute = 4; }, Why::MinuteCap },
        { "the monster's", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.presses_target = 2; }, Why::TargetCap },
        { "its settles", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.settles_target = 2; }, Why::TargetCap },
        { "a hold", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.held = true; }, Why::Held },
        { "the game behind", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.focused = false; }, Why::Unfocused },
        { "pick-ups", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.loot_owed = true; }, Why::Loot },
        { "the veto (HP)", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.veto = "HP is low"; }, Why::Veto },
        { "the map not trusted", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.trusted = false; }, Why::MapUnknown },
        { "off every floor", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.stand.reset(); }, Why::OffFloor },
        // The one key ever left out since 2026.11.24 (no key off for the run):
        // Ctrl + ↑ with a walk or attack key on Ctrl -- and only it reaches
        // ledge A, so no plan.
        { "the attack key Ctrl", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.attack_key = 0x11; }, Why::Plan },
    };
    for (const Case& k : cases) {
        HunterConfig c = cfg;
        Hunter::JumpUpFacts f = facts(false, 600);
        k.set(c, f);
        const Hunter::JumpUpCall call = Hunter::jump_up_rule(c, f);
        check(call.why == k.why, fmt("%s: %s (got %s)", k.what, Hunter::jump_why_name(k.why),
                                     Hunter::jump_why_name(call.why)));
    }
    // At the press only: not settled, the stop not reported, an arrow down.
    {
        Hunter::JumpUpFacts f = facts(true, 700);
        f.fix.moving = true;
        Hunter::JumpUpFacts g = facts(true, 700);
        g.walk_held = true;
        g.fix.report_ms = 99'900;
        Hunter::JumpUpFacts h = facts(true, 700);
        h.left_held = true;
        Hunter::JumpUpFacts a = facts(false, 600);
        a.fix.moving = true;
        a.left_held = true;
        check(Hunter::jump_up_rule(cfg, f).why == Why::NotSettled &&
                  Hunter::jump_up_rule(cfg, g).why == Why::NotSettled &&
                  Hunter::jump_up_rule(cfg, h).why == Why::Walking && Hunter::jump_up_rule(cfg, a).ok(),
              "at the press: walking, or its stop not reported since the settle began, or an arrow down -- refused; "
              "walked to, none of them");
    }
    // On its own floor: walked to. A monster up the ground's slope from
    // 987,683 (700,579).
    {
        Hunter::JumpUpFacts f = facts(false, 987);
        f.stand = MapPoint{ 987, stand_y(*page, 987, 683) };
        f.fix.x = f.fix.rx = 987.0f;
        f.fix.y = f.fix.ry = static_cast<float>(f.stand->y);
        f.dy = 579 - f.stand->y;
        f.target_wx = 700.0f;
        f.target_wy = 579.0f;
        check(Hunter::jump_up_rule(cfg, f).why == Why::OnOwnFloor, "a monster up the character's own slope: walked to");
    }
    // The way back's leg: as given, the target's rules not asked; AfterDrop
    // not either. Seek: the monster's caps.
    {
        Hunter::JumpUpFacts f = facts(false, 880);
        f.purpose = Hunter::JumpUpFor::Home;
        f.packet_target = false;
        f.slot = -1;
        f.last_drop_worked_ms = 95'000;
        f.stand = MapPoint{ 880, stand_y(*page, 880, 634) };
        JumpRules up;
        up.reach_px = 450;
        f.plan = plan_jump_to(*page, box, *f.stand, MapPoint{ 880, 273 }, up);
        const Hunter::JumpUpCall home = Hunter::jump_up_rule(cfg, f);
        Hunter::JumpUpFacts s = f;
        s.purpose = Hunter::JumpUpFor::Seek;
        s.packet_target = true;
        s.slot = 4;
        s.target_wx = 880.0f;
        s.target_wy = 273.0f;
        check(home.ok() && home.plan && home.plan->kind == JumpKind::CtrlUp &&
                  Hunter::jump_up_rule(cfg, s).why == Why::AfterDrop,
              fmt("Home: the leg as given, Ctrl+Up at %d, a drop a moment ago not in its way; Seek: AfterDrop",
                  home.plan ? home.plan->x : 0));
        Hunter::JumpUpFacts none = f;
        none.plan.reset();
        check(Hunter::jump_up_rule(cfg, none).why == Why::Plan, "Home with no leg: Plan");
    }

    // The drop's side: no drop for kJumpUpStayMs after a jump up toward a
    // target that worked (DropWhy::AfterJump, appended).
    {
        Hunter::DropFacts d;
        d.now = 100'000;
        d.wired = true;
        d.packet_target = true;
        d.dy = 300;
        d.fix.state = WorldFix::State::Known;
        d.entry_ms = 10'000;
        d.last_jump_worked_ms = 95'000;
        HunterConfig dc = cfg;
        dc.drop_down = true;
        dc.spot = false;
        const Hunter::DropCall call = Hunter::drop_rule(dc, d);
        check(call.why == Hunter::DropWhy::AfterJump &&
                  static_cast<int>(Hunter::DropWhy::AfterJump) == static_cast<int>(Hunter::DropWhy::NoDropHere) + 1 &&
                  std::string(Hunter::drop_why_name(Hunter::DropWhy::AfterJump)).find("jump up") != std::string::npos,
              fmt("a drop 5 s after a jump up that worked: AfterJump, appended after NoDropHere (%s)",
                  Hunter::drop_why_name(call.why)));
    }
}

// ------------------------------------------------------------- the way back

void test_jump_way_back() {
    section("Hunter 05b: the way back by Ctrl + Up -- 04d's walk back from 1342,772 replayed: along the ground, "
            "settled near 810, one high jump, onto the ledge, home");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the way back by a jump up\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");

    JumpRig r(owner_config(), 1342, stand_y(p, 1342, 772));
    r.hunter.set_zone(owner_zone());
    bool used = false;
    std::uint64_t first_key = 0;
    // What the card says of the way back on its way there: a jump up, the
    // one it starts with Ctrl + Up near 810 (2026.11.05b's GUI).
    bool said_jump = false;
    // The first look too: the walk back first goes at once (2026.11.10).
    std::vector<std::string> log = info_lines([&] {
        r.pass();
        used = from_map(r);
        r.until([&] {
            const Hunter::Status s = r.st();
            said_jump = said_jump || (s.spot_way_by_jumps && s.spot_way_jumps == 1 && s.spot_way_jump_first &&
                                      s.spot_way_jump_kind == JumpKind::CtrlUp && s.spot_way_jump_x >= 695 &&
                                      s.spot_way_jump_x <= 1002);
            return !r.w.jumps.empty();
        }, 12'000);
        if (!r.w.jumps.empty()) first_key = r.w.jumps.front().at;
        r.until([&] { return r.st().spot_home && !r.w.walking(); }, 8'000);
        r.run(1500);
    });
    const Hunter::Status st = r.st();
    const bool one = r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::CtrlUp;
    const int jx = one ? r.w.jumps[0].x : 0;
    // 2026.11.12c: the run under the ledge is 695..1002 (Ctrl+Up's lowest peak
    // 432, headroom enough all along): the take-off nearest the walk back.
    check(used && one && jx >= 695 && jx <= 1002 && r.w.jumps[0].y == stand_y(p, jx, 604),
          fmt("one Ctrl+Up, from %d,%d on the ground (the run's take-offs 695..1002)", jx, one ? r.w.jumps[0].y : 0));
    check(st.spot_home && std::abs(r.w.me_x - 880) <= 48 && r.w.me_y == stand_y(p, r.w.me_x, 273) && st.jumps == 1 &&
              st.jumps_worked == 1,
          fmt("landed on the ledge and walked home: %d,%d, home %d (%llu worked)", r.w.me_x, r.w.me_y,
              st.spot_home ? 1 : 0, static_cast<unsigned long long>(st.jumps_worked)));
    check(r.w.arrows_in_air() == 0 && r.w.downs_of(38) == 0 && r.w.downs_of(VK_DOWN) == 0 &&
              r.w.downs_of(VK_LEFT, 0, first_key) >= 1 && r.w.downs_of(VK_RIGHT, 0, first_key) == 0,
          fmt("walked left to it, no hop (%d), no Down, no arrow in the air (%d)", r.w.downs_of(38),
              r.w.arrows_in_air()));
    check(st.return_give_ups == 0 && lines_with(log, "got nowhere") == 0,
          "the give-up clock stood while it waited for the map entry's 5 s, settled and flew: no walk back given up");
    check(said_jump, "the status on the way: the way back worked out with jumps, one, starting with Ctrl+Up at 695..1002 "
                     "-- the card's \"Về: nhảy Ctrl+↑ ở x ...\"");
    // The walk back first goes at the first look (2026.11.10), before the
    // map is trusted for jumps: it starts along the ground, and the way is
    // asked again with the jump once it is -- one walk back, one start line.
    check(lines_with(log, "Hunter: walking back into the hunting area from 1342,") == 1 &&
              lines_with(log, "Hunter: jump up (Ctrl+Up) on the way home: from ", "onto floor 27") == 1 &&
              lines_with(log, "Hunter: jump up worked: landed at ", "on floor 27") == 1 &&
              lines_with(log, "came up short") == 0 && lines_with(log, "bumped from below") == 0,
          "said: \"" + line_with(log, "Hunter: jump up (Ctrl+Up)") + "\" -- \"" + line_with(log, "jump up worked") +
              "\"");

    // From 880,634 under the home: the same jump, 70-odd px left.
    JumpRig u(owner_config(), 880, stand_y(p, 880, 634));
    u.hunter.set_zone(owner_zone());
    u.until([&] { return u.st().spot_home && !u.w.walking() && u.w.me_y < 300; }, 12'000);
    check(u.w.jumps.size() == 1 && u.w.jumps[0].x >= 856 && u.w.jumps[0].x <= 904 && u.st().spot_home,
          fmt("from 880,634: one Ctrl+Up at %d (where it stands since 2026.11.12c, within %d of it), home",
              u.w.jumps.empty() ? 0 : u.w.jumps[0].x, Hunter::kJumpUpLeadPx));
}

// ------------------------------------------------------------- a target above

void test_jump_target() {
    section("Hunter 05b: a monster on the ledge seen from 600,579 -- walked right to about 695, never pressed from "
            "505..638, no swing in the air at it behind and no arrow, then turned and fought on the ledge");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: a monster above\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");

    HunterConfig c = owner_config();
    c.spot_return_delay_ms = 60'000;
    c.keep_distance = true;
    JumpRig r(c, 600, stand_y(p, 600, 579));
    r.hunter.set_zone(owner_zone());
    r.run(Hunter::kDropAfterEntryMs + 300);
    const bool used = from_map(r) && r.w.jumps.empty();
    r.w.mobs = { mob_at(560, 273, 11) };
    std::uint64_t key = 0, land = 0;
    int air_attacks = 0;
    std::vector<std::string> log = info_lines([&] {
        r.until([&] { return !r.w.jumps.empty(); }, 6000);
        if (r.w.jumps.empty()) return;
        key = r.w.jumps.front().at;
        r.until([&] { return !r.w.flying; }, 3000);
        land = r.w.t;
        air_attacks = r.w.downs_of('Z', key, land);
        r.run(3000);
    });
    const bool one = r.w.jumps.size() == 1;
    const int jx = one ? r.w.jumps[0].x : 0;
    check(used && one && r.w.jumps[0].kind == JumpKind::CtrlUp && jx >= 663 && jx <= 740 && (jx < 505 || jx > 638),
          fmt("one Ctrl+Up from x %d (about 695; never 505..638)", jx));
    // In the air the character faces the way it walked, right, and the
    // monster is behind it: until 2026.11.12 it swung all the same, at
    // nothing; now the swing waits for the turn, which waits for the landing
    // (F1: never a swing facing away).
    check(air_attacks == 0 && r.w.arrows_in_air() == 0,
          fmt("no swing in the air at the monster behind (%d) and no arrow -- no turn's tap, no step back",
              air_attacks));
    check(r.w.me_y == stand_y(p, r.w.me_x, 273) && r.st().jumps_worked == 1 && r.w.downs_of('Z', land) >= 2 &&
              r.w.downs_of(VK_LEFT, land) >= 1,
          fmt("landed on the ledge at %d,%d and fought there: turned left after the landing, %d attacks", r.w.me_x,
              r.w.me_y, r.w.downs_of('Z', land)));
    check(r.w.downs_of(38) == 0 && r.w.downs_of(VK_DOWN) == 0, "no hop, no Down");
    check(lines_with(log, "Hunter: jump up (Ctrl+Up) toward the monster: from ") == 1,
          "said: \"" + line_with(log, "Hunter: jump up (Ctrl+Up)") + "\"");
    // The minute line: the looks, the press by key, what came of it, g≈.
    const std::vector<std::string> minute = info_lines([&] { r.run(60'000); });
    const std::string m = line_with(minute, " fights, ");
    const std::size_t part = m.find("; jump-ups");
    check(part != std::string::npos &&
              m.find("pressed 1 (Up 0, Up,Up 0, Ctrl+Up 1), worked 1, fell back 0") != std::string::npos &&
              m.find("; g≈") != std::string::npos,
          "the minute line: \"" + m.substr(part == std::string::npos ? 0 : part) + "\"");
}

// ------------------------------------------------------------- what goes wrong

void test_jump_fails() {
    section("Hunter 05b, 2026.11.24: falls back and what they teach (for kLearnedForgetMs), a key not taken as its "
            "jump, Nothing again and again, outside, g out of range, a map change after the key -- never a pause, an "
            "\"off\" or a stop");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the jumps that go wrong\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    const int ground = stand_y(p, 880, 634);

    // Fall back twice from under the home: the point learned and the next
    // press 48 px away at least, the gap kept; the second onto the same floor
    // makes it solid -- no third press while it is (2026.11.24: kLearnedForgetMs,
    // the session until then). Then it is forgotten, and tried again.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::FallBack;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(25'000); });
        const auto& j = r.w.jumps;
        check(j.size() == 2 && std::abs(j[1].x - j[0].x) >= GameMapBook::kNoJumpXPx &&
                  j[1].at >= j[0].at + Hunter::kDropGapMs,
              fmt("two presses: at %d, then %d -- %d px apart, %llu ms apart", j.empty() ? 0 : j[0].x,
                  j.size() > 1 ? j[1].x : 0, j.size() > 1 ? std::abs(j[1].x - j[0].x) : 0,
                  j.size() > 1 ? static_cast<unsigned long long>(j[1].at - j[0].at) : 0ull));
        check(r.book.no_jumps("t52_s2").size() == 2 && r.book.solid_floors("t52_s2").size() == 1 &&
                  r.st().jumps_fell_back == 2,
              fmt("learned: %zu no-jump points, %zu floor solid from below", r.book.no_jumps("t52_s2").size(),
                  r.book.solid_floors("t52_s2").size()));
        check(lines_with(log, "Hunter: jump up FELL BACK to ", "bumped from below) -- no Ctrl+Up to floor 27") == 2 &&
                  lines_with(log, "for 30 s, another column first") == 2 &&
                  lines_with(log, "Hunter: floor 27", "taken as solid from below for 30 s") == 1 &&
                  lines_with(log, "this session") == 0 && lines_with(log, "OFF until") == 0,
              "said: \"" + line_with(log, "FELL BACK") + "\" -- \"" + line_with(log, "solid from below") +
                  "\"; a bump's g not counted against the model");
        check(r.w.downs_of(VK_DOWN) == 0 && r.w.arrows_in_air() == 0, "no Down, no arrow in the air");
        // kLearnedForgetMs after it was learned the floor is forgotten -- the
        // take-offs with it -- and the way back's Ctrl+Up goes again.
        const std::uint64_t second = j.size() > 1 ? j[1].at : 0;
        std::vector<std::string> later = info_lines([&] {
            r.until([&] { return r.w.jumps.size() >= 3; }, Hunter::kLearnedForgetMs + 15'000);
        });
        check(r.w.jumps.size() >= 3 && r.w.jumps[2].at >= second + Hunter::kLearnedForgetMs &&
                  lines_with(later, "where the jump did not work is tried again (30 s on)") >= 1,
              fmt("the floor and its take-offs forgotten %llu s on: a third press %llu ms after the second -- \"%s\"",
                  static_cast<unsigned long long>(Hunter::kLearnedForgetMs / 1000),
                  r.w.jumps.size() >= 3 ? static_cast<unsigned long long>(r.w.jumps[2].at - second) : 0ull,
                  line_with(later, "is tried again").c_str()));
    }
    // Nothing again and again: a fail each, not learned -- and never a pause
    // (2026.11.24, the owner: "khi hỏng thì thử lại luôn"; 30 / 60 / 120 s
    // from 2026.11.15, off until the next start before). The way back's
    // Ctrl+Up goes again at the next look, kDropGapMs or more after the last;
    // a press that did nothing is no press of the minute's, so the minute's
    // 4 never holds the next back.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::Nothing;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(25'000); });
        bool spaced = r.w.jumps.size() >= 2;
        std::uint64_t widest = 0;
        for (std::size_t i = 1; i < r.w.jumps.size(); ++i) {
            const std::uint64_t gap = r.w.jumps[i].at - r.w.jumps[i - 1].at;
            spaced = spaced && gap >= Hunter::kDropGapMs;
            widest = std::max(widest, gap);
        }
        check(r.w.jumps.size() > static_cast<std::size_t>(Hunter::kJumpUpsPerMinute) + 1 && spaced && widest < 6000 &&
                  r.book.no_jumps("t52_s2").empty(),
              fmt("%zu presses that did nothing in 25 s, each %llu ms or more after the last, the widest gap %llu ms -- "
                  "more than the minute's %d, nothing learned",
                  r.w.jumps.size(), static_cast<unsigned long long>(Hunter::kDropGapMs),
                  static_cast<unsigned long long>(widest), Hunter::kJumpUpsPerMinute));
        // Each judged and said -- the last one perhaps not yet, 0.9 s after its
        // key, when the 25 s ran out.
        const int said = lines_with(log, "did nothing in 900 ms");
        check(said + 1 >= static_cast<int>(r.w.jumps.size()) && said <= static_cast<int>(r.w.jumps.size()) &&
                  lines_with(log, "jumps up paused") == 0 && lines_with(log, "on again after") == 0 &&
                  lines_with(log, "OFF until the next") == 0 && lines_with(log, "minute's used up") == 0,
              fmt("said each time (%d of %zu), never paused: \"%s\"", said, r.w.jumps.size(),
                  line_with(log, "did nothing in 900 ms").c_str()));
    }
    // Ctrl + ↑ taken as a plain jump: a press that did not get there, tried
    // again -- the key never off for the run (2026.11.24; two such had Ctrl + ↑
    // off until the next start until then).
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::Plain;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(25'000); });
        // Learned since the 2026.11.24 review: that take-off with Ctrl+Up
        // skipped kLearnedForgetMs, so the next try is not the same jump from
        // the same column every 3 s; and out of the minute's count.
        const std::vector<GameMapBook::NoJump> nj = r.book.no_jumps("t52_s2");
        bool same_again = false;
        for (std::size_t i = 1; i < r.w.jumps.size(); ++i)
            same_again = same_again || (r.w.jumps[i].kind == r.w.jumps[0].kind && r.w.jumps[i].x == r.w.jumps[0].x &&
                                        r.w.jumps[i].at - r.w.jumps[0].at < 30'000);
        check(!r.w.jumps.empty() && r.w.jumps[0].kind == JumpKind::CtrlUp && !nj.empty() &&
                  nj[0].take.x == r.w.jumps[0].x && !same_again,
              fmt("%zu jumps; the first, a Ctrl+Up the game took as a plain jump: that take-off learned (%zu), the "
                  "same Ctrl+Up from the same column not pressed again within 30 s",
                  r.w.jumps.size(), nj.size()));
        check(lines_with(log, "Ctrl+Up came out as a plain jump (take-off at -20 a step, state 1) -- a press that did "
                              "not get there; the key stays on, that take-off skipped 30 s, tried again from "
                              "another") >= 1 &&
                  lines_with(log, "OFF until") == 0 && lines_with(log, "minute's used up") == 0,
              "said: \"" + line_with(log, "came out as") + "\"");
    }
    // Landed outside the area: that take-off skipped kLearnedForgetMs, the
    // walk back takes over -- the jumps up stay on (2026.11.24; Physics, off
    // until the next start, until then).
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::Outside;
        r.w.force_x = 586;
        r.w.force_y = 163;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(12'000); });
        const std::vector<GameMapBook::NoJump> nj = r.book.no_jumps("t52_s2");
        check(!r.w.jumps.empty() && !nj.empty() && nj[0].take.x == r.w.jumps[0].x &&
                  lines_with(log, "OUTSIDE the hunting area -- that take-off skipped 30 s, jumps up go on; the walk "
                                  "back takes over") >= 1 &&
                  lines_with(log, "OFF until") == 0,
              fmt("landed on ledge A, outside the box: the take-off at %d skipped, jumps up on -- \"%s\"",
                  r.w.jumps.empty() ? 0 : r.w.jumps[0].x, line_with(log, "OUTSIDE").c_str()));
    }
    // g out of 1300..3200 (2026.11.12c's gross guard; 2100..2800 until then):
    // said, and nothing more -- twice in a run had the jumps up off until the
    // next start until 2026.11.24.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.gravity = 1250.0;
        r.w.answer = Answer::Physics;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(9000); }, LogLevel::Info);
        const bool one = r.w.jumps.size() == 1;
        check(one && lines_with(log, "jump up worked", "g≈1") == 1,
              "a flight under g=1250: measured, said (\"" + line_with(log, "g≈") + "\")");
        // A second and a third such flight: said each time, the jumps on.
        for (int i = 0; i < 2; ++i) {
            const std::vector<std::string> l = info_lines([&] {
                r.hunter.set_zone(owner_zone());
                r.w.place(880, ground);
                r.run(12'000);
            });
            log.insert(log.end(), l.begin(), l.end());
        }
        check(r.w.jumps.size() == 3 && lines_with(log, "out of 1300..3200 -- logged only; jumps up go on") == 3 &&
                  lines_with(log, "OFF until") == 0,
              fmt("three flights under g=1250: each said, the jumps on (%zu jumps): \"%s\"", r.w.jumps.size(),
                  line_with(log, "logged only").c_str()));
    }
    // A map change 0.9 s after the key: both ends learned as gates, said once
    // for the window (PressGate::Jump), and the hunt goes on -- nothing
    // stopped, jump_up left ticked (2026.11.24; the hunter halted for the hunt
    // to stop and jump_up to be unticked until then).
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::MapChange;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(12'000); });
        const std::vector<MapGate> learned = r.book.learned_gates("t52_s2");
        const bool gate = !r.w.jumps.empty() && std::any_of(learned.begin(), learned.end(), [&](const MapGate& g) {
            return g.x == r.w.jumps[0].x && g.y == r.w.jumps[0].y;
        });
        const Hunter::PressGate said = r.hunter.take_press_gate();
        check(r.w.jumps.size() == 1 && said == Hunter::PressGate::Jump &&
                  r.hunter.take_press_gate() == Hunter::PressGate::None && r.st().press_gates == 1 && gate &&
                  r.book.learned_gates("t53_s1").size() == 1 && r.hunter.running() && r.hunter.config().jump_up,
              fmt("PressGate::Jump once, the take-off %d learned as a gate and the entry it led to; the hunter running, "
                  "jump_up on",
                  r.w.jumps.empty() ? 0 : r.w.jumps[0].x));
        check(lines_with(log, "Hunter: MAP CHANGED ", "s after Ctrl+Up at ") == 1 &&
                  lines_with(log, "Hunter: MAP CHANGED ", "hunting goes on, Nhảy lên stays ticked") == 1 &&
                  lines_with(log, "to be unticked") == 0,
              "said: \"" + line_with(log, "MAP CHANGED") + "\"");
        // It hunts on where it landed: a monster in reach there is fought.
        const std::uint64_t after = r.w.t;
        r.w.mobs = { mob_at(r.w.me_x + 60, r.w.me_y, 9) };
        r.run(4000);
        check(r.w.downs_of('Z', after) >= 1,
              fmt("hunting on after it: %d attacks on a monster in reach where it landed", r.w.downs_of('Z', after)));
    }
    // 2026.11.21 (the review of the audit's B3): the same jump up under the
    // expedition's window -- a member in the lobby moved in by its leader's
    // entry -- is no press's: nothing learned, jumps on, the hunt not stopped.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::MapChange;
        r.hunter.set_zone(owner_zone());
        r.hunter.expect_map_change(15'000);
        std::vector<std::string> log = info_lines([&] { r.run(12'000); });
        // The map's entry is learned as a gate by MapTrust as ever; the
        // take-off must not be.
        const std::vector<MapGate> learned = r.book.learned_gates("t52_s2");
        const bool gate = !r.w.jumps.empty() && std::any_of(learned.begin(), learned.end(), [&](const MapGate& g) {
            return g.x == r.w.jumps[0].x && g.y == r.w.jumps[0].y;
        });
        check(r.w.jumps.size() >= 1 && r.hunter.take_press_gate() == Hunter::PressGate::None &&
                  r.st().press_gates == 0 && !gate && lines_with(log, "the expedition's own map change") >= 1,
              fmt("the expedition's map change after a jump up: the press forgotten -- the take-off no gate, nothing "
                  "for the window to say (%zu jumps, %d gates)",
                  r.w.jumps.size(), r.st().press_gates));
    }
}

/// 2026.11.15: the game loses 30-40% of GoBot's short presses at random (the
/// log of 2026-11-15: Ctrl + ↑ did nothing at 4 of 5 first tries, the second
/// ~1-2 s on worked every time). A first press that did nothing passes its
/// monster over 1.2 s. Three in a row, or five in the run with more failed
/// than worked, paused the jumps up from 2026.11.15 -- 30 s, 60 s, 120 s --
/// and since 2026.11.24 (the owner: "khi hỏng thì thử lại luôn ... chứ không
/// phải là dừng") nothing does: every one is tried again.
void test_jump_misses_2026_11_15() {
    section("Hunter (2026.11.15, 2026.11.24): a jump up the game lost is no evidence -- the monster passed over 1.2 s; "
            "never a pause, however many in a row or in the run");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the jumps the game lost\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    const int ground = stand_y(p, 880, 634);

    // A monster on the ledge above: the first Ctrl+Up did nothing, the monster
    // passed over only kRetryPassOverMs -- the second press, from the same
    // take-off, comes within seconds (it came after 8 s and more before) and
    // works.
    {
        HunterConfig c = owner_config();
        c.spot_return_delay_ms = 60'000;
        c.keep_distance = true;
        JumpRig r(c, 600, stand_y(p, 600, 579));
        r.hunter.set_zone(owner_zone());
        r.run(Hunter::kDropAfterEntryMs + 300);
        r.w.answers = { Answer::Nothing };
        r.w.mobs = { mob_at(560, 273, 11) };
        std::vector<std::string> log = info_lines([&] {
            r.until([&] { return r.w.jumps.size() >= 2; }, 25'000);
            r.until([&] { return !r.w.flying; }, 3000);
            r.run(2000);
        });
        const std::uint64_t gap = r.w.jumps.size() >= 2 ? r.w.jumps[1].at - r.w.jumps[0].at : 0;
        const Hunter::Status st = r.st();
        check(r.w.jumps.size() == 2 && gap >= Hunter::kDropGapMs && gap < 6000 && st.jumps_worked == 1 &&
                  lines_with(log, "did nothing in 900 ms", "-- 1 in a row") == 1 &&
                  lines_with(log, "jump up worked") == 1,
              fmt("Ctrl+Up did nothing, the monster above passed over 1.2 s: the second press %llu ms after the "
                  "first, from x %d, and it worked (%llu)",
                  static_cast<unsigned long long>(gap), r.w.jumps.size() >= 2 ? r.w.jumps[1].x : 0,
                  static_cast<unsigned long long>(st.jumps_worked)));
    }
    // The way back's Ctrl+Up that did nothing over and over, for two minutes:
    // never a pause (2026.11.24; 30 s after three in a row, then 60 s, 120 s
    // and 120 s, from 2026.11.15) -- each press at the next look, kDropGapMs
    // or more after the last, and no wider gap than a settle and a judging.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::Nothing;
        r.hunter.set_zone(owner_zone());
        const std::vector<std::string> log = info_lines([&] { r.run(120'000); });
        bool spaced = r.w.jumps.size() >= 2;
        std::uint64_t widest = 0;
        for (std::size_t i = 1; i < r.w.jumps.size(); ++i) {
            const std::uint64_t gap = r.w.jumps[i].at - r.w.jumps[i - 1].at;
            spaced = spaced && gap >= Hunter::kDropGapMs;
            widest = std::max(widest, gap);
        }
        check(r.w.jumps.size() >= 30 && spaced && widest < 6000 && lines_with(log, "paused") == 0 &&
                  lines_with(log, "on again after") == 0 && lines_with(log, "OFF until the next") == 0,
              fmt("%zu presses that did nothing in 2 minutes, never a pause: each %llu ms or more after the last, the "
                  "widest gap %llu ms",
                  r.w.jumps.size(), static_cast<unsigned long long>(Hunter::kDropGapMs),
                  static_cast<unsigned long long>(widest)));
    }
    // Five that did nothing and five that worked, then a sixth that did
    // nothing: no pause, ever (2026.11.24; from 2026.11.15 the run's count
    // tripped at the sixth, more failed than worked). The presses go as the
    // game is scripted to answer them, one each: a jump that worked and the
    // character put back on the ground, one that did nothing and -- the way
    // back not done -- the next, by itself, which worked; the eleventh did
    // nothing again, and the twelfth comes at once.
    {
        JumpRig r(owner_config(), 880, ground);
        r.hunter.set_zone(owner_zone());
        r.w.answer = Answer::Nothing;
        r.w.answers = { Answer::Physics, Answer::Nothing, Answer::Physics, Answer::Nothing, Answer::Physics,
                        Answer::Nothing, Answer::Physics, Answer::Nothing, Answer::Physics, Answer::Nothing };
        const auto judged = [&](std::size_t n) {
            r.until([&] { return r.w.jumps.size() >= n; }, 120'000);
            r.until([&] { return !r.w.flying; }, 4000);
            r.run(1500);
        };
        std::uint64_t worked_at_ten = 0;
        std::uint64_t eleventh = 0;
        const std::vector<std::string> log = info_lines([&] {
            // Presses 1, 3, 5, 7, 9 worked -- the character put back on the
            // ground each time; 2, 4, 6, 8, 10 did nothing, each followed by
            // the next press by itself.
            for (std::size_t i = 1; i <= 10; ++i) {
                judged(i);
                if (i % 2 == 1) r.w.place(880, ground);
                if (i == 10) worked_at_ten = r.st().jumps_worked;
            }
            judged(11);
            eleventh = r.w.jumps.size() >= 11 ? r.w.jumps[10].at : 0;
            // The sixth that did nothing in the run: the next at once.
            r.until([&] { return r.w.jumps.size() >= 12; }, 8000);
        });
        const int did_nothing = lines_with(log, "did nothing in 900 ms");
        check(r.w.jumps.size() >= 11 && worked_at_ten == 5 && did_nothing >= 6,
              fmt("five jumps that worked and five that did nothing: %llu worked, %d that did nothing so far this run, "
                  "%zu presses",
                  static_cast<unsigned long long>(worked_at_ten), did_nothing, r.w.jumps.size()));
        check(r.w.jumps.size() >= 12 && r.w.jumps[11].at >= eleventh + Hunter::kDropGapMs &&
                  r.w.jumps[11].at < eleventh + 6000 && lines_with(log, "paused") == 0,
              fmt("the sixth that did nothing (6 in the run, more than the 5 that worked): the next press %llu ms on -- "
                  "never a pause",
                  r.w.jumps.size() >= 12 ? static_cast<unsigned long long>(r.w.jumps[11].at - eleventh) : 0ull));
    }
}

/// 2026.11.15, the owner's log of 00:27:54 and the night after it: a Ctrl+Up
/// hit at the top of its arc sent a 0x002A of state 0 -- taken for the landing --
/// at 2956,815, on no floor: the file was distrusted for the rest of the session
/// and the character stood outside its area from 01:10 to 08:07. A landing on no
/// floor now waits for the next report. A landing off the file's floors that
/// stands distrusted the file for kDistrustMs (120 s) from 2026.11.15, the
/// jumps off with it; since 2026.11.24 MapTrust lets the file go, and takes it
/// again from kTrustPoints standing points on its floors -- no clock, nothing
/// switched off.
void test_jump_apex_report() {
    section("Hunter (2026.11.15, 2026.11.24): a landing reported on no floor waits for the next report -- the apex's "
            "0x002A is no landing, the file stays trusted; a landing off the floors that stands has MapTrust let the "
            "file go until its standing points take it again, with no clock");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the apex report\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    const int ground = stand_y(p, 880, 634);

    // A: the report at the apex, then the landing on the home ledge as planned.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::Apex;
        r.hunter.set_zone(owner_zone());
        const std::vector<std::string> log = info_lines([&] { r.run(12'000); });
        check(r.w.jumps.size() == 1 && r.st().jumps_worked == 1 && !r.book.distrusted("t52_s2") &&
                  lines_with(log, "on no floor of") == 0 && lines_with(log, "Hunter: jump up worked: landed at ") == 1 &&
                  r.st().drop_map.why == MapTrust::Why::Trusted,
              fmt("a state-0 report in mid-air at the apex, the landing 0.4 s on: one jump up, worked (%llu), the file "
                  "not distrusted (%d), no \"on no floor\" (%d) -- \"%s\"",
                  static_cast<unsigned long long>(r.st().jumps_worked), r.book.distrusted("t52_s2") ? 1 : 0,
                  lines_with(log, "on no floor of"), line_with(log, "jump up").c_str()));
    }

    // B: a landing on no floor and no later report: after kLandSettleMs it
    // stands -- OffMap: MapTrust lets the file go (2026.11.24; the book
    // distrusted it 120 s, jumps off, until then). Three standing points of
    // yours on its floors and it is gone by again, at once -- and the way
    // back's jump goes.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answers = { Answer::Outside };
        r.w.force_x = 880;
        r.w.force_y = 120;                      // 154 px over the home ledge: on no floor
        r.hunter.set_zone(owner_zone());
        std::uint64_t off_at = 0;
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && !r.w.flying; }, 12'000);
            r.run(Hunter::kLandSettleMs + 600);
            off_at = r.w.t;
        });
        const bool off = r.st().drop_map.why != MapTrust::Why::Trusted && !r.book.distrusted("t52_s2") &&
                         lines_with(log, "Hunter: jump up landed at ", "on no floor of t52_s2's file -- the file let go "
                                                                     "of until 3 standing points on its floors agree "
                                                                     "again (no clock); jumps up go on then") == 1 &&
                         lines_with(log, "OFF until") == 0;
        // Put back on the ground, three standing points on its floors apart:
        // trusted again by them -- no clock -- and the next jump goes.
        r.w.answer = Answer::Physics;
        r.w.force_x = r.w.force_y = 0;
        const std::size_t jumps_before = r.w.jumps.size();
        const std::uint64_t back_from = r.w.t;
        const std::vector<std::string> then = info_lines([&] {
            for (const int x : { 720, 820, 940 }) {
                r.w.place(x, stand_y(p, x, 634));
                r.run(700);
            }
            r.until([&] { return r.w.jumps.size() > jumps_before && !r.w.flying; }, 15'000);
            r.run(500);
        });
        const MapTrust::Report t = r.st().drop_map;
        check(off && t.why == MapTrust::Why::Trusted && t.how == MapTrust::How::Points &&
                  r.w.jumps.size() > jumps_before && r.w.t - back_from < 30'000 &&
                  lines_with(then, "t52_s2 trusted by 3 standing points on its floors") == 1,
              fmt("OffMap after %llu ms on no floor (the file let go of: %d), trusted again by its standing points "
                  "%llu ms on (%s), and a jump went -- \"%s\"",
                  static_cast<unsigned long long>(off_at), off ? 1 : 0,
                  static_cast<unsigned long long>(r.w.t - back_from), MapTrust::why_text(t.why),
                  line_with(then, "trusted by").c_str()));
    }
}

/// The first live run of 2026.11.15 (10:15:51): the first report of a flight
/// that is neither a cast's nor a 0x002A -- a 0x0027 in mid-air, 478 ms into a
/// jump that landed at 1221 ms -- was taken for the landing (JumpCheck::
/// flight_of), judged off the file's floors, and the file distrusted for 120
/// s. The wait of kLandSettleMs now covers every kind of report, and starts
/// again at each later report that puts the character kLandSamePx or more
/// elsewhere.
void test_jump_mid_air_move() {
    section("Hunter (2026.11.15, the first live run): a 0x0027 in mid-air is no landing -- the wait covers any kind of "
            "report and starts again while the character moves");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the mid-air 0x0027\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    const int ground = stand_y(p, 880, 634);

    // A: the 0x0027 150 ms into the flight, the landing on the home ledge 0.7 s
    // after it.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::MoveEarly;
        r.hunter.set_zone(owner_zone());
        const std::vector<std::string> log = info_lines([&] { r.run(12'000); });
        check(r.w.jumps.size() == 1 && r.st().jumps_worked == 1 && r.st().drop_map.why == MapTrust::Why::Trusted &&
                  !r.book.distrusted("t52_s2") && lines_with(log, "on no floor of") == 0 &&
                  lines_with(log, "Hunter: jump up worked: landed at ") == 1,
              fmt("a 0x0027 in mid-air 150 ms into the flight, the landing 0.7 s on: one jump up, worked (%llu), the "
                  "file still trusted (%s), no \"on no floor\" (%d) -- \"%s\"",
                  static_cast<unsigned long long>(r.st().jumps_worked), MapTrust::why_text(r.st().drop_map.why),
                  lines_with(log, "on no floor of"), line_with(log, "jump up").c_str()));
    }

    // B: a landing 1.35 s into the flight (the live one's 1.2 s, and more): the
    // 0x0027 at 150 ms would be judged at 1.05 s, 0.3 s early -- but a cast in
    // mid-air at 700 ms puts the character elsewhere, and the wait starts again.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::MoveRenewed;
        r.w.slow_ms = 500;
        r.hunter.set_zone(owner_zone());
        const std::vector<std::string> log = info_lines([&] { r.run(12'000); });
        check(r.w.jumps.size() == 1 && r.st().jumps_worked == 1 && r.st().drop_map.why == MapTrust::Why::Trusted &&
                  !r.book.distrusted("t52_s2") && lines_with(log, "on no floor of") == 0 &&
                  lines_with(log, "Hunter: jump up worked: landed at ") == 1,
              fmt("a 0x0027 at 150 ms and a cast at 700 ms in mid-air, the landing at 1.35 s: worked (%llu), the file "
                  "still trusted (%s) -- \"%s\"",
                  static_cast<unsigned long long>(r.st().jumps_worked), MapTrust::why_text(r.st().drop_map.why),
                  line_with(log, "jump up").c_str()));
    }

    // The control of B: the same flight with no cast -- the character quiet for
    // kLandSettleMs at a place on no floor is taken as standing there, as ever:
    // off the map, the file let go of by MapTrust (2026.11.24: never the book's
    // distrust, never the jumps off).
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::MoveEarly;
        r.w.slow_ms = 500;
        r.hunter.set_zone(owner_zone());
        const std::vector<std::string> log = info_lines([&] { r.run(12'000); });
        check(lines_with(log, "Hunter: jump up landed at ", "on no floor of t52_s2's file -- the file let go of") >= 1 &&
                  !r.book.distrusted("t52_s2") && lines_with(log, "OFF until") == 0,
              fmt("the control: no cast, a 0x0027 at 150 ms and the landing at 1.35 s: judged off the map at 1.05 s -- "
                  "the file let go of, never distrusted; \"%s\" / \"%s\"",
                  line_with(log, "Hunter: jump up landed").c_str(), line_with(log, "Hunter: jump up worked").c_str()));
    }

    // C: the landing itself a 0x0027 on no floor, nothing after it: not judged
    // before kLandSettleMs, judged OffMap after it -- the file let go of.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answers = { Answer::Outside };
        r.w.force_x = 880;
        r.w.force_y = 120;                      // 154 px over the home ledge: on no floor
        r.w.land_as_move = true;
        r.hunter.set_zone(owner_zone());
        r.until([&] { return !r.w.jumps.empty(); }, 12'000);
        r.until([&] { return r.w.t >= r.w.land_at + Hunter::kLandSettleMs - 200; }, 6'000);
        const bool waiting = r.st().drop_map.why == MapTrust::Why::Trusted;
        r.run(600);
        const bool off = r.st().drop_map.why != MapTrust::Why::Trusted && !r.book.distrusted("t52_s2");
        check(waiting && off,
              fmt("a 0x0027 on no floor as the landing, nothing after: still waiting at kLandSettleMs - 200 ms (%d), "
                  "OffMap and the file let go of after it (%d)",
                  waiting ? 1 : 0, off ? 1 : 0));
    }
}

/// The review of 2026.11.15 (blind landing, a way back whose first leg is a
/// jump up where the character stands): a dash ended in the air, the last
/// report a cast's 36 px over the ground, no report of the landing. Pressed
/// from the projection the jump was refused (OffFloor) again and again -- the
/// press reads the real report, on no floor -- with the give-up clock running;
/// the walk now takes the first step, and its report puts the character right.
void test_jump_blind_landing() {
    section("Hunter (2026.11.15 review): a blind landing whose way back starts with a jump up where it stands -- a "
            "step first, then the press from a real stand");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the blind landing's jump up\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    const int ground = stand_y(p, 880, 634);
    JumpRig r(owner_config(), 880, ground);
    r.hunter.set_zone(owner_zone());
    // The cast's report in the air, and no report of the landing.
    r.w.report_x = 880.0f;
    r.w.report_y = static_cast<float>(ground - 36);
    r.w.report_ms = r.w.t;
    r.w.report_op = 0x0031;
    r.w.blind = true;
    const std::uint64_t t0 = r.w.t;
    const std::vector<std::string> log = info_lines([&] { r.run(15'000); });
    const bool in_time = !r.w.jumps.empty() && r.w.jumps[0].at >= t0 + Hunter::kBlindLandMs;
    const int refused = lines_with(log, "where you stand is on no floor of the map file");
    const int arrows = r.w.downs_of(VK_LEFT) + r.w.downs_of(VK_RIGHT);
    check(lines_with(log, "taken as landed on floor") == 1 && r.w.jumps.size() == 1 && r.st().jumps_worked == 1 &&
              refused == 0 && arrows >= 1 && in_time,
          fmt("a cast's report 36 px over the ground, no landing report: taken as landed after %d ms, a step first (%d "
              "arrows), then one jump up that worked (%llu jumps, %llu worked), none refused for standing on no floor "
              "(%d)",
              static_cast<int>(Hunter::kBlindLandMs), arrows, static_cast<unsigned long long>(r.w.jumps.size()),
              static_cast<unsigned long long>(r.st().jumps_worked), refused));
}

// ------------------------------------------------------------- the staircase

void test_jump_twice() {
    section("Hunter 05b: Up,Up where it is sure -- a made-up platform 180 px over solid ground; its second Up not "
            "sent, judged as a plain Up");
    const std::string dir = staircase_folder();
    check(!dir.empty(), "the made-up t98_s1 written to this run's temp folder");
    if (dir.empty()) return;
    HunterConfig c = owner_config();
    const auto rig = [&](JumpRig& r) {
        r.w.map = 98;
        r.w.area = 1;
        r.w.entry_x = 2000.0f;
        r.w.entry_y = 999.0f;
        r.hunter.set_zone(zone_box("t98_s1", 1000, 819, 200, 400, 2400, 1050));
    };
    {
        JumpRig r(c, 1000, 999, true, dir, "t98_s1");
        rig(r);
        r.until([&] { return r.st().spot_home && r.w.me_y == 819 && !r.w.walking(); }, 14'000);
        check(from_map(r) && r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::UpUp && r.st().jumps_worked == 1 &&
                  r.st().spot_home,
              fmt("from the ground at 1000: one Up,Up (%s), onto the platform, home",
                  r.w.jumps.empty() ? "none" : jump_kind_name(r.w.jumps[0].kind)));
    }
    {
        JumpRig r(c, 1000, 999, true, dir, "t98_s1");
        rig(r);
        r.w.second_cut = true;
        std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && !r.w.flying; }, 12'000);
            r.run(600);
        });
        check(r.w.jumps.size() == 1 && r.st().jumps_fell_back == 0 && r.book.no_jumps("t98_s1").empty() &&
                  lines_with(log, "OFF until") == 0 &&
                  lines_with(log, "second Up not sent (a hold, or too late) -- judged as a plain Up") == 1,
              "the second Up not sent: a plain Up, back where it stood -- no fall back counted, nothing learned (\"" +
                  line_with(log, "second Up not sent") + "\")");
    }
}

// ------------------------------------------------------------- off, and the rest

void test_jump_off() {
    section("Hunter 05b: jump_up off -- the same keys as the hunter without jumps, one \"would jump\" line; the "
            "config; the map book's learning; MapTrust in the air");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: jump_up off\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");

    // T-J8: jump_up off, wired, against the hunter with no air counts at all
    // (the command line's): the same keys at the same times.
    for (const int start : { 0, 1 }) {
        HunterConfig off = owner_config();
        off.jump_up = false;
        const int x = start == 0 ? 1342 : 600, y = start == 0 ? 772 : 579;
        JumpRig a(off, x, stand_y(p, x, y));
        JumpRig b(owner_config(), x, stand_y(p, x, y), false);
        for (JumpRig* r : { &a, &b }) {
            r->hunter.set_zone(owner_zone());
            if (start == 1) r->w.mobs = { mob_at(560, 273, 11), mob_at(300, stand_y(p, 300, 531), 12) };
        }
        std::vector<std::string> log = info_lines([&] {
            a.run(20'000);
            b.run(20'000);
        });
        check(!a.w.keys.empty() && key_log(a.w) == key_log(b.w) && a.w.jumps.empty() && b.w.jumps.empty(),
              fmt("from %d,%d: %zu keys, the same with jump_up off as with no jumps wired; no jump", x, y,
                  a.w.keys.size()));
        check(lines_with(log, "Hunter: a jump up would go here (jump_up is off): Ctrl+Up at x ") == 1 &&
                  a.st().jumps_would >= 1 && b.st().jumps_would == 0,
              "said once: \"" + line_with(log, "would go here") + "\"");
        // And kept for the map tab's dashed arrow, as worked out at the last pass.
        check(a.st().jump_would_ms != 0 && a.st().jump_would_plan.kind == JumpKind::CtrlUp &&
                  a.st().jump_would_plan.land_top < a.st().jump_would_plan.feet_y && b.st().jump_would_ms == 0,
              fmt("the jump that would have gone kept for the map tab: Ctrl+Up at x %d onto floor %d",
                  a.st().jump_would_plan.x, a.st().jump_would_plan.land_top));
    }

    // The config: saved and read back; a file without it reads on; the
    // shipped hunt.ini says jump_up=true.
    {
        const std::string path = temp_path("jump_up.ini");
        HunterConfig c;
        c.jump_up = false;
        HunterConfig back;
        const bool saved = c.save(path) && back.load(path);
        std::ofstream(path) << "[Hunt]\njump_key=38\n";
        HunterConfig bare;
        bare.jump_up = false;
        check(HunterConfig{}.jump_up && saved && !back.jump_up && bare.load(path) && bare.jump_up,
              "jump_up: on by default, saved off and read back off, missing from a file -- on");
        DeleteFileA(path.c_str());
#ifdef GOBOT_SOURCE_DIR
        IniFile ini;
        HunterConfig ship;
        const std::string shipped = std::string(GOBOT_SOURCE_DIR) + "\\configs\\hunt.ini";
        check(ini.load(shipped) && ini.has_key("Hunt", "jump_up") && ship.load(shipped) && ship.jump_up,
              "the shipped hunt.ini: jump_up=true (the owner's answer 5)");
#endif
    }

    // The map book: no-jump points and floors solid from below, learned and
    // taken back.
    {
        GameMapBook book{ std::string() };     // no data folder, never looked up
        book.learn_no_jump("t52_s2", GameMapBook::NoJump{ MapPoint{ 810, 603 }, MapPoint{ 810, 273 }, 2 });
        book.learn_solid("t52_s2", MapPoint{ 810, 273 });
        book.learn_solid("t52_s2", MapPoint{ 810, 273 });
        const bool had = book.no_jumps("t52_s2").size() == 1 && book.solid_floors("t52_s2").size() == 1 &&
                         book.no_jumps("t52_s1").empty();
        check(had && book.unlearn_no_jump("t52_s2", MapPoint{ 810, 603 }) &&
                  !book.unlearn_no_jump("t52_s2", MapPoint{ 810, 603 }) &&
                  book.unlearn_solid("t52_s2", MapPoint{ 810, 273 }) &&
                  book.no_jumps("t52_s2").empty() && book.solid_floors("t52_s2").empty(),
              "GameMapBook: a no-jump point and a solid floor, once each, per map; taken back by their points");
    }

    // A cast in the air reports you settled at the height you are: never a
    // standing point for MapTrust -- the area's file stays trusted.
    {
        JumpRig r(owner_config(), 880, stand_y(p, 880, 634));
        r.w.cast_in_air = true;
        r.hunter.set_zone(owner_zone());
        r.until([&] { return !r.w.jumps.empty(); }, 12'000);
        r.run(1200);
        const MapTrust::Report t = r.st().drop_map;
        check(!r.w.jumps.empty() && t.why == MapTrust::Why::Trusted && !t.own_off && !r.book.distrusted("t52_s2"),
              fmt("casts reported mid-flight: the file still trusted (%s)", MapTrust::why_text(t.why)));
    }
}

#ifdef GOBOT_SOURCE_DIR
std::string source(const char* rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
#endif

void test_jump_hop() {
    section("Hunter 05b: the old hop on a walk back the map has no jump in (as_before) -- kept where jumps up cannot "
            "be planned (another jump key, Watch), as 2026.11.05; off with jump_up where they can");
    const std::string dir = hop_floor_folder();
    check(!dir.empty(), "the made-up t97_s1 written to this run's temp folder");
    if (dir.empty()) return;
    // The home on the platform 100 px over the ground, the box round the
    // platform alone: from the ground at 400, 94 px under the box, no walk or
    // fall gets in -- the home, one jump above, walked toward as before.
    struct Case {
        const char*     what;
        std::uint32_t   key;
        PacketPositions pos;
        bool            jump_up;
        bool            hops;
    };
    const Case cases[] = {
        { "jump key C, packets on, jump_up on (no jump up can be planned)", 'C', PacketPositions::On, true, true },
        { "Watch, jump key Up, jump_up on (no jump up can be planned)", 38, PacketPositions::Watch, true, true },
        { "jump_up off, jump key Up, packets on (2026.11.05's walk back)", 38, PacketPositions::On, false, true },
        { "jump_up on, jump key Up, packets on (jumps up planned)", 38, PacketPositions::On, true, false },
    };
    for (const Case& k : cases) {
        HunterConfig c = owner_config();
        c.jump_key = k.key;
        c.packet_positions = k.pos;
        c.jump_up = k.jump_up;
        JumpRig r(c, 400, 999, true, dir, "t97_s1");
        r.w.map = 97;
        r.w.area = 1;
        r.w.entry_x = 2000.0f;
        r.w.entry_y = 999.0f;
        r.hunter.set_zone(zone_box("t97_s1", 1000, 899, 200, 850, 2400, 905));
        r.run(8000);
        const int hops = r.w.downs_of(k.key);
        check(from_map(r) && (k.hops ? hops >= 1 : hops == 0 && !r.w.jumps.empty()),
              fmt("%s: %d hop(s) toward the home, %zu planned jump(s)", k.what, hops, r.w.jumps.size()));
    }
}

// ------------------------------------------------- the way back (2026.11.12)

/// A JumpRig on one of way_back_folder's maps: its id, the entry's spawn by
/// its gate.
void on_map(JumpRig& r, std::uint16_t map, float entry_x, float entry_y, std::uint16_t area = 1) {
    r.w.map = map;
    r.w.area = area;
    r.w.entry_x = entry_x;
    r.w.entry_y = entry_y;
}

void test_way_back_levels() {
    section("Hunter 2026.11.12 (the owner's Q4): fallen two levels out of the area -- the way back over the map's "
            "other floors, walked to and up by Ctrl+Up; a run spot the same; no way: standing, no key, one line");
    const std::string dir = way_back_folder();
    check(!dir.empty(), "the made-up maps written to this run's temp folder");
    if (dir.empty()) return;
    GameMapBook probe(dir);
    const std::shared_ptr<const GameMapBook::Page> page = probe.get("t96_s1");
    check(page != nullptr, "t96_s1 read");
    if (!page) return;
    const GameMapBook::Page& p = *page;

    // The planner alone: from the ground at 1500 the box round the home on
    // its platform gets nothing that lands inside it -- the platform at 860
    // under the box is a floor outside it within 650 above (the ceiling
    // guard) -- and over the whole map one Ctrl+Up onto the home's floor.
    HuntZone box;
    box.home_x = 750;
    box.home_y = 719;
    box.left = 550;
    box.top = 640;
    box.right = 950;
    box.bottom = 790;
    JumpRules up;
    const std::optional<ZoneReturn> inside = return_point(p, box, MapPoint{ 1500, 999 }, &up);
    const HuntZone whole = zone_whole_map(p, box);
    const std::optional<ZoneReturn> travel = return_point(p, box, MapPoint{ 1500, 999 }, &up, &whole);
    check(!inside && travel && travel->home && travel->travel && travel->jumps == 1 && travel->jump &&
              travel->jump->kind == JumpKind::CtrlUp && travel->jump->land_top == 720 &&
              travel->jump->x >= 424 && travel->jump->x <= 1076 && whole.left == 0 && whole.top == 0 &&
              whole.right == 2559 && whole.bottom == 1279,
          fmt("return_point: none inside the box; over the whole map the home by one %s at x %d onto floor %d",
              travel && travel->jump ? jump_kind_name(travel->jump->kind) : "-", travel && travel->jump ? travel->jump->x : 0,
              travel && travel->jump ? travel->jump->land_top : 0));
    // Without jumps, nothing over the whole map either: never as before.
    const std::optional<ZoneReturn> walking = return_point(p, box, MapPoint{ 1500, 999 }, nullptr, &whole);
    check(!walking, "with no jumps up, no way over the whole map either -- and none \"as before\"");

    // The map tab's area: home on its platform, the character fallen to the
    // ground at 1500 -- two levels down, 209 px under the box and 550 right
    // of it.
    {
        JumpRig r(owner_config(), 750, 719, true, dir, "t96_s1");
        on_map(r, 96, 2000.0f, 991.0f);
        r.hunter.set_zone(zone_box("t96_s1", 750, 719, 550, 640, 950, 790));
        r.run(Hunter::kDropAfterEntryMs + 600);
        const bool used = from_map(r) && r.st().spot_outside_px == 0;
        r.w.place(1500, 999);
        std::uint64_t key = 0;
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty(); }, 15'000);
            if (!r.w.jumps.empty()) key = r.w.jumps.front().at;
            r.until([&] { return r.st().spot_outside_px == 0 && !r.w.walking() && !r.w.flying; }, 10'000);
            r.run(1500);
        });
        const bool one = r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::CtrlUp;
        check(used && one && r.w.jumps[0].y == 999 && r.w.jumps[0].x >= 424 && r.w.jumps[0].x <= 1076,
              fmt("walked left along the ground and one Ctrl+Up from %d,%d", one ? r.w.jumps[0].x : 0,
                  one ? r.w.jumps[0].y : 0));
        check(r.w.me_y == 719 && r.st().spot_outside_px == 0 && r.st().jumps_worked == 1 && !r.st().spot_stranded,
              fmt("landed on the home's floor and back inside the area: %d,%d (%d px outside)", r.w.me_x, r.w.me_y,
                  r.st().spot_outside_px));
        check(r.w.arrows_in_air() == 0 && r.w.downs_of(VK_DOWN) == 0 && r.w.downs_of(38) == 0 && key != 0 &&
                  r.w.downs_of(VK_RIGHT, 0, key) == 0,
              "no arrow in the air, no Down, no hop, nothing walked right before the jump");
        check(lines_with(log, "Hunter: walking back into the hunting area from 1500,999 (", "over floors outside the "
                                                                                           "area: 0 walk-offs and 1 "
                                                                                           "jump up in all") == 1 &&
                  lines_with(log, "Hunter: jump up (Ctrl+Up) on the way home: from ", "over floors outside the area, "
                                                                                      "on the way back into it") == 1 &&
                  lines_with(log, "Hunter: jump up worked: landed at ", "on floor 720") == 1 &&
                  lines_with(log, "OUTSIDE the hunting area") == 0,
              "said: \"" + line_with(log, "walking back into") + "\" / \"" + line_with(log, "jump up (Ctrl+Up)") +
                  "\"");
    }

    // This run's spot, taken on the home's platform with the file trusted:
    // the same way back -- the map tab's treatment once its file is trusted.
    {
        JumpRig r(owner_config(), 750, 719, true, dir, "t96_s1");
        on_map(r, 96, 2000.0f, 991.0f);
        r.run(Hunter::kDropAfterEntryMs + 600);
        const Hunter::Spot s = r.hunter.spot();
        const bool taken = s.set && !s.from_map && std::lround(s.x) == 750 && std::lround(s.y) == 719;
        r.w.place(1500, 999);
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty(); }, 15'000);
            r.until([&] { return r.st().spot_home && !r.w.walking() && !r.w.flying; }, 12'000);
            r.run(1000);
        });
        check(taken && r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::CtrlUp && r.w.me_y == 719 &&
                  std::abs(r.w.me_x - 750) <= 48 && r.st().spot_home && r.w.downs_of(VK_DOWN) == 0,
              fmt("this run's spot at 750,719: one Ctrl+Up from the ground, then home at %d,%d (%zu jump(s))",
                  r.w.me_x, r.w.me_y, r.w.jumps.size()));
        check(lines_with(log, "over floors outside the area") >= 1 && lines_with(log, "higher than one jump") == 0,
              "said as the map tab's way back, never \"higher than one jump\": \"" +
                  line_with(log, "walking back into") + "\"");
    }

    // jump_up off: no way back at all -- standing where it is, no arrow, no
    // jump; said once. A monster in reach outside the area: never attacked.
    {
        HunterConfig c = owner_config();
        c.jump_up = false;
        c.attack_key = 'Z';
        JumpRig r(c, 750, 719, true, dir, "t96_s1");
        on_map(r, 96, 2000.0f, 991.0f);
        r.hunter.set_zone(zone_box("t96_s1", 750, 719, 550, 640, 950, 790));
        r.run(Hunter::kDropAfterEntryMs + 600);
        r.w.place(1500, 999);
        r.w.mobs = { mob_at(1700, 999, 3) };
        const std::size_t keys0 = r.w.keys.size();
        bool held = true;
        const std::vector<std::string> log = info_lines([&] {
            r.run(400);
            r.run(4000, [&] {
                if (r.hunter.skill_hold() != Hunter::SkillHold::Outside) held = false;
            });
        });
        int arrows = 0, swings = 0;
        for (std::size_t i = keys0; i < r.w.keys.size(); ++i) {
            if (r.w.keys[i].vk == VK_LEFT || r.w.keys[i].vk == VK_RIGHT) ++arrows;
            if (r.w.keys[i].vk == 'Z') ++swings;
        }
        const Hunter::Status st = r.st();
        check(arrows == 0 && swings == 0 && r.w.jumps.empty() && st.spot_stranded && held &&
                  st.spot_why == Hunter::SpotWhy::NoReturn && r.w.me_x == 1500,
              fmt("no way back: no arrow (%d), no swing at the monster in reach outside (%d), no jump; stranded, the "
                  "skills held as Outside",
                  arrows, swings));
        check(lines_with(log, "Hunter: 550 px outside the hunting area at 1500,999 -- no way back the map shows (here: "
                              "Nhảy lên bục trên is off): standing where it is until a way back shows") == 1 &&
                  lines_with(log, "Hunter: no way back into the hunting area") == 0,
              "said once: \"" + line_with(log, "standing where it is") + "\"");
    }
}

void test_way_back_ramp() {
    section("Hunter 2026.11.12 (A3, the owner's Q4): this run's spot 33 px over its floor, the character 130 px "
            "lower on a ramp to it, outside the area -- walked back up the ramp, latched, never \"too high\"");
    const std::string dir = way_back_folder();
    if (dir.empty()) return;
    // The spot taken at 150,967 before the file is trusted -- 33 px over the
    // floor at 1000, as t62_s5's at its spawn -- then the character on the
    // ramp at 625, its floor 1130 there.
    JumpRig r(owner_config(), 150, 967, true, dir, "t95_s1");
    on_map(r, 95, 2000.0f, 1199.0f);
    r.pass();
    const Hunter::Spot s = r.hunter.spot();
    const bool taken = s.set && !s.from_map && std::lround(s.y) == 967;
    r.run(Hunter::kDropAfterEntryMs + 600);
    r.w.place(625, 1129);
    bool latched = false;
    const std::vector<std::string> log = info_lines([&] {
        r.until([&] {
            latched = latched || r.st().spot_back_walk;
            return r.st().spot_home && !r.w.walking();
        }, 12'000);
        r.run(600);
    });
    check(taken && latched && r.st().spot_home && r.w.me_x <= 198 && r.w.me_y == 999 && r.w.jumps.empty() &&
              r.w.downs_of(38) == 0,
          fmt("walked back up the ramp, latched, to %d,%d: home on the floor under the spot (no jump, no hop)", r.w.me_x,
              r.w.me_y));
    check(lines_with(log, "higher than one jump") == 0 &&
              lines_with(log, "Hunter: 175 px outside the hunting area -- no fight until the character is back inside "
                              "it: walking back first, to ") == 1 &&
              lines_with(log, "Hunter: walking back into the hunting area from 625,1129 (175 px outside it): to "
                              "150,999, the home") == 1,
          "said: \"" + line_with(log, "walking back into") + "\"");
}

void test_way_back_put_back() {
    section("Hunter 2026.11.12 (the owner's Q5): t62_s5 at 21:50:37 -- 3166,5446, then 3175,0, then 3166,723 with "
            "no walk, jump or map entry: put back by the game after a pit, the way back worked out from 3166,723");
    const std::string dir = way_back_folder();
    if (dir.empty() || !path_exists(dir + "\\Map\\t62_s5.map")) {
        skip("no copy of the game's t62_s5.map here (build\\handoff\\drop\\testdata): the put-back on t62_s5\n");
        return;
    }
    GameMapBook probe(dir);
    const GameMapBook::Page& p = *probe.get("t62_s5");
    // The spot at 309 on the floor by the 피스존 gate, as at 21:48:27.
    JumpRig r(owner_config(), 309, stand_y(p, 309, 5136), true, dir, "t62_s5");
    on_map(r, 62, 265.0f, 5104.0f, 5);
    r.run(Hunter::kDropAfterEntryMs + 600);
    const bool taken = r.hunter.spot().set && r.st().drop_map.why == MapTrust::Why::Trusted;
    std::uint64_t zero_at = 0, put_at = 0;
    const std::vector<std::string> log = info_lines([&] {
        r.w.place(3166, stand_y(p, 3166, 5446));
        r.run(600);
        r.w.held.clear();
        r.w.place(3175, 0);
        zero_at = r.w.t;
        r.run(300);
        r.w.place(3166, stand_y(p, 3166, 723));
        put_at = r.w.t;
        r.run(4000);
    });
    check(taken && lines_with(log, "Hunter: the game put you at 3166,723") == 1 &&
              lines_with(log, "walking back", "3175,0") == 0 && lines_with(log, "outside the hunting area at 3175") == 0,
          "said: \"" + line_with(log, "the game put you at") + "\" -- nothing walked on the y 0 figures");
    // From the bottom's right end: over the map's floors -- along the bottom
    // and up -- back to the spot. From the top: the way asked afresh from
    // 3166,723; walking and falling end on the platform across the whole map
    // at about 3000, which only Ctrl + Down goes through -- with Nhảy xuống
    // off, no way: standing, said so (2026.11.12 review).
    check(lines_with(log, "Hunter: walking back into the hunting area from 3166,5446 (", "over floors outside the "
                                                                                        "area") == 1,
          "from 3166,5446 the way back over the map's floors: \"" +
              line_with(log, "walking back into the hunting area from 3166,5446") + "\"");
    check(lines_with(log, "Hunter: 4315 px outside the hunting area at 3166,723 -- no way back the map shows but by "
                          "Ctrl+Down, and Nhảy xuống is off (here: Up ", "): standing where it is") == 1 &&
              lines_with(log, "Hunter: walking back into the hunting area from 3166,723") == 0 &&
              r.w.downs_of(VK_LEFT, put_at + Hunter::kPutBackWaitMs + 100) == 0 &&
              r.w.downs_of(VK_RIGHT, put_at + Hunter::kPutBackWaitMs + 100) == 0 && r.st().spot_stranded,
          "from 3166,723 the way asked afresh -- none the map shows: stranded, no arrow, said once: \"" +
              line_with(log, "at 3166,723 --") + "\"");
    // Nothing pressed from the report at y 0 until the put-back was agreed.
    const int arrows = r.w.downs_of(VK_LEFT, zero_at, put_at + Hunter::kPutBackWaitMs - 100) +
                       r.w.downs_of(VK_RIGHT, zero_at, put_at + Hunter::kPutBackWaitMs - 100);
    check(arrows == 0 && r.w.downs_of(VK_DOWN) == 0,
          fmt("no arrow from the y 0 report until the put-back was agreed (%d), no Down", arrows));

    // Nhảy xuống on (the owner's own hunt.ini of 2026-11-11): from the top the
    // way back drops through the platform across the map -- Ctrl + Down,
    // every Down in its chord, settled at its take-off, held still in the
    // fall -- and on down and back to the spot (2026.11.12 review, the
    // owner's Q4 "drop where it leads down").
    {
        HunterConfig c = owner_config();
        c.drop_down = true;
        JumpRig d(c, 309, stand_y(p, 309, 5136), true, dir, "t62_s5");
        d.w.drops_fall = true;
        on_map(d, 62, 265.0f, 5104.0f, 5);
        d.run(Hunter::kDropAfterEntryMs + 600);
        const bool set = d.hunter.spot().set && d.st().drop_map.why == MapTrust::Why::Trusted;
        std::uint64_t put = 0;
        bool home = false;
        const std::vector<std::string> dl = info_lines([&] {
            d.w.place(3166, stand_y(p, 3166, 5446));
            d.run(600);
            d.w.held.clear();
            d.w.place(3175, 0);
            d.run(300);
            d.w.place(3166, stand_y(p, 3166, 723));
            put = d.w.t;
            home = d.until([&] { return d.st().spot_home && !d.w.flying; }, 240'000);
        });
        // Every Down in a chord: Ctrl 50 ms before it.
        bool chorded = !d.w.chords.empty();
        for (const JumpWorld::Key& k : d.w.keys) {
            if (k.vk != VK_DOWN) continue;
            bool ctrl = false;
            for (const JumpWorld::Key& c2 : d.w.keys)
                if (c2.vk == VK_CONTROL && c2.at + 50 == k.at) ctrl = true;
            chorded = chorded && ctrl;
        }
        check(set && lines_with(dl, "Hunter: walking back into the hunting area from 3166,723 (") == 1 &&
                  lines_with(dl, "no way back the map shows") == 0 && !d.w.chords.empty() &&
                  lines_with(dl, "Hunter: drop (Ctrl+Down) on the way back:") >= 1,
              fmt("from 3166,723 the way back over the map's floors with %zu drop(s): \"%s\"; \"%s\"", d.w.chords.size(),
                  line_with(dl, "walking back into the hunting area from 3166,723").c_str(),
                  line_with(dl, "drop (Ctrl+Down) on the way back").c_str()));
        check(chorded && static_cast<std::size_t>(d.w.downs_of(VK_DOWN)) == d.w.chords.size() &&
                  lines_with(dl, "drop worked") == static_cast<int>(d.w.chords.size()) &&
                  lines_with(dl, "drops OFF") == 0 && d.st().drop_map.why == MapTrust::Why::Trusted,
              fmt("each Down in a Ctrl chord (%d Downs, %zu chords), each drop worked, none latched, the file still "
                  "trusted",
                  d.w.downs_of(VK_DOWN), d.w.chords.size()));
        check(home && d.w.downs_of(VK_LEFT, put) + d.w.downs_of(VK_RIGHT, put) >= 1,
              fmt("back at the spot (%s), now at %d,%d, %.0f s after the put-back", home ? "home" : "not home",
                  d.w.me_x, d.w.me_y, static_cast<double>(d.w.t - put) / 1000.0));
    }
}

void test_pit_edge() {
    section("Hunter 2026.11.12 (the owner's Q5): a platform's end with nothing under it -- the walk toward it let go "
            "of, the dash's keys held near it, no drop through it");
    const std::string dir = way_back_folder();
    if (dir.empty()) return;
    // No spot, drops on -- the map looked for, trusted by the entry by its
    // gate -- the monster level across the pit: no drop to it.
    HunterConfig c = owner_config();
    c.spot = false;
    c.drop_down = true;
    JumpRig r(c, 300, 799, true, dir, "t94_s1");
    on_map(r, 94, 200.0f, 799.0f);
    r.run(Hunter::kDropAfterEntryMs + 600);
    const bool trusted = r.st().drop_map.why == MapTrust::Why::Trusted;
    r.w.mobs = { mob_at(1400, 799, 4) };
    const std::vector<std::string> log = info_lines([&] { r.run(6000); });
    check(trusted && r.w.max_x <= 1000 && r.w.max_x >= 1000 - kZoneWalkAheadPx - 40 && !r.w.walking(),
          fmt("walked toward the monster across the pit, let go of before the platform's end: furthest %d", r.w.max_x));
    check(lines_with(log, "Hunter: not walking right -- the floor ends at 1000,799 with nothing of the map file under "
                          "it (a pit") == 1,
          "said once: \"" + line_with(log, "not walking right") + "\"");
    // The dash's keys at the edge, the character facing it -- a dash
    // learned: none is held while none is (2026.11.17).
    r.w.mobs.clear();
    r.hunter.set_dash('C', 320);
    r.w.place(950, 799);
    r.run(300);
    const bool edge = r.hunter.dash_edge();
    r.hunter.set_dash(0, 0);
    r.run(300);
    const bool none = !r.hunter.dash_edge();
    r.w.place(300, 799);
    r.run(300);
    check(edge && none, "50 px from the pit's edge: the dash's landing past it -- its key held (dash_edge); the "
                        "dash not learned, nothing held there");
    // A drop through the platform: nothing to land on.
    Hunter::DropFacts f;
    f.press = true;
    f.now = 100'000;
    f.wired = true;
    f.packet_target = true;
    f.dx = 0;
    f.dy = 300;
    f.target_wx = 600.0f;
    f.target_wy = 1100.0f;
    f.fix.state = WorldFix::State::Known;
    f.fix.x = f.fix.rx = 600.0f;
    f.fix.y = f.fix.ry = 799.0f;
    f.fix.report_ms = 99'960;
    f.entry_ms = 10'000;
    f.settle_from_ms = 99'940;
    f.page = r.book.get("t94_s1");
    const Hunter::DropCall d = Hunter::drop_rule(c, f);
    check(d.why == Hunter::DropWhy::NoLanding,
          fmt("Ctrl+Down from 600,799 over the pit: refused, nothing to land on (%s)", Hunter::drop_why_name(d.why)));
}

/// The way back's drops (2026.11.12 review, the owner's Q4 "drop where it
/// leads down"): a take-off judged as the drop rule judges a press -- a
/// platform, a landing either side, no gate in the fall's band, no no-drop
/// point, never a pit -- and only where nothing else gets back.
void test_way_back_drops() {
    section("Hunter 2026.11.12 review (the owner's Q4): the way back's Ctrl+Down legs -- under a lid only Ctrl+Down "
            "passes, by the drop rule's own tests: a gate near or a no-drop point moves the take-off, a pit has none");
    const std::string dir = way_back_folder();
    if (dir.empty()) return;
    GameMapBook probe(dir);
    const std::shared_ptr<const GameMapBook::Page> lid = probe.get("t92_s1");
    const std::shared_ptr<const GameMapBook::Page> pit = probe.get("t94_s1");
    if (!lid || !pit) {
        check(false, "the made-up maps t92_s1 and t94_s1 read");
        return;
    }
    HuntZone z;
    z.home_x = 1200;
    z.home_y = 999;
    z.left = 1000;
    z.top = 950;
    z.right = 1400;
    z.bottom = 1050;
    const HuntZone legs = zone_whole_map(*lid, z);
    const MapPoint from{ 1200, 499 };
    ZoneDropRules rules;
    rules.gate_px = 150;
    const std::optional<ZoneReturn> none = return_point(*lid, z, from, nullptr, &legs);
    const std::optional<ZoneReturn> down = return_point(*lid, z, from, nullptr, &legs, &rules);
    check(!none && down && down->home && down->drops == 1 && down->drop && down->drop->take.x == 1200 &&
              down->drop->take.y == 499 && down->drop->land.y == 999 && down->walk_x == 1200 && down->travel &&
              !down->own_floor(),
          fmt("on the lid at 1200,499: no way by walking and falling; with drops, one Ctrl+Down where it stands onto "
              "the ground, home (%d drop(s), take-off %d,%d, landing %d,%d)",
              down ? down->drops : -1, down && down->drop ? down->drop->take.x : 0,
              down && down->drop ? down->drop->take.y : 0, down && down->drop ? down->drop->land.x : 0,
              down && down->drop ? down->drop->land.y : 0));
    ZoneDropRules gated = rules;
    gated.gates = { MapGate{ 1250, 700, 0 } };
    const std::optional<ZoneReturn> by_gate = return_point(*lid, z, from, nullptr, &legs, &gated);
    ZoneDropRules learned = rules;
    learned.no_drop = [](MapPoint p) { return std::abs(p.x - 1200) <= GameMapBook::kNoDropXPx; };
    const std::optional<ZoneReturn> by_point = return_point(*lid, z, from, nullptr, &legs, &learned);
    check(by_gate && by_gate->drop && std::abs(by_gate->drop->take.x - 1250) > 150 + kZoneDropLeadPx &&
              by_point && by_point->drop && std::abs(by_point->drop->take.x - 1200) > GameMapBook::kNoDropXPx,
          fmt("a gate 50 px across in the fall's band: the take-off moved to %d; a no-drop point at 1200: to %d",
              by_gate && by_gate->drop ? by_gate->drop->take.x : 0,
              by_point && by_point->drop ? by_point->drop->take.x : 0));
    const HuntZone whole = zone_whole_map(*pit, z);
    check(!zone_drop_from(*pit, whole, { 600, 799 }, rules) && !zone_drop_from(*lid, legs, { 1200, 999 }, rules) &&
              zone_drop_from(*lid, legs, { 1200, 499 }, rules).has_value(),
          "no drop through a platform over a pit, nor through the bottom ground; one through the lid");

    // The final check of 2026.11.12: a way that starts with a drop keeps that
    // drop as its first leg -- the leg on its landing floor no longer takes
    // its own jump or drop for the first, to be pressed from the lid.
    const std::shared_ptr<const GameMapBook::Page> ledge = probe.get("t90_s1");
    const std::shared_ptr<const GameMapBook::Page> two = probe.get("t89_s1");
    if (!ledge || !two) {
        check(false, "the made-up maps t90_s1 and t89_s1 read");
        return;
    }
    HuntZone on_ledge;
    on_ledge.home_x = 1200;
    on_ledge.home_y = 899;
    on_ledge.left = 1120;
    on_ledge.right = 1280;
    on_ledge.top = 850;
    on_ledge.bottom = 920;
    const HuntZone ledge_legs = zone_whole_map(*ledge, on_ledge);
    ZoneDropRules right_no = rules;
    right_no.no_drop = [](MapPoint p) { return p.x >= 950; };
    JumpRules up;
    const std::optional<ZoneReturn> dj =
        return_point(*ledge, on_ledge, MapPoint{ 1500, 499 }, &up, &ledge_legs, &right_no);
    check(dj && dj->home && dj->drops == 1 && dj->jumps == 1 && dj->drop && !dj->jump && dj->drop->take.y == 499 &&
              dj->drop->take.x < 950 && dj->walk_x == dj->drop->take.x,
          fmt("on the lid at 1500,499, no drop from x 950 on, home on a ledge: Ctrl+Down first at %d,%d, the walk to "
              "x %d, the jump up left for the ground (%d drop(s), %d jump(s), a jump first: %s)",
              dj && dj->drop ? dj->drop->take.x : 0, dj && dj->drop ? dj->drop->take.y : 0, dj ? dj->walk_x : 0,
              dj ? dj->drops : -1, dj ? dj->jumps : -1, dj && dj->jump ? "yes" : "no"));
    HuntZone ground;
    ground.home_x = 1200;
    ground.home_y = 999;
    ground.left = 1100;
    ground.right = 1300;
    ground.top = 950;
    ground.bottom = 1050;
    const HuntZone two_legs = zone_whole_map(*two, ground);
    ZoneDropRules both = rules;
    both.no_drop = [](MapPoint p) { return (p.y < 600 && p.x >= 950) || (p.y > 600 && p.y < 900 && p.x < 1100); };
    const std::optional<ZoneReturn> dd = return_point(*two, ground, MapPoint{ 1500, 499 }, nullptr, &two_legs, &both);
    check(dd && dd->home && dd->drops == 2 && dd->drop && dd->drop->take.y == 499 && dd->drop->take.x < 950 &&
              dd->walk_x == dd->drop->take.x,
          fmt("on the lid over a second platform: the first Ctrl+Down on the lid at %d,%d, not the platform's; the walk "
              "to x %d (%d drop(s))",
              dd && dd->drop ? dd->drop->take.x : 0, dd && dd->drop ? dd->drop->take.y : 0, dd ? dd->walk_x : 0,
              dd ? dd->drops : -1));
}

/// A turn's tap walks the character some 28 px: never off a pit's edge
/// (2026.11.12 review, the owner's Q5) -- the taps not confirmed yet counted.
void test_turn_pit() {
    section("Hunter 2026.11.12 review (the owner's Q5): a turn's tap toward a monster across a pit -- not tapped off "
            "the platform's end, the monster passed over; the taps not confirmed yet counted");
    const std::string dir = way_back_folder();
    if (dir.empty()) return;
    HunterConfig c = owner_config();
    c.spot = false;
    c.drop_down = true;         // the map looked for, trusted by the entry by its gate
    const auto rig_at = [&](JumpRig& r) {
        on_map(r, 94, 200.0f, 799.0f);
        r.run(Hunter::kDropAfterEntryMs + 600);
        return r.st().drop_map.why == MapTrust::Why::Trusted;
    };
    // 15 px from the end, the facing not known: the tap toward the monster
    // 315 px across the pit would walk it off.
    {
        JumpRig r(c, 985, 799, true, dir, "t94_s1");
        const bool trusted = rig_at(r);
        r.w.mobs = { mob_at(1300, 799, 4) };
        const std::vector<std::string> log = info_lines([&] { r.run(1500); });
        check(trusted && r.w.downs_of(VK_RIGHT) == 0 && r.w.downs_of('Z') == 0 && r.w.me_x == 985 &&
                  lines_with(log, "Hunter: not turning right toward", "the floor ends at 1000,799 with nothing of the "
                                                                       "map file under it (a pit)") == 1,
              fmt("at 985,799 the monster across the pit: no tap right (%d), no swing, still on the platform at %d -- "
                  "said once: \"%s\"",
                  r.w.downs_of(VK_RIGHT), r.w.me_x, line_with(log, "not turning right").c_str()));
    }
    // Far from the end: turned as ever.
    {
        JumpRig r(c, 600, 799, true, dir, "t94_s1");
        const bool trusted = rig_at(r);
        r.w.mobs = { mob_at(900, 799, 4) };
        const std::vector<std::string> log = info_lines([&] { r.run(1500); });
        check(trusted && r.w.downs_of(VK_RIGHT) >= 1 && lines_with(log, "not turning") == 0,
              fmt("at 600,799: turned right toward it (%d taps), nothing said", r.w.downs_of(VK_RIGHT)));
    }
    // A turn watched by the packets, its first tap 106 px from the end -- 28
    // px walked, no report of it: the tap again would walk off, refused.
    {
        JumpRig r(c, 894, 799, true, dir, "t94_s1");
        const bool trusted = rig_at(r);
        r.w.facing = -1;
        r.w.facing_ms = r.w.t;
        r.w.mobs = { mob_at(1300, 799, 4) };
        const std::vector<std::string> log = info_lines([&] { r.run(1500); });
        check(trusted && r.w.downs_of(VK_RIGHT) == 1 && r.w.me_x < 1000 && r.w.me_y == 799 &&
                  lines_with(log, "Hunter: not turning right toward") == 1,
              fmt("the first tap from 894 went (%d), walking it to %d; the second, with the first not confirmed, "
                  "refused: \"%s\"",
                  r.w.downs_of(VK_RIGHT), r.w.me_x, line_with(log, "not turning right").c_str()));
    }
}

void test_jump_band() {
    section("Hunter 2026.11.12 (the owner's Q6 and Q7) and 2026.11.12c: a monster on a platform 50 px above, the upper "
            "bar at 10 -- one plain Up in place, the attack in the air; the bar at 60, fought from below");
    const std::string dir = way_back_folder();
    if (dir.empty()) return;
    // t93_s1: you on the ground at 400, the map tab's box round both floors,
    // the monster on the platform at 700.
    {
        HunterConfig c = owner_config();
        c.up_reach = 10;
        c.down_reach = 20;
        JumpRig r(c, 400, 999, true, dir, "t93_s1");
        on_map(r, 93, 2000.0f, 991.0f);
        r.hunter.set_zone(zone_box("t93_s1", 400, 999, 200, 850, 1600, 1050));
        r.run(Hunter::kDropAfterEntryMs + 600);
        const bool used = from_map(r) && r.w.jumps.empty();
        r.w.mobs = { mob_at(700, 949, 4) };
        std::uint64_t key = 0, land = 0;
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty(); }, 8000);
            if (!r.w.jumps.empty()) key = r.w.jumps.front().at;
            r.until([&] { return !r.w.flying; }, 3000);
            land = r.w.t;
            r.run(9000);
        });
        // 2026.11.12c, the owner's "khi ở trên ngưỡng pixel thì chỉ cần nhảy
        // thường (1 lần mũi tên lên)": one plain Up where it stands, the attack
        // in the air -- not walked under the platform to jump onto it. Its
        // bar never goes down here: two Ups from below that did not hurt it,
        // then it is passed over.
        const bool some = !r.w.jumps.empty();
        const bool all_up = std::all_of(r.w.jumps.begin(), r.w.jumps.end(),
                                        [](const JumpWorld::Jump& j) { return j.kind == JumpKind::Up; });
        check(used && some && all_up && r.w.jumps[0].x >= 380 && r.w.jumps[0].x <= 440 && r.w.jumps[0].y == 999 &&
                  r.w.jumps.size() <= static_cast<std::size_t>(Hunter::kBarUpDryPerTarget) &&
                  r.st().bar_air_only >= 1,
              fmt("the monster 50 px above and 300 across, up() 10: %zu plain Up(s) in place, the first from %d,%d -- "
                  "back on its floor each time (%llu)",
                  r.w.jumps.size(), some ? r.w.jumps[0].x : 0, some ? r.w.jumps[0].y : 0,
                  static_cast<unsigned long long>(r.st().bar_air_only)));
        check(r.w.downs_of('Z', key, land) > 0 && r.w.downs_of(VK_DOWN) == 0 && r.w.downs_of(VK_CONTROL) == 0 &&
                  r.w.downs_of(38) == 0,
              fmt("attacked in the air (%d swings between the key and the landing); no Down, no Ctrl, no hop",
                  r.w.downs_of('Z', key, land)));
        check(lines_with(log, "Hunter: Up (one plain jump) toward the monster: 50 px above, past the upper bar's 10 "
                              "(air band to 143)") >= 1 &&
                  lines_with(log, "in the air only (no hit; dry 1 of 2)") == 1 &&
                  lines_with(log, "passed over -- 2 Ups from below did not hurt it") == 1 &&
                  lines_with(log, "Hunter: jump up (Up) toward") == 0,
              "said: \"" + line_with(log, "Hunter: Up (one plain jump)") + "\" -- \"" + line_with(log, "in the air only") +
                  "\" -- \"" + line_with(log, "passed over") + "\"");
    }
    // The upper bar at 60 (height_tolerance's): the monster between the bars
    // -- fought from the ground where you stand, no jump.
    {
        JumpRig r(owner_config(), 400, 999, true, dir, "t93_s1");
        on_map(r, 93, 2000.0f, 991.0f);
        r.hunter.set_zone(zone_box("t93_s1", 400, 999, 200, 850, 1600, 1050));
        r.run(Hunter::kDropAfterEntryMs + 600);
        r.w.mobs = { mob_at(700, 949, 4) };
        r.run(4000);
        check(r.w.jumps.empty() && r.w.me_y == 999 && r.w.downs_of('Z') > 0,
              fmt("the upper bar at 60: no jump (%zu), fought from the ground (%d swings)", r.w.jumps.size(),
                  r.w.downs_of('Z')));
    }
}

/// 2026.11.12c: the way back from t23_s14's bottom, made up (t87_s1) -- what
/// the owner did by hand at 23:47 -- walked at g 2400 and g 1905; the
/// wall-foot Ctrl+Up onto t49_s5's step, made up (t88_s1); and a jump judged
/// by its take-off's speed, the client of 2026-11-12 sending no state 1, 2 or 6.
void test_way_back_12c() {
    section("Hunter 2026.11.12c: back from t23_s14's bottom by Ctrl+Up then Up,Up (made up, g 2400 and 1905); the "
            "wall-foot Ctrl+Up onto a step; a jump judged by its speed");
    const std::string dir = way_back_folder();
    check(!dir.empty(), "the made-up maps written to this run's temp folder");
    if (dir.empty()) return;
    for (const double g : { 2400.0, 1905.0 }) {
        JumpRig r(owner_config(), 1593, 1262, true, dir, "t87_s1");
        on_map(r, 87, 1593.0f, 1262.0f);
        r.w.gravity = g;
        r.hunter.set_zone(zone_box("t87_s1", 1593, 1262, 740, 1162, 2858, 1586));
        r.run(Hunter::kDropAfterEntryMs + 600);
        const bool used = from_map(r) && r.st().spot_outside_px == 0;
        r.w.place(1757, 1810);
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && r.st().spot_home && !r.w.walking() && !r.w.flying; }, 40'000);
            r.run(1500);
        });
        const bool first_high = !r.w.jumps.empty() && r.w.jumps[0].kind == JumpKind::CtrlUp;
        check(used && first_high && r.w.jumps.size() <= 2 && r.w.me_y == 1262 && r.st().spot_home &&
                  r.st().jumps_worked == r.w.jumps.size(),
              fmt("g %.0f: from 1757,1810 (224 px under the area) home at %d,%d by %zu jump(s), the first Ctrl+Up "
                  "from %d,%d -- all worked",
                  g, r.w.me_x, r.w.me_y, r.w.jumps.size(), first_high ? r.w.jumps[0].x : 0,
                  first_high ? r.w.jumps[0].y : 0));
        check(lines_with(log, "came out as") == 0 && lines_with(log, "OFF until") == 0 &&
                  lines_with(log, "jump up worked") == static_cast<int>(r.w.jumps.size()) &&
                  r.w.downs_of(VK_DOWN) == 0 && r.w.arrows_in_air() == 0,
              fmt("g %.0f: no kind missed, nothing latched, no Down, no arrow in the air: \"%s\"", g,
                  line_with(log, "jump up worked").c_str()));
    }
    // The wall-foot Ctrl+Up: the home on the ledge, the character on the
    // ground beside it -- walked into the step, never a "pit", one Ctrl+Up
    // in place at its foot, onto the step.
    {
        JumpRig r(owner_config(), 1800, 999, true, dir, "t88_s1");
        on_map(r, 88, 2000.0f, 999.0f);
        r.w.answer = Answer::Ledge;
        r.w.force_x = 1299;
        r.w.force_y = 935;
        r.hunter.set_zone(zone_box("t88_s1", 1100, 935, 400, 650, 2400, 1050));
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return r.st().spot_home && !r.w.walking() && !r.w.flying; }, 25'000);
            r.run(1000);
        });
        const bool one = r.w.jumps.size() == 1;
        check(one && r.w.jumps[0].kind == JumpKind::CtrlUp && r.w.jumps[0].x >= 1300 && r.w.jumps[0].x <= 1304 &&
                  r.w.me_y == 935 && r.st().spot_home,
              fmt("one Ctrl+Up at the step's foot, x %d, onto the ledge; home at %d,%d", one ? r.w.jumps[0].x : 0,
                  r.w.me_x, r.w.me_y));
        check(lines_with(log, "(a pit") == 0 && lines_with(log, "at the foot of a step of 64 px at x 1300") == 1 &&
                  lines_with(log, "jump up worked") == 1 && r.w.downs_of(VK_DOWN) == 0,
              "said: \"" + line_with(log, "at the foot of a step") + "\" -- no pit, no Down");
    }
    if (game_data().empty()) {
        skip("no copies of the game's map files here: a jump judged by its speed on t52_s2\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    const int ground = stand_y(p, 880, 634);
    // Every 0x002A in the air of state 7, at the key's own speed: worked.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::OddState;
        r.hunter.set_zone(owner_zone());
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return r.st().spot_home && !r.w.walking() && !r.w.flying && r.w.me_y < 300; }, 25'000);
            r.run(1000);
        });
        check(r.w.jumps.size() == 1 && r.st().jumps_worked == 1 && lines_with(log, "came out as") == 0,
              "Ctrl+Up reported as state 7 at -36: worked, no kind missed -- \"" + line_with(log, "jump up worked") +
                  "\"");
    }
    // Off at a plain jump's -20, but down on the planned floor: worked, taken
    // by its landing -- never a kind missed.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::PlainOnPlanned;
        r.w.force_x = 880;
        r.w.force_y = 274;
        r.hunter.set_zone(owner_zone());
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && !r.w.flying; }, 25'000);
            r.run(1000);
        });
        // 360 px up is more than a plain Up reaches: a high jump by its landing.
        check(r.w.jumps.size() == 1 && r.st().jumps_worked == 1 && lines_with(log, "came out as") == 0,
              "Ctrl+Up off at -20 onto the planned floor: \"" + line_with(log, "jump up worked") + "\"");
    }
}

/// The review of 2026.11.12c, the bar's ↑: at a step's foot, climbing it, it
/// lands on the step's lip; onto the monster's floor; a hit that is not passed
/// over and the minute's cap; outside the area and ones that did nothing --
/// since 2026.11.24 never off or paused; the dash's keys held in its flight.
void test_review_bar() {
    section("Hunter 2026.11.12c review, 2026.11.24: the bar's Up -- climbing a step onto its lip, onto the monster's "
            "floor, a hit and the minute's cap, outside the area (the take-off skipped, never off), ones that did "
            "nothing (never a pause), the dash held in its flight");
    const std::string dir = way_back_folder();
    check(!dir.empty(), "the made-up maps written to this run's temp folder");
    if (dir.empty()) return;
    HunterConfig bars = owner_config();
    bars.up_reach = 10;
    bars.down_reach = 20;

    // A plain Up toward a monster on t49_s5's step, made up (t88_s1), from
    // the step's foot: the step climbed, the character reported a few px past
    // its face on its top -- its lip. Onto the monster's floor; never "on no
    // floor" with the file distrusted and every jump up off. A climb with no
    // take-off reported the same, never a press that did nothing.
    for (const Answer a : { Answer::Ledge, Answer::Climb }) {
        JumpRig r(bars, 1302, 999, true, dir, "t88_s1");
        on_map(r, 88, 2000.0f, 999.0f);
        r.hunter.set_zone(zone_box("t88_s1", 1302, 999, 400, 650, 2400, 1050));
        r.run(Hunter::kDropAfterEntryMs + 600);
        r.w.answer = a;
        r.w.force_x = 1302;
        r.w.force_y = 935;
        r.w.mobs = { mob_at(1290, 935, 4) };
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && !r.w.flying && r.w.climb_at == 0; }, 8000);
            r.run(2500);
        });
        const bool one = r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::Up;
        check(one && r.w.jumps[0].x >= 1300 && r.w.jumps[0].x <= 1304 && r.w.jumps[0].y == 999 &&
                  r.st().bar_onto == 1 && r.st().drop_map.why == MapTrust::Why::Trusted &&
                  !r.book.distrusted("t88_s1") && lines_with(log, "on no floor") == 0 &&
                  lines_with(log, "did nothing") == 0 &&
                  lines_with(log, "Hunter: Up toward the monster: onto its floor at 1302,935") == 1 &&
                  (a != Answer::Climb || lines_with(log, "no take-off reported: a climb") == 1),
              fmt("a plain Up at the step's foot (x %d), %s at its lip 1302,935: onto the monster's floor (%llu), the "
                  "file kept, nothing off -- \"%s\"",
                  one ? r.w.jumps[0].x : 0,
                  a == Answer::Climb ? "a climb with no take-off reported" : "landed",
                  static_cast<unsigned long long>(r.st().bar_onto), line_with(log, "Hunter: Up toward").c_str()));
    }

    // t93_s1 (a platform at 950, x 600..1500, over solid ground at 1000): you
    // under the platform, the monster on it -- one Up in place, onto its floor.
    const auto t93 = [&](JumpRig& r, int home, int right) {
        on_map(r, 93, 2000.0f, 991.0f);
        r.hunter.set_zone(zone_box("t93_s1", home, 999, 200, 850, right, 1050));
        r.run(Hunter::kDropAfterEntryMs + 600);
    };
    {
        JumpRig r(bars, 700, 999, true, dir, "t93_s1");
        t93(r, 700, 1600);
        r.w.mobs = { mob_at(720, 949, 4) };
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && !r.w.flying; }, 8000);
            r.run(1500);
        });
        const bool one = r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::Up;
        const std::string onto = one ? fmt("Hunter: Up toward the monster: onto its floor at %d,949", r.w.jumps[0].x)
                                     : std::string("?");
        check(one && r.w.jumps[0].x >= 680 && r.w.jumps[0].x <= 740 && r.w.jumps[0].y == 999 && r.w.me_y == 949 &&
                  r.st().bar_onto == 1 && r.st().bar_air_only == 0 && lines_with(log, onto.c_str()) == 1,
              fmt("under the platform, the monster on it: one plain Up in place, onto its floor -- now at %d,%d: \"%s\"",
                  r.w.me_x, r.w.me_y, line_with(log, "Hunter: Up toward").c_str()));
    }
    // Hurt in the air at every landing: never passed over, Up after Up --
    // kBarUpsPerMinute in the minute, no more.
    {
        JumpRig r(bars, 400, 999, true, dir, "t93_s1");
        t93(r, 400, 1600);
        r.w.mobs = { mob_at(700, 949, 4) };
        std::size_t seen = 0;
        const auto hurt = [&] {
            if (!r.w.flying && r.w.jumps.size() > seen && !r.w.mobs.empty()) {
                seen = r.w.jumps.size();
                r.w.mobs[0].hp -= 0.04f;
            }
        };
        const std::size_t cap = static_cast<std::size_t>(Hunter::kBarUpsPerMinute);
        const std::uint64_t t0 = r.w.t;
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] {
                hurt();
                return r.w.jumps.size() >= cap && !r.w.flying;
            }, 45'000);
            r.run(1000, hurt);
        });
        const std::uint64_t took = r.w.t - t0;
        r.run(15'000, hurt);
        const bool all_up = std::all_of(r.w.jumps.begin(), r.w.jumps.end(),
                                        [](const JumpWorld::Jump& j) { return j.kind == JumpKind::Up; });
        check(all_up && r.w.jumps.size() == cap && r.st().bar_ups == cap &&
                  lines_with(log, "in the air only (hit it; dry 0 of 2)") == static_cast<int>(cap) &&
                  lines_with(log, "passed over") == 0,
              fmt("its HP lower after each landing: %zu plain Ups in %.0f s, each a hit, never passed over -- then no "
                  "more, %d a minute at most: \"%s\"",
                  r.w.jumps.size(), static_cast<double>(took) / 1000.0, Hunter::kBarUpsPerMinute,
                  line_with(log, "in the air only").c_str()));
    }
    // Down outside the area: the walk back takes over, the take-off skipped
    // kLearnedForgetMs -- then forgotten -- and the Up for monsters above
    // stays on (2026.11.24; off for the run until then).
    {
        JumpRig r(bars, 400, 999, true, dir, "t93_s1");
        t93(r, 400, 1600);
        r.w.answers = { Answer::Outside };
        r.w.force_x = 1700;
        r.w.force_y = 999;
        r.w.mobs = { mob_at(700, 949, 4) };
        std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && !r.w.flying; }, 8000);
            r.run(4000);
        });
        const std::vector<GameMapBook::NoJump> nj = r.book.no_jumps("t93_s1");
        const bool learned = !r.w.jumps.empty() && nj.size() == 1 && nj[0].take.x == r.w.jumps[0].x;
        check(r.st().bar_ups == 1 && learned &&
                  lines_with(log, "OUTSIDE the hunting area -- that take-off skipped 30 s, Up for monsters above goes "
                                  "on; the walk back takes over") == 1 &&
                  lines_with(log, "OFF until") == 0,
              fmt("landed outside the area: \"%s\" -- one Up (%llu), its take-off learned (%zu)",
                  line_with(log, "OUTSIDE the hunting area").c_str(), static_cast<unsigned long long>(r.st().bar_ups),
                  nj.size()));
        const std::vector<std::string> later = info_lines([&] { r.run(Hunter::kLearnedForgetMs); });
        check(r.book.no_jumps("t93_s1").empty() && lines_with(later, "where the jump did not work is tried again") == 1,
              "its take-off forgotten " + std::to_string(Hunter::kLearnedForgetMs / 1000) + " s on: \"" +
                  line_with(later, "is tried again") + "\"");
    }
    // Ups that did nothing, in a row: never a pause (2026.11.24; 30 s after
    // three, then 60 s, from 2026.11.15; off for the run before) -- the next
    // Up kBarUpGapMs or more after each.
    {
        JumpRig r(bars, 400, 999, true, dir, "t93_s1");
        t93(r, 400, 1600);
        r.w.answer = Answer::Nothing;
        r.w.mobs = { mob_at(700, 949, 4) };
        const std::vector<std::string> log = info_lines([&] { r.run(20'000); });
        bool spaced = r.w.jumps.size() >= 2;
        for (std::size_t i = 1; i < r.w.jumps.size(); ++i)
            spaced = spaced && r.w.jumps[i].at - r.w.jumps[i - 1].at >= Hunter::kBarUpGapMs;
        const int said = lines_with(log, "did nothing in 900 ms");
        check(r.st().bar_ups >= 5 && spaced && lines_with(log, "paused for") == 0 &&
                  lines_with(log, "on again after") == 0 && said + 1 >= static_cast<int>(r.st().bar_ups) &&
                  said <= static_cast<int>(r.st().bar_ups),
              fmt("%llu Ups with no take-off in 20 s (%d said), each %llu ms or more after the last -- never a pause: "
                  "\"%s\"",
                  static_cast<unsigned long long>(r.st().bar_ups), said,
                  static_cast<unsigned long long>(Hunter::kBarUpGapMs), line_with(log, "Up toward").c_str()));
    }
    // The dash's keys in the Up's flight: judged from the take-off -- 550,
    // the area's right edge at 700, a dash of 320 toward the monster lands
    // outside it -- held at every pass in the air, as on the ground.
    {
        JumpRig r(bars, 550, 999, true, dir, "t93_s1");
        t93(r, 550, 700);
        // A dash learned: none is held while none is (2026.11.17).
        r.hunter.set_dash('C', 320);
        r.w.mobs = { mob_at(650, 949, 4) };
        r.until([&] { return !r.w.jumps.empty(); }, 8000);
        int air = 0, held = 0;
        r.until([&] {
            if (r.w.flying) {
                ++air;
                if (r.hunter.dash_edge()) ++held;
            }
            return !r.w.flying;
        }, 3000);
        check(r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::Up && air >= 3 && held == air,
              fmt("in the Up's flight from 550 (the area's edge at 700, the dash 320 px): the dash's keys held at %d of "
                  "%d passes in the air",
                  held, air));
    }
}

/// The review of 2026.11.12c, the drop within one ↑ under the monster: only
/// where that ↑ comes down on no other floor; the ↑ not held after it; the
/// drop's level rule.
void test_review_drop() {
    section("Hunter 2026.11.12c review: a drop within one Up under the monster -- only where that Up comes down on no "
            "other floor, the Up not held after it; the drop's level rule");
    const std::string dir = way_back_folder();
    check(!dir.empty(), "the made-up maps written to this run's temp folder");
    if (dir.empty()) return;
    GameMapBook probe(dir);
    const std::shared_ptr<const GameMapBook::Page> few = probe.get("t86_s1");
    check(few != nullptr, "t86_s1 read");
    if (!few) return;
    HunterConfig c = owner_config();
    c.up_reach = 10;
    c.down_reach = 20;
    c.drop_down = true;
    c.spot = false;
    const int one_up = c.up() + jump_peak(JumpKind::Up).lo;
    const auto facts = [&](int x0, int y0, int tx, int ty, bool bar_ok) {
        Hunter::DropFacts f;
        f.press = true;
        f.now = 100'000;
        f.wired = true;
        f.packet_target = true;
        f.dx = tx - x0;
        f.dy = ty - y0;
        f.target_wx = static_cast<float>(tx);
        f.target_wy = static_cast<float>(ty);
        f.fix.state = WorldFix::State::Known;
        f.fix.x = f.fix.rx = static_cast<float>(x0);
        f.fix.y = f.fix.ry = static_cast<float>(y0);
        f.fix.report_ms = 99'960;
        f.entry_ms = 10'000;
        f.settle_from_ms = 99'940;
        f.page = few;
        f.bar_ok = bar_ok;
        f.below_one_up_px = one_up;
        return f;
    };
    // t23_s14's 1700: on 1390, the monster on the small 1425 at 1745 -- the
    // fall lands on 1465, 40 px under it, and an Up from there comes down on
    // 1390 again: refused, past the monster. Over the small one, onto it.
    const Hunter::DropCall pp = Hunter::drop_rule(c, facts(1700, 1389, 1745, 1424, true));
    const Hunter::DropCall over = Hunter::drop_rule(c, facts(1745, 1389, 1745, 1424, true));
    check(pp.why == Hunter::DropWhy::PastMonster && pp.land_y == 1465 && over.ok() && over.direct && !over.one_up &&
              over.land_y == 1425,
          fmt("from 1700,1389 the fall lands on %d, an Up from there comes down on 1390: %s; from over it at 1745: %s "
              "onto %d",
              pp.land_y, Hunter::drop_why_name(pp.why), Hunter::drop_why_name(over.why), over.land_y));
    // On the ledge at 1250: the fall lands on 1465, 40 px under the monster's
    // small 1425 at 2152, and an Up from there reaches nothing but it: a drop
    // within one Up -- with the bar's Up able to go only.
    const Hunter::DropCall up = Hunter::drop_rule(c, facts(2130, 1249, 2152, 1424, true));
    const Hunter::DropCall no_bar = Hunter::drop_rule(c, facts(2130, 1249, 2152, 1424, false));
    check(up.ok() && up.one_up && !up.direct && up.land_y == 1465 && no_bar.why == Hunter::DropWhy::PastMonster,
          fmt("from 2130,1249 onto 1465, 40 px under it, an Up from there onto nothing else: %s (within one Up: %s); "
              "the bar's Up unable to go: %s",
              Hunter::drop_why_name(up.why), up.one_up ? "yes" : "no", Hunter::drop_why_name(no_bar.why)));

    // In the pretend game: the drop, then the Up at once -- not the 8 s a
    // drop's landing holds an Up toward a monster.
    {
        HunterConfig h = c;
        h.spot = true;
        JumpRig r(h, 2480, 1249, true, dir, "t86_s1");
        on_map(r, 86, 2480.0f, 1249.0f);
        r.w.drops_fall = true;
        r.hunter.set_zone(zone_box("t86_s1", 2130, 1464, 1950, 1200, 2550, 1500));
        r.run(Hunter::kDropAfterEntryMs + 600);
        r.w.place(2130, 1249);
        r.run(600);
        r.w.mobs = { mob_at(2152, 1424, 4) };
        std::uint64_t landed = 0, key = 0;
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.chords.empty() && !r.w.flying; }, 10'000);
            landed = r.w.t;
            r.until([&] { return !r.w.jumps.empty(); }, 10'000);
            key = r.w.jumps.empty() ? 0 : r.w.jumps[0].at;
            r.run(1500);
        });
        const bool upped = !r.w.jumps.empty() && r.w.jumps[0].kind == JumpKind::Up && r.w.jumps[0].y == 1464;
        check(r.w.chords.size() == 1 && upped && key > landed && key - landed < Hunter::kDropStayMs &&
                  lines_with(log, "Hunter: drop worked") == 1 && r.w.downs_of(VK_DOWN) == 1,
              fmt("Ctrl+Down from 2130,1249 onto 1465, then a plain Up from %d,%d %.1f s after the landing (a drop's "
                  "hold %llu s not in its way)",
                  upped ? r.w.jumps[0].x : 0, upped ? r.w.jumps[0].y : 0,
                  static_cast<double>(key > landed ? key - landed : 0) / 1000.0,
                  static_cast<unsigned long long>(Hunter::kDropStayMs / 1000)));
    }

    // The drop's level rule (kBarNearestFirst): one on the level in reach --
    // further across than the target -- goes first; one out of reach and
    // further does not; one nearer across does.
    Nameplate me = mob_at(1000, 999, -1);
    me.is_player = me.is_self = true;
    std::vector<Nameplate> mobs = { mob_at(1030, 1139, 4), mob_at(1400, 999, 5) };
    const bool in_reach_first = Hunter::level_before_drop(c, mobs, me, {}, 30);
    mobs[1] = mob_at(1600, 999, 5);
    const bool far_not = !Hunter::level_before_drop(c, mobs, me, {}, 30);
    const bool nearer_first = Hunter::level_before_drop(c, mobs, me, {}, 700);
    check(in_reach_first && far_not && nearer_first,
          "the target below 30 px across: a level one 400 px across (in reach) goes first; one 600 px across (out of "
          "reach, further) does not; with the target 700 px across, that one (nearer) does");
}

/// The review of 2026.11.12c, the way back: stranded on a file not trusted
/// yet, trust steps then the way; stranded with none, asked again every
/// kStrandedReaskMs (10 s until 2026.11.15, 3 s since; on a standing point --
/// the 8 s stand of 00:05:56 had none, see test_zone_walk_back) and "a way
/// back shows now"; a wall-foot Ctrl+Up landing elsewhere.
void test_review_way() {
    section("Hunter 2026.11.12c review: the way back -- trust steps on a file not trusted yet, the stranded stand "
            "asked again every kStrandedReaskMs (3 s since 2026.11.15), a wall-foot Ctrl+Up landing elsewhere");
    const std::string dir = way_back_folder();
    check(!dir.empty(), "the made-up maps written to this run's temp folder");
    if (dir.empty()) return;
    // t23_s14's bottom, made up (t87_s1), the map entered there -- its spawn
    // by no gate: the file not trusted, no jump up may go, no way back.
    // Steps of 80 px along the bottom floor, never off its ends (x
    // 1500..1950), until MapTrust has its three standing points; then the
    // way, Ctrl+Up first, home.
    {
        JumpRig r(owner_config(), 1757, 1810, true, dir, "t87_s1");
        on_map(r, 87, 1757.0f, 1810.0f);
        r.hunter.set_zone(zone_box("t87_s1", 1593, 1262, 740, 1162, 2858, 1586));
        int lo = INT_MAX, hi = 0;
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] {
                if (r.w.jumps.empty()) {
                    lo = std::min(lo, r.w.me_x);
                    hi = std::max(hi, r.w.me_x);
                }
                return r.st().spot_home && !r.w.walking() && !r.w.flying;
            }, 60'000);
            r.run(1000);
        });
        const std::uint64_t steps = r.st().trust_steps;
        const bool first_high = !r.w.jumps.empty() && r.w.jumps[0].kind == JumpKind::CtrlUp;
        check(steps >= 1 && steps <= static_cast<std::uint64_t>(Hunter::kTrustStepsMax) &&
                  lines_with(log, "its file not trusted yet (standing points") >= 1 &&
                  lines_with(log, "a step of 80 px left along its floor for one") >= 1 &&
                  r.st().drop_map.why == MapTrust::Why::Trusted && lo >= 1500 && hi <= 1950 && first_high &&
                  r.st().spot_home && r.w.me_y == 1262 && r.w.downs_of(VK_DOWN) == 0,
              fmt("from 1757,1810, its file not trusted: %llu step(s) along the bottom (x %d..%d before a jump), "
                  "trusted, then Ctrl+Up first and home at %d,%d -- \"%s\"",
                  static_cast<unsigned long long>(steps), lo, hi, r.w.me_x, r.w.me_y,
                  line_with(log, "a step of 80 px").c_str()));
    }
    // t92_s1's lid over the area: only Ctrl+Down gets back, and Nhảy xuống is
    // off. Stranded on a standing point, asked again every kStrandedReaskMs
    // (3 s since 2026.11.15) with nothing pressed; drops switched on, "a way
    // back shows now", one Ctrl+Down, home.
    {
        HunterConfig c = owner_config();
        JumpRig r(c, 2000, 999, true, dir, "t92_s1");
        on_map(r, 92, 2000.0f, 999.0f);
        r.hunter.set_zone(zone_box("t92_s1", 1200, 999, 1000, 950, 1400, 1050));
        r.run(Hunter::kDropAfterEntryMs + 600);
        r.w.place(1200, 499);
        const std::size_t keys0 = r.w.keys.size();
        std::vector<std::uint64_t> asked_at;
        std::uint64_t last_asks = r.st().way_asks;
        r.run(25'000, [&] {
            const std::uint64_t n = r.st().way_asks;
            if (n != last_asks) {
                last_asks = n;
                asked_at.push_back(r.w.t);
            }
        });
        const std::uint64_t asks = r.st().way_asks;
        bool spaced = asked_at.size() >= 2;
        for (std::size_t i = 1; i < asked_at.size(); ++i) {
            const std::uint64_t gap = asked_at[i] - asked_at[i - 1];
            spaced = spaced && gap >= Hunter::kStrandedReaskMs && gap <= Hunter::kStrandedReaskMs + 300;
        }
        check(Hunter::kStrandedReaskMs == 3000 && spaced && asks >= 6 && asks <= 10,
              fmt("stranded with no way: asked again every %llu ms (kStrandedReaskMs, not 10 s), %llu times in 25 s",
                  static_cast<unsigned long long>(Hunter::kStrandedReaskMs), static_cast<unsigned long long>(asks)));
        const bool stranded = r.st().spot_stranded;
        const bool still = r.w.keys.size() == keys0 && r.w.me_x == 1200 && r.w.me_y == 499;
        c.drop_down = true;
        r.hunter.set_config(c);
        r.w.drops_fall = true;
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return r.st().spot_home && !r.w.walking() && !r.w.flying; }, 20'000);
            r.run(500);
        });
        check(stranded && still && asks >= 2 &&
                  lines_with(log, "Hunter: a way back shows now from 1200,499 after",
                             "then a drop (Ctrl+Down) onto floor 1000") == 1 &&
                  r.w.chords.size() == 1 && r.st().spot_home && r.w.me_y == 999,
              fmt("on the lid, drops off: stranded, nothing pressed, asked again %llu times in 25 s; drops on: \"%s\" "
                  "-- one Ctrl+Down, home at %d,%d",
                  static_cast<unsigned long long>(asks), line_with(log, "a way back shows now").c_str(), r.w.me_x,
                  r.w.me_y));
    }
    // The wall-foot Ctrl+Up on t49_s5's step, made up (t88_s1), as the game
    // would have it: straight up, past the step's 64 px onto the platform at
    // 584 over it -- outside the area. That take-off skipped kLearnedForgetMs,
    // the wall-foot jumps and the others on (2026.11.24; wall-foot jumps off
    // for the run until then); the way back walks off the platform's left end
    // onto the ledge, home.
    {
        JumpRig r(owner_config(), 1800, 999, true, dir, "t88_s1");
        on_map(r, 88, 2000.0f, 999.0f);
        r.hunter.set_zone(zone_box("t88_s1", 1100, 935, 400, 650, 2400, 1050));
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && !r.w.flying; }, 25'000);
            r.until([&] { return r.st().spot_home && !r.w.walking() && !r.w.flying; }, 30'000);
            r.run(1000);
        });
        const bool first = !r.w.jumps.empty() && r.w.jumps[0].kind == JumpKind::CtrlUp;
        const int walls = static_cast<int>(std::count_if(r.w.jumps.begin(), r.w.jumps.end(), [](const JumpWorld::Jump& j) {
            return j.kind == JumpKind::CtrlUp && j.x >= 1300 && j.x <= 1304 && j.y == 999;
        }));
        const std::vector<GameMapBook::NoJump> nj = r.book.no_jumps("t88_s1");
        check(first && walls == 1 && !nj.empty() && nj[0].take.x >= 1300 && nj[0].take.x <= 1304 &&
                  lines_with(log, "that take-off skipped 30 s, other jumps stay on") == 1 &&
                  lines_with(log, "OFF until") == 0 && r.st().spot_home && r.w.me_y == 935,
              fmt("one wall-foot Ctrl+Up, onto the platform at 584 outside the area: \"%s\" -- its take-off learned, "
                  "home at %d,%d",
                  line_with(log, "the wall-foot Ctrl+Up").c_str(), r.w.me_x, r.w.me_y));
    }
}

/// 2026.11.24 (the owner: "Kiểm tra các phần khác xem có phần nào không làm
/// được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác"): stranded on
/// t23_s14's bottom (t87_s1, made up), its file not trusted, three trust steps
/// and still no trust -- a monster standing off the file's floors sets
/// MapTrust's points back at every look -- and the steps come again
/// kTrustStepsAgainMs on, a new round, never given up (until then none before
/// the next map entry). The monster gone, the next round's points trust the
/// file and the way back takes the character home.
void test_trust_steps_again() {
    section("Hunter 2026.11.24: three trust steps and the file still not trusted (a monster standing off its "
            "floors) -- the steps again a few seconds on (kTrustStepsAgainMs), a new round; the monster gone, trusted, home by Ctrl+Up");
    const std::string dir = way_back_folder();
    check(!dir.empty(), "the made-up maps written to this run's temp folder");
    if (dir.empty()) return;
    JumpRig r(owner_config(), 1757, 1810, true, dir, "t87_s1");
    on_map(r, 87, 1757.0f, 1810.0f);
    r.hunter.set_zone(zone_box("t87_s1", 1593, 1262, 740, 1162, 2858, 1586));
    // 164 px over the ground at x 2400, on no floor, outside the area: never
    // fought, and every look it stands there the points so far count no more.
    r.w.mobs = { mob_at(2400, 1700, 7) };
    int lo = INT_MAX, hi = 0;
    std::uint64_t third_at = 0, fourth_at = 0;
    bool trusted_early = false;
    const std::vector<std::string> log = info_lines([&] {
        const auto watch = [&] {
            if (r.w.jumps.empty()) {
                lo = std::min(lo, r.w.me_x);
                hi = std::max(hi, r.w.me_x);
            }
            if (r.st().drop_map.why == MapTrust::Why::Trusted) trusted_early = true;
        };
        r.until([&] {
            watch();
            return r.st().trust_steps >= static_cast<std::uint64_t>(Hunter::kTrustStepsMax);
        }, 30'000);
        third_at = r.w.t;
        // Gone as the third step begins: the two points before it were set
        // back, its own counts -- one of three after the round.
        r.w.mobs.clear();
        r.until([&] {
            watch();
            return r.st().trust_steps > static_cast<std::uint64_t>(Hunter::kTrustStepsMax);
        }, Hunter::kTrustStepsAgainMs + 15'000);
        fourth_at = r.w.t;
    });
    const std::uint64_t gap = fourth_at > third_at ? fourth_at - third_at : 0;
    check(third_at != 0 && r.st().trust_steps == static_cast<std::uint64_t>(Hunter::kTrustStepsMax) + 1 &&
              gap >= Hunter::kTrustStepsAgainMs && gap <= Hunter::kTrustStepsAgainMs + 8000 && !trusted_early &&
              lines_with(log, "Hunter: stranded on t87_s1, its file still not trusted after 3 steps -- the steps "
                              "again in 5 s") == 1 &&
              lo >= 1500 && hi <= 1950 && r.w.jumps.empty(),
          fmt("three steps along the bottom (x %d..%d), no trust (a monster standing off its floors): the fourth step "
              "%llu ms after the third began -- the round's wait of %llu s, never given up -- \"%s\"",
              lo, hi, static_cast<unsigned long long>(gap),
              static_cast<unsigned long long>(Hunter::kTrustStepsAgainMs / 1000),
              line_with(log, "its file still not trusted after").c_str()));
    // The new round's steps earn MapTrust's other points (its first back at
    // the entry's spawn, which is none), the file is trusted, and the way
    // back takes the character home.
    const std::vector<std::string> home = info_lines([&] {
        r.until([&] { return r.st().spot_home && !r.w.walking() && !r.w.flying; }, 40'000);
        r.run(1000);
    });
    check(r.st().drop_map.why == MapTrust::Why::Trusted && r.st().spot_home && r.w.me_y == 1262 &&
              !r.w.jumps.empty() && r.w.downs_of(VK_DOWN) == 0 &&
              r.st().trust_steps <= 2ull * Hunter::kTrustStepsMax &&
              lines_with(home, "Hunter: t87_s1 trusted by 3 standing points on its floors") == 1,
          fmt("then, the monster gone, trusted after %llu step(s) in all, home at %d,%d by %zu jump(s) -- \"%s\"",
              static_cast<unsigned long long>(r.st().trust_steps), r.w.me_x, r.w.me_y, r.w.jumps.size(),
              line_with(home, "trusted by").c_str()));
}

/// 2026.11.24, the way back's jumps up that fall back: each a strike of the
/// walk back, three a round, and the next round spot_retry_ms on with three
/// tries again (track_give_up) -- until then one try after four
/// spot_retry_ms, 60 s at least. t96_s1: the home on the platform at 720, the
/// character fallen to the ground at 1500; every Ctrl+Up falls back.
void test_way_back_jump_rounds() {
    section("Hunter 2026.11.24: the way back's Ctrl+Up falling back -- three strikes a round, the next round "
            "spot_retry_ms on with three tries again, never given up");
    const std::string dir = way_back_folder();
    check(!dir.empty(), "the made-up maps written to this run's temp folder");
    if (dir.empty()) return;
    HunterConfig c = owner_config();
    c.spot_retry_ms = 5000;
    JumpRig r(c, 750, 719, true, dir, "t96_s1");
    on_map(r, 96, 2000.0f, 991.0f);
    r.hunter.set_zone(zone_box("t96_s1", 750, 719, 550, 640, 950, 790));
    r.run(Hunter::kDropAfterEntryMs + 600);
    r.w.place(1500, 999);
    r.w.answer = Answer::FallBack;
    std::uint64_t gave_at = 0, round_at = 0;
    std::size_t jumps_round = 0;
    const std::vector<std::string> log = info_lines([&] {
        r.until([&] { return r.st().spot_why == Hunter::SpotWhy::GaveUp; }, 120'000);
        gave_at = r.w.t;
        jumps_round = r.w.jumps.size();
        // The new round: GaveUp no longer -- its jumps from take-offs not
        // learned in the last kLearnedForgetMs (2026.11.24), so they may come
        // later than the round.
        r.until([&] { return r.st().spot_why != Hunter::SpotWhy::GaveUp; }, 60'000);
        round_at = r.w.t;
        r.until([&] { return r.st().return_give_ups >= 2ull * Hunter::kSpotMaxTries; }, 150'000);
        r.run(500);
    });
    const std::uint64_t gap = round_at > gave_at ? round_at - gave_at : 0;
    check(gave_at != 0 && jumps_round == static_cast<std::size_t>(Hunter::kSpotMaxTries) &&
              gap >= c.spot_retry_ms && gap <= c.spot_retry_ms + 1000 &&
              r.st().return_give_ups == 2ull * Hunter::kSpotMaxTries &&
              r.w.jumps.size() == 2ull * Hunter::kSpotMaxTries &&
              lines_with(log, "Hunter: the way back's jumps up got nowhere 3 times in a row -- this round of tries "
                              "over, the next after 5 s") == 2 &&
              lines_with(log, "Hunter: trying the walk back again -- the wait is over", "a new round of 3 tries") == 1 &&
              lines_with(log, "long wait") == 0 && r.w.downs_of(VK_DOWN) == 0,
          fmt("three Ctrl+Ups fell back, a round (%zu jumps); the next round %llu ms after it (spot_retry_ms %u), "
              "three more jumps, given up on again (%llu give-ups, %zu jumps) -- \"%s\"",
              jumps_round, static_cast<unsigned long long>(gap), c.spot_retry_ms,
              static_cast<unsigned long long>(r.st().return_give_ups), r.w.jumps.size(),
              line_with(log, "jumps up got nowhere").c_str()));
}

/// The review of 2026.11.12c, the verdict's order: Up,Up with no second push
/// to be seen, down on the planned floor -- worked, its kind taken by its
/// landing; down on a floor above the plan within the key's reach -- a move.
void test_review_verdict() {
    section("Hunter 2026.11.12c review: the verdict's order -- Up,Up with no second push seen, on the planned floor: "
            "worked; on a floor above the plan within reach: a move");
    {
        const std::string dir = staircase_folder();
        check(!dir.empty(), "the made-up t98_s1 written to this run's temp folder");
        if (!dir.empty()) {
            JumpRig r(owner_config(), 1000, 999, true, dir, "t98_s1");
            r.w.map = 98;
            r.w.area = 1;
            r.w.entry_x = 2000.0f;
            r.w.entry_y = 999.0f;
            r.hunter.set_zone(zone_box("t98_s1", 1000, 819, 200, 400, 2400, 1050));
            r.w.answer = Answer::PlainOnPlanned;
            r.w.force_x = 1000;
            r.w.force_y = 819;
            const std::vector<std::string> log = info_lines([&] {
                r.until([&] { return !r.w.jumps.empty() && !r.w.flying; }, 14'000);
                r.run(1000);
            });
            check(r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::UpUp && r.st().jumps_worked == 1 &&
                      lines_with(log, "came out as") == 0 &&
                      lines_with(log, "jump up worked", "its 0x002A said a single jump (no second push)") == 1 &&
                      lines_with(log, "taken by its landing") == 1,
                  "Up,Up off at -20 with no second push seen, down on the planned floor: \"" +
                      line_with(log, "jump up worked") + "\"");
        }
    }
    {
        const std::string dir = way_back_folder();
        if (dir.empty()) return;
        // t85_s1: the way back from the ground under the home's platform at
        // 700 -- Ctrl+Up straight onto it -- down on the higher one at 500
        // beside it instead, inside the area, 500 px up: a move, nothing off.
        JumpRig r(owner_config(), 1000, 999, true, dir, "t85_s1");
        on_map(r, 85, 2000.0f, 999.0f);
        r.w.answers = { Answer::Outside };
        r.w.force_x = 1500;
        r.w.force_y = 499;
        r.hunter.set_zone(zone_box("t85_s1", 1000, 699, 600, 450, 1650, 900));
        const std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && !r.w.flying; }, 30'000);
            r.run(3000);
        });
        const bool first_high = !r.w.jumps.empty() && r.w.jumps[0].kind == JumpKind::CtrlUp;
        check(first_high && lines_with(log, "landed at 1500,499 on floor 500, above the planned 700", "taken as a move") ==
                                1 &&
                  lines_with(log, "OFF until") == 0 && lines_with(log, "came out as") == 0 &&
                  r.book.no_jumps("t85_s1").empty(),
              fmt("the way back's Ctrl+Up for 700 from %d,%d down on 500 beside it, inside the area within its reach: "
                  "\"%s\" -- nothing off",
                  first_high ? r.w.jumps[0].x : 0, first_high ? r.w.jumps[0].y : 0,
                  line_with(log, "taken as a move").c_str()));
    }
}

void test_jump_source() {
    section("Hunter 05b: what the jump up leaves alone -- no Down, no chord flag, no old jump, nothing of the facing");
#ifdef GOBOT_SOURCE_DIR
    const std::string jump = source("/src/hunter_jump.cpp");
    if (jump.empty()) {
        skip("the hunter's source is not here to read\n");
        return;
    }
    const auto has = [&](const char* what) { return jump.find(what) != std::string::npos; };
    check(!has("0x28") && !has("VK_DOWN") && !has("kChord") && !has("drop_ctrl_down") && !has("input_.jump(") &&
              !has("double_jump(") && !has("key_jump") && !has("facing_"),
          "hunter_jump.cpp: no Down, no chord flag, no InputManager::jump()/double_jump(), no key_jump, no facing");
    check(has("input_.jump_once(kUpVk, go_on)") && has("input_.jump_twice(kUpVk, kJumpUpSecondAtMs, go_on)") &&
              has("input_.high_jump(go_on)") && has("constexpr std::uint32_t kUpVk = 0x26;"),
          "its keys: always Up (0x26) -- jump_once, jump_twice at kJumpUpSecondAtMs, high_jump");
    // jump_go: only its own settle goes straight to jump_at; one under way for
    // another purpose or monster is let go of first, and this jump walked to
    // its own take-off -- never begun where the other one stopped.
    check(has("const bool mine = jump_.phase == JumpUpPhase::Settle && jump_.purpose == purpose && jump_.slot == slot "
              "&&") &&
              has("if (jump_.phase == JumpUpPhase::Settle && !mine) jump_settle_end(\"another jump\", false, now);") &&
              has("if (mine || std::abs(stand->x - plan.x) <= take_off_lead(plan))") &&
              !has("if (jump_.phase == JumpUpPhase::Settle || std::abs(stand->x - plan.x) <= kJumpUpLeadPx)"),
          "jump_go: another's settle let go of, the take-off walked to -- not jumped from where that one stood");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_jump_hunt(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_jump_rule();
    test_jump_way_back();
    test_jump_target();
    test_jump_fails();
    test_jump_misses_2026_11_15();
    test_jump_apex_report();
    test_jump_mid_air_move();
    test_jump_blind_landing();
    test_jump_twice();
    test_jump_hop();
    test_jump_off();
    test_way_back_levels();
    test_way_back_ramp();
    test_way_back_put_back();
    test_pit_edge();
    test_turn_pit();
    test_way_back_drops();
    test_jump_band();
    test_way_back_12c();
    test_review_bar();
    test_review_drop();
    test_review_way();
    test_trust_steps_again();
    test_way_back_jump_rounds();
    test_review_verdict();
    test_jump_source();
}
