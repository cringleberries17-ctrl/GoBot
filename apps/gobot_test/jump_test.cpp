// gobot_test -- "Thử" beside the jump key (gobot/jump_check.hpp): the key
// pressed exactly once, and what the character's plate did after; and what
// else the key is already used for. And the walker's jump key, hunt.ini's
// [Move] one: shipped as Up, and said when it would attack.
//
// Nothing here goes near the game: the plate is scripted, the keyboard is
// pretend and writes down every press, and the clock is the test's own.

#include "gobot/execution_coordinator.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/hotkeys.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/jump_check.hpp"
#include "gobot/logger.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/navigator.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

/// The pretend game for one check: where the character's plate is at a given
/// moment, every key pressed, and a clock wound on by the check's own waits.
struct JumpRig {
    ExecutionCoordinator coord;
    JumpCheck            check_{ coord };
    std::uint64_t        t = 3'000'000;
    bool                 in_front = true;
    bool                 seen = true;
    /// The player clicks another window after this many reads of the plate;
    /// 0: never.
    int                  away_after_reads = 0;
    int                  reads = 0;
    /// The game loses focus while the key is down: the press does not come
    /// back as gone out.
    bool                 lost_during_press = false;
    /// The plate's rise, in pixels, `ms` after the press went out.
    std::function<int(std::uint64_t)> rise = [](std::uint64_t) { return 0; };
    std::uint64_t pressed_at = 0;
    std::vector<std::pair<std::uint32_t, int>> presses;

    JumpRig() {
        JumpCheck::Eyes eyes;
        eyes.player = plate_eye();
        check_.set_eyes(eyes);
        JumpCheck::Hands hands;
        hands.focus = [this] { return in_front; };
        hands.focused = [this] { return in_front; };
        hands.key = [this](std::uint32_t vk, int hold) {
            presses.emplace_back(vk, hold);
            pressed_at = t;
            if (lost_during_press) in_front = false;
            return in_front;
        };
        check_.set_hands(hands);
        JumpCheck::Clock clock;
        clock.now_ms = [this] { return t; };
        clock.sleep_ms = [this](std::uint32_t ms) { t += ms; };
        check_.set_clock(clock);
    }
    JumpCheck::Result run(std::uint32_t key = VK_UP) { return check_.run(key); }

    /// The character's plate, where `rise` has it.
    std::function<bool(Nameplate&, bool*)> plate_eye() {
        return [this](Nameplate& out, bool* fresh) {
            if (fresh) *fresh = seen;
            if (away_after_reads > 0 && ++reads >= away_after_reads) in_front = false;
            const int up = pressed_at != 0 ? rise(t - pressed_at) : 0;
            out.bar = ScreenRect{ 620, 400 - up, 40, 3 };
            out.plate = ScreenRect{ 610, 386 - up, 60, 12 };
            out.is_self = true;
            // Remembered where it was, as the detector does, when not seen.
            return true;
        };
    }

    /// The packets' count of your take-offs as well, asked by the check:
    /// `count` gives it, or nothing when the packets cannot tell.
    void count_takeoffs(std::function<std::optional<std::uint64_t>()> count) {
        JumpCheck::Eyes eyes;
        eyes.player = plate_eye();
        eyes.takeoffs = std::move(count);
        check_.set_eyes(eyes);
    }
};

/// A jump: up to `peak` px at 300 ms, and down again by 600.
int jump_curve(std::uint64_t ms, int peak) {
    if (ms >= 600) return 0;
    const double s = (static_cast<double>(ms) - 300.0) / 300.0;
    return static_cast<int>(peak * (1.0 - s * s));
}

