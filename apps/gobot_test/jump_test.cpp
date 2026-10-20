// gobot_test -- "Thử" beside the jump key (gobot/jump_check.hpp): the key
// pressed exactly once, and what the character's plate did after; and what
// else the key is already used for. And the walker's jump key, hunt.ini's
// [Move] one: shipped as Up, and said when it would attack.
//
// Nothing here goes near the game: the plate is scripted, the keyboard is
// pretend and writes down every press, and the clock is the test's own.

#include "gobot/execution_coordinator.hpp"
#include "gobot/hotkeys.hpp"
#include "gobot/jump_check.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/navigator.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
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
        eyes.player = [this](Nameplate& out, bool* fresh) {
            if (fresh) *fresh = seen;
            if (away_after_reads > 0 && ++reads >= away_after_reads) in_front = false;
            const int up = pressed_at != 0 ? rise(t - pressed_at) : 0;
            out.bar = ScreenRect{ 620, 400 - up, 40, 3 };
            out.plate = ScreenRect{ 610, 386 - up, 60, 12 };
            out.is_self = true;
            // Remembered where it was, as the detector does, when not seen.
            return true;
        };
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
    std::printf("  [skip] no source folder\n");
#endif
    // A player's own file from before: Space, kept as it is, and said.
    const std::string old = temp_ini("walk_old.ini", "[Move]\nleft_key=37\nright_key=39\njump_key=32\n");
    NavigatorConfig before;
    check(before.load(old) && before.jump_key == VK_SPACE &&
              !JumpCheck::walk_key_warning_vi(before.jump_key, 'Z').empty(),
          "a file of the player's with jump_key=32: read as Space, as it says, and the warning shown");
    // A file without the key: Space, as every file before said -- and said.
    const std::string bare = temp_ini("walk_bare.ini", "[Move]\nleft_key=37\n");
    NavigatorConfig none;
    check(none.load(bare) && none.jump_key == VK_SPACE && !JumpCheck::walk_key_warning_vi(none.jump_key, 'Z').empty(),
          "a file without the key: Space, as before, and the warning shown");
    DeleteFileA(old.c_str());
    DeleteFileA(bare.c_str());
}

} // namespace

void test_jump(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_jump_check();
    test_jump_advice();
    test_walk_key();
}