void test_jump_check() {
    section("Phím nhảy, Thử: one press, and whether the character jumped");

    {
        JumpRig rig;
        rig.rise = [](std::uint64_t ms) { return jump_curve(ms, 110); };
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::Jumped && r.pressed && rig.presses.size() == 1 &&
                  rig.presses[0].first == VK_UP && rig.presses[0].second == JumpCheck::kHoldMs &&
                  r.rose_px >= 100 && r.rose_px <= 110,
              "a plate that rises 110 px and lands: jumped, " + std::to_string(r.rose_px) +
                  " px, the key pressed exactly once for 80 ms");
        const std::string said = JumpCheck::said_vi(r, "↑");
        check(said.find("đã nhảy") != std::string::npos && said.find("↑") != std::string::npos,
              "and it says so: " + said);
    }
    {
        JumpRig rig;
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::NoJump && rig.presses.size() == 1 &&
                  r.reads >= JumpCheck::kStillReads,
              "a plate that stays where it is: no jump seen, one press, " + std::to_string(r.reads) +
                  " reads after it");
        check(JumpCheck::said_vi(r, "Space").find("không thấy nhân vật nhảy") != std::string::npos,
              "and it sends the player to look in the game and its key settings");
    }
    {
        JumpRig rig;
        rig.seen = false;
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::NoPlate && rig.presses.size() == 1,
              "the character's plate not seen: pressed once all the same, and not checked");
        check(JumpCheck::said_vi(r, "↑").find("không kiểm tra được") != std::string::npos,
              "'không kiểm tra được'");
    }
    {
        JumpRig rig;
        rig.rise = [](std::uint64_t ms) { return ms < 600 ? 15 : 0; };
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::Unsure && rig.presses.size() == 1,
              "a plate that rises only 15 px: not sure, one press");
    }
    {
        JumpRig rig;
        rig.coord.engage_critical("trash cleaning");
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::Held && rig.presses.empty() && !r.pressed &&
                  r.hold == "trash cleaning",
              "a hold on: nothing pressed at all");
        check(JumpCheck::said_vi(r, "↑").find("Bot đang tạm dừng (trash cleaning)") != std::string::npos,
              "and it says the bot is held, and by what");
    }
    {
        JumpRig rig;
        rig.coord.request("hunt-attack", Priority::Combat);
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::Busy && rig.presses.empty(),
              "another job holding the keys throughout: nothing pressed");
    }
    {
        JumpRig rig;
        rig.in_front = false;
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::NotInFront && rig.presses.empty(),
              "the game not brought to the front: nothing pressed");
    }
    {
        // The player clicks GoBot while the plate is being read: the game is
        // asked again the moment before the press, and nothing goes.
        JumpRig rig;
        rig.away_after_reads = 2;
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::NotInFront && rig.presses.empty() && !r.pressed,
              "the game sent behind another window while the plate was read: nothing pressed, and "
              "'not in front' means just that");
    }
    {
        // The game loses focus while the key is down: the press may have
        // reached it. Not "nothing pressed".
        JumpRig rig;
        rig.lost_during_press = true;
        const JumpCheck::Result r = rig.run();
        const std::string said = JumpCheck::said_vi(r, "↑");
        check(r.outcome == JumpCheck::Outcome::Unchecked && r.pressed && rig.presses.size() == 1 &&
                  said.find("Có thể đã bấm ↑") != std::string::npos &&
                  said.find("Chưa bấm") == std::string::npos,
              "focus lost while the key was down: one press, said as maybe pressed and not checked "
              "-- " + said);
    }
    {
        JumpRig rig;
        const JumpCheck::Result r = rig.run(0);
        check(r.outcome == JumpCheck::Outcome::NoKey && rig.presses.empty(), "no jump key: nothing");
    }
    {
        // ↓ on a gate changes the map: never pressed, and said as not pressed
        // -- not as one that may have gone out.
        JumpRig rig;
        const JumpCheck::Result r = rig.run(VK_DOWN);
        const std::string said = JumpCheck::said_vi(r, "↓");
        check(r.outcome == JumpCheck::Outcome::NeverPressed && rig.presses.empty() && !r.pressed &&
                  said.find("Chưa bấm") != std::string::npos && said.find("không bấm ↓") != std::string::npos,
              "the jump key set to Down: nothing pressed, and said why -- " + said);
    }
    {
        JumpRig rig;
        std::atomic<bool> stop{ true };
        const JumpCheck::Result r = rig.check_.run(VK_UP, &stop);
        check(r.outcome == JumpCheck::Outcome::Stopped && rig.presses.empty(),
              "told to stop before the press -- F12, or GoBot closing: nothing pressed");
    }
}

void test_jump_advice() {
    section("Phím nhảy: what else the key is already");

    const std::vector<NamedKey> others = {
        { 17, "nhặt đồ" },              // Ctrl, the player's loot key
        { VK_LEFT, "đi sang trái" },
        { VK_RIGHT, "đi sang phải" },
        { 'X', "phím kỹ năng" },
        { '1', "bình HP" },
    };
    const auto is = [&](std::uint32_t jump, std::uint32_t attack, JumpCheck::Advice kind,
                        const char* what) {
        const JumpCheck::KeyAdvice a = JumpCheck::advice(jump, attack, others);
        return a.kind == kind && a.what_vi == what;
    };
    check(is('Z', 'Z', JumpCheck::Advice::Conflict, "đánh"), "the attack key: a clash, named 'đánh'");
    check(is(17, 'Z', JumpCheck::Advice::Conflict, "nhặt đồ"), "the loot key: a clash, named");
    check(is(VK_LEFT, 'Z', JumpCheck::Advice::Conflict, "đi sang trái") &&
              is(VK_RIGHT, 'Z', JumpCheck::Advice::Conflict, "đi sang phải"),
          "a direction key: a clash, named");
    check(is('X', 'Z', JumpCheck::Advice::Conflict, "phím kỹ năng"), "a skill key: a clash, named");
    check(is(VK_SPACE, 'Z', JumpCheck::Advice::SpaceIsAttack, ""),
          "Space with the attack key on Z: the warning that Space is the game's attack");
    check(is(VK_SPACE, VK_SPACE, JumpCheck::Advice::Conflict, "đánh"),
          "Space on Space: a clash with the attack key");
    check(is(0, 'Z', JumpCheck::Advice::NotSet, ""), "0: not set");
    check(is(VK_UP, 'Z', JumpCheck::Advice::Clean, ""), "Up with nothing else on it: clean");
    check(is(VK_DOWN, 'Z', JumpCheck::Advice::DownChangesMap, "") &&
              JumpCheck::advice(VK_DOWN, VK_DOWN, { { VK_DOWN, "phím kỹ năng" } }).kind ==
                  JumpCheck::Advice::DownChangesMap,
          "Down: never pressed, said before any clash -- on a gate it changes the map");
}

void test_never_down() {
    section("↓ is never pressed: on a gate it changes the map");

    check(InputManager::never_pressed(VK_DOWN) && !InputManager::never_pressed(VK_UP) &&
              !InputManager::never_pressed(VK_LEFT) && !InputManager::never_pressed(VK_RIGHT) &&
              !InputManager::never_pressed(0) && !InputManager::never_pressed('Z') &&
              InputManager::never_pressed(0x10028u) && InputManager::never_pressed(0xFFFF0028u),
          "Down, and only Down -- a file's 0x10028 or -65496 too, which reach the game as Down");
#ifdef GOBOT_SOURCE_DIR
    // Refused where every key goes out, before anything else is asked of it:
    // a key box set to it before it refused it presses nothing either.
    const auto read = [](const char* rel) {
        std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string im = read("/src/input_manager.cpp");
    const std::size_t send = im.find("bool InputManager::send_key(");
    const std::size_t typed = im.find("bool InputManager::type_key(");
    const std::size_t send_guard = im.find("if (down && never_pressed(vk))", send);
    const std::size_t send_real = im.find("refuse_real_input(down ?", send);
    const std::size_t typed_guard = im.find("if (never_pressed(vk))", typed);
    const std::size_t typed_real = im.find("refuse_real_input(\"type a key\")", typed);
    check(send != std::string::npos && send_guard < send_real && typed != std::string::npos &&
              typed_guard < typed_real,
          "send_key and type_key refuse Down before anything else -- every press, held key and typed key");
    const std::string view = read("/apps/gobot_gui/simple_view.cpp");
    check(view.find("InputManager::never_pressed(got)") != std::string::npos,
          "a key box refuses Down when it is pressed for it");
#else
    skip("no source folder\n");
#endif
}

void test_sightings() {
    section("Your character found or not: said once each way, and not for a moment's dropout");

    MobDetector::PlayerSightings s;
    const std::uint64_t t = 1'000'000, grace = MobDetector::kPlayerMemoryMs;
    const bool start_quiet = s.note(false, t, grace) == 0 && s.note(false, t + 5000, grace) == 0;
    s = MobDetector::PlayerSightings{};
    const int found = s.note(true, t, grace);
    const int again = s.note(true, t + 100, grace);
    // A dropout of 1.2 s, as the plate has several times a minute. One call
    // a statement: the operands of + may go in either order.
    int gone = s.note(false, t + 200, grace);
    gone += s.note(false, t + 1000, grace);
    gone += s.note(false, t + 1400, grace);
    const int back = s.note(true, t + 1400, grace);
    // Gone for good: said once, past the grace, and found again said once.
    int quiet = s.note(false, t + 2000, grace);
    quiet += s.note(false, t + 3400, grace);
    const int lost = s.note(false, t + 3600, grace);
    const int still = s.note(false, t + 9000, grace);
    const int refound = s.note(true, t + 9100, grace);
    check(start_quiet && found == 1 && again == 0 && gone == 0 && back == 0 && quiet == 0 && lost == -1 &&
              still == 0 && refound == 1,
          "found: said once; a 1.2-s dropout: nothing; gone past 1.5 s: said once; found again: said once");

    // The grace counted in looks: as many as 1.5 s is at 140 ms, at the wait
    // in use -- a resting detector's 600 ms waits 6 s, never under 1.5 s.
    check(MobDetector::sightings_grace_ms(140) == 1500 && MobDetector::sightings_grace_ms(600) == 6000 &&
              MobDetector::sightings_grace_ms(50) == 1500 && MobDetector::sightings_grace_ms(0) == 1500,
          "the grace: 1500 ms at 140 ms and at 50, 6000 at the resting 600; a wait of 0 counts as 140");
    // A resting detector's plate missed at every look, one every 620 ms: no
    // "Cannot find" through 9 looks (5.58 s), said at the 10th (6.2 s).
    {
        MobDetector::PlayerSightings r;
        const std::uint64_t g600 = MobDetector::sightings_grace_ms(600);
        const int first = r.note(true, t, g600);
        int quiet600 = 0;
        for (std::uint64_t k = 0; k <= 9; ++k) quiet600 += r.note(false, t + 100 + 620 * k, g600) == 0 ? 0 : 1;
        const int lost600 = r.note(false, t + 100 + 6200, g600);
        check(first == 1 && quiet600 == 0 && lost600 == -1,
              "resting at 600 ms, the plate missed every 620 ms: nothing through 5.58 s, \"Cannot find\" at the "
              "first look past 6 s");
    }
#ifdef GOBOT_SOURCE_DIR
    {
        std::ifstream in(std::string(GOBOT_SOURCE_DIR) + "/src/mob_detector.cpp", std::ios::binary);
        const std::string md((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::size_t from = md.find("void MobDetector::find_character(");
        const std::size_t end = from == std::string::npos ? std::string::npos : md.find("\n}", from);
        const std::string fc = end == std::string::npos ? std::string() : md.substr(from, end - from);
        const std::size_t warn = fc.find("Cannot find \\\"%s\\\" on screen for over %.1f s");
        check(fc.find("const std::uint64_t grace = sightings_grace_ms(period_ms_.load());") != std::string::npos &&
                  fc.find("sightings_.note(got, now_ms(), grace)") != std::string::npos &&
                  warn != std::string::npos &&
                  fc.find("static_cast<double>(grace) / 1000.0", warn) != std::string::npos &&
                  fc.find("kPlayerMemoryMs) / 1000.0") == std::string::npos,
              "find_character: \"Cannot find\" waits sightings_grace_ms at the detector's wait, and says that grace");
        // The same words at Info while the packets steer, and the warning
        // otherwise; told so by GoBot's window with the wait it sets.
        const std::size_t steer = fc.find("if (say < 0 && !packets_steer_.load())");
        const std::size_t info =
            fc.find("GB_INFO(\"Cannot find \\\"%s\\\" on screen for over %.1f s -- the packets steer");
        std::ifstream pin(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/packets_detector.cpp", std::ios::binary);
        const std::string pd((std::istreambuf_iterator<char>(pin)), std::istreambuf_iterator<char>());
        check(steer != std::string::npos && warn != std::string::npos && info != std::string::npos && steer < warn &&
                  warn < info &&
                  pd.find("detector_.set_packets_steer(p.hunting && p.source == PacketEyes::Source::Packets && "
                          "p.why == PacketWhy::InUse);") != std::string::npos,
              "find_character warns only while the packets do not steer, and says it at Info while they do -- set at "
              "every pace tick from the hunt, the source and its health");
    }
#endif
}

/// "Cannot find" while the packets steer the hunt: the same words, at Info
/// and not as a warning -- the hunt takes where you are from the packets
/// then, and both times it was said in the farm of 2026-11-03 the name was
/// right. The real detector's thread on frames the test draws (set_grab, a
/// window that is only a number), your name in the game's own glyphs.
void test_unfound_while_packets_steer() {
    section("Your character not found while the packets steer: said, not warned of");

    GlyphFont font;
    const std::string dir = GlyphFont::default_dir();
    Frame name;
    const char* const who = "ThanhAmCoc";
    if (dir.empty() || !font.load(dir) || !font.render_line(who, name)) {
        skip("no glyph set installed -- no name to draw\n");
        return;
    }
    Frame blank;
    blank.width = 1024;
    blank.height = 768;
    blank.pixels.resize(static_cast<std::size_t>(blank.width) * blank.height * 4);
    for (std::size_t i = 0; i < blank.pixels.size(); i += 4) {
        blank.pixels[i] = 190;          // the sky, BGRA
        blank.pixels[i + 1] = 150;
        blank.pixels[i + 2] = 120;
        blank.pixels[i + 3] = 255;
    }
    Frame named = blank;
    for (int y = 0; y < name.height; ++y)
        for (int x = 0; x < name.width; ++x) {
            const std::size_t from = (static_cast<std::size_t>(y) * name.width + x) * 4;
            const std::size_t to = (static_cast<std::size_t>(560 + y) * named.width + 180 + x) * 4;
            for (int c = 0; c < 4; ++c) named.pixels[to + c] = name.pixels[from + c];
        }

    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_level(LogLevel::Info);
    log.set_ring_capacity(400);
    log.clear_ring();
    const auto said = [&log](const char* text) {
        std::vector<LogLine> out;
        for (const LogLine& l : log.ring())
            if (l.text.find(text) != std::string::npos) out.push_back(l);
        return out;
    };
    const auto wait_for = [](const std::function<bool()>& cond) {
        for (int i = 0; i < 250 && !cond(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return cond();
    };

    std::atomic<bool> show{ true };     // before the detector, whose thread reads it
    MobDetector d;
    d.set_grab([&](void*, Frame& out) {
        out = show.load() ? named : blank;
        return true;
    });
    d.set_tuning(DetectorTuning::defaults());
    d.set_font(&font);
    d.set_character_name(who);
    d.set_period_ms(MobDetector::kMinPeriodMs);
    d.set_target_window(reinterpret_cast<void*>(std::uintptr_t{ 0x3000 }));
    d.start();
    const bool found = wait_for([&] { return said("Found your character").size() == 1; });
    d.set_packets_steer(true);
    show.store(false);
    const bool lost = wait_for([&] { return said("Cannot find \"ThanhAmCoc\"").size() == 1; });
    show.store(true);
    const bool back = wait_for([&] { return said("Found your character").size() == 2; });
    d.set_packets_steer(false);
    show.store(false);
    const bool lost_again = wait_for([&] { return said("Cannot find \"ThanhAmCoc\"").size() == 2; });
    d.stop();
    const std::vector<LogLine> lines = said("Cannot find \"ThanhAmCoc\" on screen for over 1.5 s");
    log.set_level(was);
    log.set_ring_capacity(0);

    check(found && lost && back && lost_again && lines.size() == 2,
          "found, lost, found again, lost again: \"Cannot find\" said twice, after the 1.5-s grace both times");
    check(lines.size() == 2 && lines[0].level == LogLevel::Info &&
              lines[0].text.find("-- the packets steer, so the hunt does not need it") != std::string::npos &&
              lines[1].level == LogLevel::Warn &&
              lines[1].text.find("-- check it against the name the game writes under your feet") != std::string::npos,
          "while the packets steer, at Info and saying so; with them not steering, the warning as before");
}

/// A file of the test's own, in the temp folder, holding `text`.
std::string temp_ini(const char* name, const std::string& text) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    const std::string path = (n == 0 || n > MAX_PATH ? std::string() : std::string(dir)) + "gobot_test_" +
                             std::to_string(GetCurrentProcessId()) + "_" + name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    return path;
}

void test_walk_key() {
    section("Phím nhảy khi đi tới điểm đã lưu: hunt.ini [Move] ships Up, and a key that attacks is said");
    const auto npos = std::string::npos;
    check(JumpCheck::walk_key_warning_vi(VK_UP, 'Z').empty() && JumpCheck::walk_key_warning_vi(0, 'Z').empty(),
          "Up, or no key at all: nothing to say");
    const std::string space = JumpCheck::walk_key_warning_vi(VK_SPACE, 'Z');
    check(space.find("Space") != npos && space.find("Tấn công") != npos && space.find("jump_key=38") != npos,
          "Space, the attack key on Z: said, Space being the game's attack -- " + space);
    const std::string same = JumpCheck::walk_key_warning_vi('Z', 'Z');
    const std::string both = JumpCheck::walk_key_warning_vi(VK_SPACE, VK_SPACE);
    check(same.find("trùng phím đánh") != npos && both.find("trùng phím đánh") != npos,
          "the attack key itself, Z or Space: said as the same key as the attack -- " + same);
#ifdef GOBOT_SOURCE_DIR
    NavigatorConfig shipped;
    check(shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/hunt.ini") && shipped.jump_key == VK_UP &&
              JumpCheck::walk_key_warning_vi(shipped.jump_key, 'Z').empty(),
          "the shipped hunt.ini: [Move] jump_key=38, Up, the game's jump -- nothing to say");
#else
    skip("no source folder\n");
#endif
    // A player's own file from before: Space, kept as it is, and said.
    const std::string old = temp_ini("walk_old.ini", "[Move]\nleft_key=37\nright_key=39\njump_key=32\n");
    NavigatorConfig before;
    check(before.load(old) && before.jump_key == VK_SPACE &&
              !JumpCheck::walk_key_warning_vi(before.jump_key, 'Z').empty(),
          "a file of the player's with jump_key=32: read as Space, as it says, and the warning shown");
    // A file without the key, or no file at all: Up, as the shipped file has
    // it -- never Space, this game's attack -- and nothing to say.
    const std::string bare = temp_ini("walk_bare.ini", "[Move]\nleft_key=37\n");
    NavigatorConfig none;
    check(none.load(bare) && none.jump_key == VK_UP && JumpCheck::walk_key_warning_vi(none.jump_key, 'Z').empty(),
          "a file without the key: Up, as the shipped file says, and no warning");
    const std::string no_move = temp_ini("walk_no_move.ini", "[Hunt]\nattack_key=90\n");
    NavigatorConfig unset, other_section;
    check(unset.jump_key == VK_UP && other_section.load(no_move) && other_section.jump_key == VK_UP,
          "no [Move] section, and the walker's own default: Up");
    DeleteFileA(no_move.c_str());
    DeleteFileA(old.c_str());
    DeleteFileA(bare.c_str());
}

/// Thử by the packets (the packets plan, item M): your client sends an 0x002A
/// as the character leaves the ground, and while the packets can tell, how
/// many went out after the press decides -- on a map where the camera follows
/// a jump, the plate hardly moves.
void test_jump_by_packets() {
    section("Phím nhảy, Thử: by your client's own take-offs, whenever the packets can tell");

    // The packets' running count of take-offs: 5 before the press, `after`
    // once the key has gone out.
    const auto counted = [](JumpRig& rig, std::uint64_t after) {
        rig.count_takeoffs([&rig, after]() -> std::optional<std::uint64_t> {
            return rig.pressed_at != 0 ? after : std::uint64_t{ 5 };
        });
    };
    {
        // A take-off went out, and the camera followed the jump: the plate
        // never moved.
        JumpRig rig;
        counted(rig, 6);
        const JumpCheck::Result r = rig.run();
        const std::string said = JumpCheck::said_vi(r, "↑");
        check(r.outcome == JumpCheck::Outcome::Jumped && r.by_packets && r.takeoffs == 1 && r.rose_px == 0 &&
                  r.reads >= JumpCheck::kStillReads && rig.presses.size() == 1 &&
                  said.find("đã nhảy (theo gói tin)") != std::string::npos,
              "take-offs 5 -> 6 and the plate still (the camera followed): jumped, by the packets, one press -- " +
                  said);
    }
    {
        // None went out, while the plate rose 110 px.
        JumpRig rig;
        rig.rise = [](std::uint64_t ms) { return jump_curve(ms, 110); };
        counted(rig, 5);
        const JumpCheck::Result r = rig.run();
        const std::string said = JumpCheck::said_vi(r, "↑");
        check(r.outcome == JumpCheck::Outcome::NoJump && r.by_packets && r.takeoffs == 0 && r.rose_px >= 100 &&
                  rig.presses.size() == 1 && said.find("không nhảy (theo gói tin)") != std::string::npos,
              "take-offs 5 -> 5: no jump, by the packets, whatever the plate does (it rose " +
                  std::to_string(r.rose_px) + " px) -- " + said);
    }
    {
        // The character's plate not seen at all: the packets still tell.
        JumpRig rig;
        rig.seen = false;
        counted(rig, 7);
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::Jumped && r.by_packets && r.takeoffs == 2 && r.reads == 0 &&
                  rig.presses.size() == 1,
              "the plate not seen, two take-offs after the press: jumped by the packets, not 'character not seen'");
    }
    {
        // The packets cannot tell: the plate decides, as it always has.
        JumpRig up, still;
        up.rise = [](std::uint64_t ms) { return jump_curve(ms, 110); };
        up.count_takeoffs([] { return std::optional<std::uint64_t>(); });
        still.count_takeoffs([] { return std::optional<std::uint64_t>(); });
        const JumpCheck::Result a = up.run(), b = still.run();
        check(a.outcome == JumpCheck::Outcome::Jumped && !a.by_packets && a.rose_px >= 100 &&
                  b.outcome == JumpCheck::Outcome::NoJump && !b.by_packets &&
                  JumpCheck::said_vi(a, "↑").find("cao khoảng") != std::string::npos,
              "nothing from the packets: by the plate, unchanged -- jumped at 110 px, no jump when it stays");
    }
    {
        // Able to tell before the press and not after it, or a count lower
        // after it -- a capture started afresh meanwhile: the plate decides.
        JumpRig lost, fell;
        lost.rise = [](std::uint64_t ms) { return jump_curve(ms, 110); };
        lost.count_takeoffs([&lost]() -> std::optional<std::uint64_t> {
            if (lost.pressed_at != 0) return std::nullopt;
            return std::uint64_t{ 5 };
        });
        counted(fell, 0);
        const JumpCheck::Result a = lost.run(), b = fell.run();
        check(a.outcome == JumpCheck::Outcome::Jumped && !a.by_packets && b.outcome == JumpCheck::Outcome::NoJump &&
                  !b.by_packets,
              "the packets able to tell before the press but not after it, or their count lower after it: the "
              "plate decides");
    }
    {
        // F12 or DỪNG at the first look after the press: the watch cut short,
        // the take-off's frame perhaps not in yet.
        const auto stopped_after_press = [](JumpRig& rig, std::atomic<bool>& stop) {
            JumpCheck::Clock clock;
            clock.now_ms = [&rig] { return rig.t; };
            clock.sleep_ms = [&rig, &stop](std::uint32_t ms) {
                rig.t += ms;
                if (rig.pressed_at != 0) stop = true;
            };
            rig.check_.set_clock(clock);
            return rig.check_.run(VK_UP, &stop);
        };
        JumpRig none, one;
        counted(none, 5);
        counted(one, 6);
        std::atomic<bool> stop_none{ false }, stop_one{ false };
        const JumpCheck::Result a = stopped_after_press(none, stop_none);
        const JumpCheck::Result b = stopped_after_press(one, stop_one);
        check(a.outcome != JumpCheck::Outcome::NoJump && !a.by_packets && a.takeoffs == 0 &&
                  none.presses.size() == 1 && b.outcome == JumpCheck::Outcome::Jumped && b.by_packets &&
                  b.takeoffs == 1,
              "the watch stopped right after the press: no take-off counted yet is no proof of no jump -- the plate "
              "decides (" + std::string(JumpCheck::outcome_name(a.outcome)) + "); one counted already is a jump, "
              "by the packets");
    }
    {
        // A hold on: nothing pressed, and nothing asked of the packets.
        JumpRig rig;
        int asked = 0;
        rig.count_takeoffs([&asked]() -> std::optional<std::uint64_t> {
            ++asked;
            return std::uint64_t{ 5 };
        });
        rig.coord.engage_critical("trash cleaning");
        const JumpCheck::Result r = rig.run();
        check(r.outcome == JumpCheck::Outcome::Held && rig.presses.empty() && !r.by_packets && asked == 0,
              "a hold on: nothing pressed, and the packets not asked");
    }
}

} // namespace

void test_jump(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_jump_check();
    test_jump_advice();
    test_never_down();
    test_sightings();
    test_unfound_while_packets_steer();
    test_walk_key();
    test_jump_by_packets();
}
