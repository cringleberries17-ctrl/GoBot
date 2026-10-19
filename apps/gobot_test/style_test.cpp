// gobot_test -- the hunter's attack styles ("Kiểu đánh"), keeping its distance
// ("Giữ khoảng cách"), turning only when it has to, the hop's jump key, and
// whether it is fighting -- what skills that follow the fight go by.
//
// Nothing here goes near the game. The hunter is driven through its seams on
// scripted plates, one pass at a time on a clock of the test's own, with a
// pretend keyboard that writes down every key -- and here, unlike
// hunter_test.cpp's, moves the character the way a held arrow would: 355 px a
// second, GoC's walking speed, or not at all against a wall.

#include "gobot/attack_style.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/mob_detector.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

/// A file in this run's own temp folder, which main.cpp makes and removes:
/// see temp_file there.
std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(wide(path), std::ios::binary | std::ios::trunc);
    out << text;
}

/// A plate standing at `x`, `y`: `x` the middle of its name, `y` its feet.
Nameplate plate(int x, int y, const std::string& name = {}, float hp = 1.0f) {
    Nameplate n;
    n.bar = ScreenRect{ x - 20, y, 40, 3 };
    n.plate = ScreenRect{ x - 30, y - 14, 60, 12 };
    n.name = name;
    n.name_confidence = name.empty() ? 0.0f : 0.9f;
    n.hp = hp;
    return n;
}

/// A config with the style of id `id` chosen, as the picker chooses it.
HunterConfig styled(const char* id) {
    HunterConfig cfg;
    if (const AttackStyle* s = find_attack_style(id)) apply_attack_style(cfg, *s);
    return cfg;
}

/// What the log said while `run` ran, at `level` and up.
struct Logged {
    std::vector<std::string> lines;
    /// Whether one line has every one of `words`.
    bool has(std::initializer_list<const char*> words) const {
        for (const std::string& l : lines) {
            bool all = true;
            for (const char* w : words) all = all && l.find(w) != std::string::npos;
            if (all) return true;
        }
        return false;
    }
};
Logged logged(LogLevel level, const std::function<void()>& run) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(2000);
    log.clear_ring();
    log.set_level(level);
    run();
    Logged out;
    for (const LogLine& l : log.ring()) out.lines.push_back(l.text);
    log.set_level(was);
    log.set_ring_capacity(0);
    return out;
}

// ------------------------------------------------------------------ the rig

/// The pretend game. A direction held 50 ms or more -- a walking step or a
/// step back, not a 30-ms turn -- moves the character at 355 px a second,
/// unless there is a wall; with `follow` the monsters keep their distance by
/// walking after it, and `ledge` drops it that far with the next step.
///
/// With `view_follows` the game's view moves with the character, as the real
/// one does: the character's plate stays where it is on the screen and the
/// world slides the other way under it -- all but a monster walking after it.
/// `frozen` is the detector still showing a picture taken a moment ago:
/// `freeze` takes it, and the eyes see it until `thaw`.
struct World {
    std::uint64_t t = 9'000'000;
    bool focused = true;
    bool see_me = true;
    int  me_x = 640, me_y = 400;
    std::vector<Nameplate> mobs;

    bool wall = false;
    bool follow = false;
    bool view_follows = false;
    int  ledge = 0;

    /// The number of the picture the eyes see, for a test that numbers them
    /// itself (Rig's `own_pictures`).
    std::uint64_t picture = 0;
    bool          frozen = false;
    int           frozen_me_x = 0, frozen_me_y = 0;
    std::vector<Nameplate> frozen_mobs;
    void freeze() {
        frozen = true;
        frozen_me_x = me_x;
        frozen_me_y = me_y;
        frozen_mobs = mobs;
    }
    void thaw() { frozen = false; }

    struct Press {
        std::uint64_t at;
        std::uint32_t vk;
        int           hold;
    };
    std::vector<Press> presses;
    int                releases = 0;

    void slide(int px, int py) {
        for (Nameplate& n : mobs) {
            n.bar.x += px;
            n.plate.x += px;
            n.bar.y += py;
            n.plate.y += py;
        }
    }
    void step_by(int px) {
        if (view_follows) {
            // The character stays put on the screen. A monster that does not
            // walk after it slides the other way; one that does stays where
            // it is. A drop moves the view down, and the monsters up.
            if (!follow) slide(-px, 0);
            slide(0, -ledge);
        } else {
            me_x += px;
            if (follow) slide(px, 0);
            me_y += ledge;
        }
        ledge = 0;
    }

    std::vector<Press> of(std::uint32_t vk, int min_hold = 0, int max_hold = 1 << 20) const {
        std::vector<Press> out;
        for (const Press& p : presses)
            if (p.vk == vk && p.hold >= min_hold && p.hold <= max_hold) out.push_back(p);
        return out;
    }
    /// Steps back or walking steps: a direction held 50 ms or more.
    std::size_t steps(std::uint32_t vk) const { return of(vk, 50).size(); }
    /// Turns: a direction tapped for 30 ms.
    std::size_t taps(std::uint32_t vk) const { return of(vk, 30, 30).size(); }
};

struct Rig {
    InputManager         input;
    ExecutionCoordinator coord;
    MobDetector          detector;
    Hunter               hunter{ input, coord, detector };
    World                w;

    /// `own_pictures`: the test numbers the detector's pictures itself
    /// (World::picture); otherwise each pass is a new one.
    explicit Rig(const HunterConfig& cfg, bool own_pictures = false) {
        Hunter::Eyes eyes;
        eyes.player = [this](Nameplate& out, bool* fresh) {
            if (fresh) *fresh = w.see_me;
            if (!w.see_me) return false;
            out = w.frozen ? plate(w.frozen_me_x, w.frozen_me_y, "Me") : plate(w.me_x, w.me_y, "Me");
            out.is_player = true;
            out.is_self = true;
            return true;
        };
        eyes.mobs = [this] {
            std::vector<Nameplate> out = w.frozen ? w.frozen_mobs : w.mobs;
            const int px = w.frozen ? w.frozen_me_x : w.me_x, py = w.frozen ? w.frozen_me_y : w.me_y;
            const auto cost = [px, py](const Nameplate& n) {
                const int dx = n.x() - px, dy = (n.y() - py) * 3;
                return dx * dx + dy * dy;
            };
            std::stable_sort(out.begin(), out.end(), [&](const Nameplate& a, const Nameplate& b) {
                return cost(a) < cost(b);
            });
            return out;
        };
        if (own_pictures) eyes.frame = [this] { return w.picture; };
        hunter.set_eyes(eyes);

        Hunter::Hands hands;
        hands.focused = [this] { return w.focused; };
        hands.key = [this](std::uint32_t vk, int hold) {
            w.presses.push_back(World::Press{ w.t, vk, hold });
            if ((vk == VK_LEFT || vk == VK_RIGHT) && hold >= 50 && !w.wall)
                w.step_by((vk == VK_LEFT ? -1 : 1) * hold * 355 / 1000);
            return true;
        };
        hands.key_up = [](std::uint32_t) {};
        hands.held = [](std::uint32_t) { return false; };
        hands.release_all = [this] { ++w.releases; };
        hunter.set_hands(hands);

        Hunter::Clock clock;
        clock.now_ms = [this] { return w.t; };
        clock.sleep_ms = [this](std::uint32_t ms) { w.t += ms; };
        hunter.set_clock(clock);

        hunter.set_config(cfg);
        hunter.start_stepped();
    }
    ~Rig() { hunter.stop(); }

    void pass() {
        hunter.step();
        w.t += 60;
    }
    void run(std::uint64_t ms, const std::function<void()>& each = nullptr) {
        for (const std::uint64_t until = w.t + ms; w.t < until;) {
            pass();
            if (each) each();
        }
    }
    Hunter::Status st() const { return hunter.status(); }
};

// ------------------------------------------------------------ the styles

void test_style_reach() {
    section("Kiểu đánh: up and down reach apart, from GoC's presets");

    const HunterConfig wide_cfg = styled("near_wide");
    check(wide_cfg.goc_rules() && wide_cfg.up() == 131 && wide_cfg.down() == 80,
          "near_wide: 131 px up, 80 down, GoC's rules on");
    check(Hunter::in_reach(wide_cfg, 60, -100) && !Hunter::in_reach(wide_cfg, 60, 100),
          "near_wide: a monster 100 px above is in reach, one 100 px below is not");
    {
        Rig r(wide_cfg);
        r.w.mobs = { plate(700, 300, "Cao") };
        r.pass();
        check(r.w.of('Z').size() == 1 && r.w.steps(VK_RIGHT) == 0 && r.w.of(VK_SPACE).empty() &&
                  r.st().state == Hunter::State::Attacking,
              "near_wide: 100 px above, attacked from the ground -- no step, no hop");
    }
    {
        Rig r(wide_cfg);
        r.w.mobs = { plate(700, 500, "Thap") };
        r.pass();
        check(r.w.of('Z').empty() && r.st().state == Hunter::State::Approaching,
              "near_wide: 100 px below, not attacked: it walks");
    }
    const HunterConfig narrow_cfg = styled("near_narrow");
    check(Hunter::in_reach(narrow_cfg, 0, -60) && !Hunter::in_reach(narrow_cfg, 0, -70),
          "near_narrow: 60 px above is in reach, 70 px above is not");
    const HunterConfig far_wide_cfg = styled("far_wide");
    check(!Hunter::in_reach(far_wide_cfg, 0, 70) && Hunter::in_reach(far_wide_cfg, 0, 63) &&
              Hunter::in_reach(far_wide_cfg, 340, -150),
          "far_wide: 70 px below is not (63 is), and 340 px off and 150 up is");

    // Without a style, the band it always had.
    const HunterConfig legacy;
    check(!legacy.goc_rules() && legacy.up() == 60 && legacy.down() == 60 && legacy.up_nj() == 30 &&
              legacy.down_nj() == 30 && Hunter::keep_away_px(legacy) == 50,
          "no style: 60 px either way, 30 without jumping, and a 50-px distance for keep_distance");
}

void test_style_presets() {
    section("Kiểu đánh: fifteen entries, Như trước, Tự chỉnh, and hunt.ini");

    const auto& styles = attack_styles();
    check(styles.size() == 15, "fifteen styles to pick from -- " + std::to_string(styles.size()));
    std::set<std::string> ids;
    bool all = true;
    const std::string path = temp_path("gobot_style.ini");
    for (const AttackStyle& s : styles) {
        ids.insert(s.id);
        HunterConfig cfg;
        apply_attack_style(cfg, s);
        const RangePreset& r = *s.ranges;
        const bool six = cfg.min_distance == r.min_range && cfg.reach == r.max_range &&
                         cfg.up_reach == r.highest_hit_y && cfg.down_reach == r.lowest_hit_y &&
                         cfg.up_no_jump == r.higher_threshold &&
                         cfg.down_no_jump == r.lower_threshold && cfg.style == s.id;
        HunterConfig back;
        const bool round = cfg.save(path) && back.load(path) && back.style == s.id &&
                           back.min_distance == r.min_range && back.reach == r.max_range &&
                           back.up() == r.highest_hit_y && back.down() == r.lowest_hit_y &&
                           back.up_nj() == r.higher_threshold && back.down_nj() == r.lower_threshold &&
                           attack_style_matches(back, s) && find_attack_style(s.id) == &s;
        if (!six || !round) {
            all = false;
            check(false, std::string("style ") + s.id + " writes its six numbers and reads them back");
        }
    }
    check(all && ids.size() == 15,
          "each of the fifteen writes its six numbers and its id, and reads them back");
    check(find_attack_style("near_wide")->ranges->name == std::string("Near_Wide") &&
              find_attack_style("dao_si")->ranges->name == std::string("Far_Wide") &&
              find_attack_style("tieu")->ranges->name == std::string("Far_Narrow") &&
              find_attack_style("vuot")->ranges->name == std::string("Near_Narrow") &&
              find_attack_style("") == nullptr && find_attack_style("not_a_style") == nullptr,
          "each class and weapon is on its GoC set; an empty or unknown id is no style");

    HunterConfig band;
    band.set_band(100, 40);
    check(band.up() == 100 && band.down() == 40 && band.up_nj() == 30 && band.down_nj() == 30 &&
              band.height_tolerance == 100 && band.style.empty(),
          "a band set by hand: up and down as given, the no-jump band left where it was, "
          "height_tolerance the larger, no style");

    HunterConfig cfg = styled("far_narrow");
    cfg.up_reach = 70;
    check(cfg.style == "far_narrow" && !attack_style_matches(cfg, *find_attack_style("far_narrow")),
          "a number edited by hand keeps the style, and no longer matches it (Tự chỉnh)");
    apply_legacy_style(cfg);
    check(cfg.style.empty() && !cfg.goc_rules() && cfg.reach == 150 && cfg.height_tolerance == 60 &&
              cfg.up_reach == -1 && cfg.down_reach == -1 && cfg.up_no_jump == -1 &&
              cfg.down_no_jump == -1 && cfg.min_distance == 0 && legacy_ranges(cfg),
          "Như trước: 150 / 60 / follow / 0, no style");

    // A file of before the styles.
    write_file(path, "[Hunt]\nheight_tolerance=44\nreach=150\n");
    HunterConfig old;
    check(old.load(path) && old.up() == 44 && old.down() == 44 && old.up_nj() == 22 &&
              old.down_nj() == 22 && old.min_distance == 0 && old.style.empty(),
          "a hunt.ini with only height_tolerance=44: up and down 44, 22 without jumping, no "
          "minimum, no style");
    IniFile raw;
    check(old.save(path) && raw.load(path) && raw.get_int("Hunt", "height_tolerance", 0) == 44 &&
              !raw.has_key("Hunt", "up_reach") && !raw.has_key("Hunt", "down_no_jump"),
          "saved unchanged: height_tolerance=44, and no up_reach written");

    // A band set once, then followed again: no stale number left behind.
    HunterConfig set = styled("near_wide");
    HunterConfig again;
    IniFile raw2;
    check(set.save(path) && again.load(path) && again.up() == 131, "a style's band is written");
    apply_legacy_style(again);
    HunterConfig third;
    check(again.save(path) && third.load(path) && raw2.load(path) && third.up() == 60 &&
              third.up_reach == -1 && raw2.get_int("Hunt", "up_reach", 0) == -1 &&
              raw2.get_int("Hunt", "height_tolerance", 0) == 60,
          "back to Như trước: up_reach=-1 over the old 131, and height 60 again");
    std::remove(path.c_str());
}

// ------------------------------------------------- giving up, with a style

void test_goc_give_up() {
    section("Kiểu đánh: a target in reach is fought until it dies, or until it takes no damage");

    {
        // In reach for 10 s, its bar going down slowly.
        Rig r(styled("near_wide"));
        r.w.mobs = { plate(760, 400, "Moc", 1.0f) };
        bool stayed = true;
        r.run(10'000, [&] {
            r.w.mobs[0].hp = std::max(0.2f, r.w.mobs[0].hp - 0.004f);
            if (r.st().state != Hunter::State::Attacking) stayed = false;
        });
        const auto swings = r.w.of('Z').size();
        check(stayed && swings >= 13 && r.st().targets_dropped == 0,
              "with a style, a target in reach for 10 s is attacked throughout and never dropped -- " +
                  std::to_string(swings) + " swings, " + std::to_string(r.st().targets_dropped) +
                  " dropped");
    }
    {
        // The same, with no style: picked afresh every 4 s, as it always was.
        Rig r(HunterConfig{});
        r.w.mobs = { plate(700, 400, "Moc", 1.0f) };
        r.run(10'000, [&] { r.w.mobs[0].hp = std::max(0.2f, r.w.mobs[0].hp - 0.004f); });
        check(r.st().targets_dropped == 2,
              "with no style, the same scene re-picks every 4 s, as before -- " +
                  std::to_string(r.st().targets_dropped) + " dropped in 10 s");
    }
    {
        // Up on a ledge it cannot climb: out of reach, given up on after give_up_ms.
        HunterConfig cfg = styled("near_narrow");
        cfg.jump_key = 0;
        Rig r(cfg);
        r.w.wall = true;
        r.w.mobs = { plate(900, 250, "Cao") };
        r.run(3900);
        const auto before = r.st().targets_dropped;
        r.run(400);
        check(before == 0 && r.st().targets_dropped == 1,
              "out of reach for give_up_ms (4 s): dropped once, not before");
    }
    {
        // In reach, the bar never moving.
        Rig r(styled("near_wide"));
        r.w.mobs = { plate(760, 400, "Da", 0.8f) };
        r.pass();
        const std::uint64_t first = r.w.of('Z').front().at;
        std::uint64_t dropped_at = 0;
        const Logged log = logged(LogLevel::Info, [&] {
            r.run(16'000, [&] {
                if (dropped_at == 0 && r.st().targets_dropped > 0) dropped_at = r.w.t;
            });
        });
        check(dropped_at != 0 && dropped_at - first >= 15'000 && dropped_at - first <= 15'300,
              "a target whose bar never goes down is given up on after 15 s of attacking -- at +" +
                  std::to_string(dropped_at ? dropped_at - first : 0) + " ms");
        check(log.has({ "giving up on Da", "không thấy mất máu" }),
              "and the log says so: 'không thấy mất máu'");
        const std::size_t swings = r.w.of('Z').size();
        r.run(5000);
        check(r.w.of('Z').size() == swings && r.st().state == Hunter::State::NoTargets,
              "and is passed over afterwards rather than picked straight back");
    }
    {
        // The same, with plates dropping out as they do: the monster's for
        // one look in 50 or in 25, its bar read empty one look in 50, the
        // character's own plate gone one look in 50. The watch lived in the
        // fight's log, which each such look ended and began again: with
        // the monster's plate missed now and then, it was never given up on.
        const auto given_up_after = [](int what, int every) -> std::uint64_t {
            Rig r(styled("near_wide"));
            r.w.mobs = { plate(760, 400, "Da", 0.8f) };
            r.pass();
            const std::uint64_t first = r.w.of('Z').front().at;
            std::vector<Nameplate> kept;
            std::uint64_t at = 0;
            int i = 0;
            r.run(30'000, [&] {
                if (at == 0 && r.st().targets_dropped > 0) at = r.w.t;
                const bool out = ++i % every == 0;
                if (what == 0) {
                    if (out) {
                        kept = r.w.mobs;
                        r.w.mobs.clear();
                    } else if (!kept.empty()) {
                        r.w.mobs = kept;
                        kept.clear();
                    }
                } else if (what == 1) {
                    if (!r.w.mobs.empty()) r.w.mobs[0].hp = out ? 0.0f : 0.8f;
                } else {
                    r.w.see_me = !out;
                }
            });
            return at ? at - first : 0;
        };
        const std::uint64_t gone50 = given_up_after(0, 50), gone25 = given_up_after(0, 25);
        const std::uint64_t empty50 = given_up_after(1, 50), me50 = given_up_after(2, 50);
        const auto within = [](std::uint64_t ms) { return ms >= 15'000 && ms <= 16'000; };
        check(within(gone50) && within(gone25),
              "its plate missed one look in 50, or in 25: still given up on after 15 s of attacking -- at +" +
                  std::to_string(gone50) + " and +" + std::to_string(gone25) + " ms (0: never)");
        check(within(empty50) && within(me50),
              "its bar read empty one look in 50, or the character's plate missed one in 50: likewise -- at +" +
                  std::to_string(empty50) + " and +" + std::to_string(me50) + " ms");
    }
    {
        // Given up on, and another monster 60 px from it fought next: watched
        // from nothing. Followed by where it stands, the watch would have
        // handed the first one's 15 s on to it, and it would have been given
        // up on at its first look.
        Rig r(styled("near_wide"));
        r.w.mobs = { plate(760, 400, "Da", 0.8f), plate(820, 400, "Moc", 1.0f) };
        std::uint64_t first_drop = 0;
        r.run(22'000, [&] {
            if (first_drop == 0 && r.st().targets_dropped > 0) first_drop = r.w.t;
            // The second is hurt once the first has been given up on.
            if (first_drop != 0) r.w.mobs[1].hp = std::max(0.2f, r.w.mobs[1].hp - 0.004f);
        });
        std::size_t at_moc = 0;
        for (const World::Press& p : r.w.of('Z'))
            if (first_drop != 0 && p.at > first_drop) ++at_moc;
        check(first_drop != 0 && r.st().targets_dropped == 1 && at_moc >= 6,
              "one given up on, the next 60 px away fought from nothing: not given up on -- " +
                  std::to_string(r.st().targets_dropped) + " given up, " + std::to_string(at_moc) +
                  " swings at the second");
    }
    {
        // A kill, and another monster where it fell whose bar never moves:
        // the watch, the first's bar falling to its end, hands on only a
        // moment of quiet -- given up on after about 15 s, not sooner.
        Rig r(styled("near_wide"));
        r.w.mobs = { plate(760, 400, "Moc", 1.0f) };
        std::uint64_t came = 0, dropped = 0;
        r.run(30'000, [&] {
            if (came == 0) {
                Nameplate& m = r.w.mobs[0];
                m.hp = std::max(0.0f, m.hp - 0.01f);
                if (m.hp <= 0.0f) {
                    r.w.mobs = { plate(770, 400, "Da", 0.8f) };
                    came = r.w.t;
                }
            } else if (dropped == 0 && r.st().targets_dropped > 0) {
                dropped = r.w.t;
            }
        });
        check(came != 0 && dropped != 0 && dropped - came >= 14'000 && dropped - came <= 16'000,
              "a kill, then one that takes no damage where it fell: given up on " +
                  std::to_string(dropped && came ? dropped - came : 0) + " ms after it came, not sooner");
    }
    {
        // The fight's line in the log: one for the fight, its plate missed
        // a look now and then -- not one for each stretch between the gaps.
        int fights = 0, finished = 0;
        const Logged log = logged(LogLevel::Debug, [&] {
            Rig r(styled("near_wide"));
            r.w.mobs = { plate(760, 400, "Moc", 1.0f) };
            std::vector<Nameplate> kept;
            int i = 0;
            r.run(9000, [&] {
                if (++i % 50 == 0 && !r.w.mobs.empty()) {
                    kept = r.w.mobs;
                    r.w.mobs.clear();
                } else if (!kept.empty()) {
                    r.w.mobs = kept;
                    kept.clear();
                }
                if (!r.w.mobs.empty()) r.w.mobs[0].hp = std::max(0.0f, r.w.mobs[0].hp - 0.007f);
            });
        });
        for (const std::string& l : log.lines)
            if (l.find("Hunter: fight over") != std::string::npos) {
                ++fights;
                if (l.find("(finished)") != std::string::npos) ++finished;
            }
        check(fights == 1 && finished == 1,
              "a fight with its plate missed one look in 50 is one fight in the log, over when it dies -- " +
                  std::to_string(fights) + " line(s)");
    }
    {
        // On "Đánh trước": never given up on for that.
        HunterConfig cfg = styled("near_wide");
        cfg.first_names = { "HoLy" };
        Rig r(cfg);
        r.w.mobs = { plate(760, 400, "HoLy", 0.8f) };
        r.run(20'000);
        check(r.st().targets_dropped == 0 && r.st().state == Hunter::State::Attacking,
              "a target on Đánh trước is never given up on for taking no damage");
    }
    {
        // With no style, no such drop.
        Rig r(HunterConfig{});
        r.w.mobs = { plate(700, 400, "Da", 0.8f) };
        r.run(20'000);
        check(r.st().state == Hunter::State::Attacking && r.w.of('Z').size() >= 25,
              "with no style, a target that takes no damage is still attacked, as before");
    }
}

// ---------------------------------------------------------------- the hop

void test_hop() {
    section("The jump key: a hop only while walking, only with a key, only to what is above");

    {
        HunterConfig cfg;           // 60 px band, 30 without jumping, Space
        Rig r(cfg);
        r.w.wall = true;
        r.w.mobs = { plate(900, 300, "Cao") };
        r.pass();
        check(r.w.of(VK_SPACE).size() == 1 && r.w.steps(VK_RIGHT) == 1,
              "100 px up and out of reach: a step and a hop, as before");
    }
    {
        HunterConfig cfg;
        cfg.jump_key = 0;
        Rig r(cfg);
        r.w.wall = true;
        r.w.mobs = { plate(900, 300, "Cao") };
        r.run(600);
        check(r.w.of(VK_SPACE).empty() && r.w.steps(VK_RIGHT) == 10,
              "with no jump key set: the steps, and never a hop");
    }
    {
        HunterConfig cfg;
        cfg.jump_key = VK_UP;
        Rig r(cfg);
        r.w.mobs = { plate(700, 380, "Gan") };
        r.run(600);
        check(r.w.of(VK_UP).empty() && !r.w.of('Z').empty(),
              "a monster 20 px up and within reach: attacked, no hop");
    }
    {
        HunterConfig cfg = styled("near_narrow");      // 63 up, 31 without jumping
        cfg.jump_key = VK_UP;
        Rig r(cfg);
        r.w.wall = true;
        r.w.mobs = { plate(950, 375, "Xa") };
        r.pass();
        const bool low = r.w.of(VK_UP).empty() && r.w.steps(VK_RIGHT) == 1;
        r.w.mobs = { plate(950, 300, "Cao") };
        r.pass();
        check(low && r.w.of(VK_UP).size() == 1,
              "with a style: walking to one 25 px up, no hop (within 31); to one 100 px up, a hop");
    }
}

// ------------------------------------------------------- keeping distance

HunterConfig keeping(const char* id = "near_narrow") {
    HunterConfig cfg = styled(id);      // near_narrow: 120 to 200 px, 63 / 31 up and down
    cfg.keep_distance = true;
    return cfg;
}

void test_keep_distance() {
    section("Giữ khoảng cách: a step back, bounded, then the target faced and attacked");

    check(Hunter::backstep_ms(120, 80) == 169 && Hunter::backstep_ms(50, 30) == 113 &&
              Hunter::backstep_ms(50, 60) == 50 && Hunter::backstep_ms(400, 0) == 800,
          "a step back is (distance - |dx| + 20) px at 355 px a second, from 50 to 800 ms");
    {
        Rig r(keeping());
        r.w.mobs = { plate(720, 400, "Moc") };        // 80 px to the right
        r.pass();
        const bool stepped = r.w.presses.size() == 1 && r.w.presses[0].vk == VK_LEFT &&
                             r.w.presses[0].hold >= 159 && r.w.presses[0].hold <= 179;
        r.pass();
        check(stepped && r.w.presses.size() == 3 && r.w.presses[1].vk == VK_RIGHT &&
                  r.w.presses[1].hold == 30 && r.w.presses[2].vk == 'Z',
              "min 120, a target at dx=80: a step back held " +
                  std::to_string(r.w.presses.empty() ? 0 : r.w.presses[0].hold) +
                  " ms, then one 30-ms turn toward it, then the attack");
        r.run(3000);
        check(r.w.steps(VK_LEFT) == 1 && r.w.taps(VK_RIGHT) == 1 && r.st().backsteps == 1,
              "far enough after it: no more steps back, and no more turns");
    }
    {
        HunterConfig cfg = keeping();
        cfg.min_distance = 0;
        Rig r(cfg);
        r.w.mobs = { plate(670, 400, "Moc") };        // 30 px
        r.pass();
        check(r.w.presses.size() == 1 && r.w.presses[0].vk == VK_LEFT &&
                  std::abs(r.w.presses[0].hold - 113) <= 5,
              "no minimum set and dx=30: GoC's 50 px, a step held about 113 ms");
    }
    {
        // One that follows: dx stays 80 however far it steps.
        Rig r(keeping());
        r.w.follow = true;
        r.w.mobs = { plate(720, 400, "Moc") };
        r.run(6000);
        check(r.w.steps(VK_LEFT) == 3 && r.st().backsteps == 3 && r.w.of('Z').size() >= 6,
              "a target that follows: exactly 3 steps back, then it is fought where it stands -- " +
                  std::to_string(r.w.steps(VK_LEFT)) + " steps");
        // Another one, where the first was: the count starts again.
        r.w.mobs[0].hp = 0.0f;
        r.pass();
        r.w.mobs = { plate(r.w.me_x - 80, 400, "Moc2") };
        r.run(6000);
        check(r.w.steps(VK_RIGHT) == 3,
              "a new target, 80 px on the other side: three steps back again -- " +
                  std::to_string(r.w.steps(VK_RIGHT)));
    }
    {
        Rig r(keeping());
        r.w.wall = true;
        r.w.mobs = { plate(720, 400, "Moc") };
        r.run(4000);
        check(r.w.steps(VK_LEFT) == 1 && !r.w.of('Z').empty(),
              "a step that moves nothing -- a wall -- is not followed by a second");
    }
    {
        Rig r(keeping());
        r.w.ledge = 25;
        r.w.mobs = { plate(720, 400, "Moc") };
        r.w.follow = true;
        r.run(4000);
        check(r.w.steps(VK_LEFT) == 1, "nor one that drops the character 25 px -- a ledge");
    }

    // Never: standing still, switched off, a target off its level, a hold.
    {
        HunterConfig cfg = keeping();
        cfg.stay_put = true;
        Rig r(cfg);
        r.w.mobs = { plate(720, 400, "Moc") };
        r.run(2000);
        check(r.w.steps(VK_LEFT) == 0 && !r.w.of('Z').empty(),
              "standing still (Đứng yên): never a step back, only the attack");
    }
    {
        HunterConfig cfg = styled("near_narrow");
        Rig r(cfg);
        r.w.mobs = { plate(720, 400, "Moc") };
        r.run(2000);
        check(r.w.steps(VK_LEFT) == 0, "keep_distance off: no step back");
    }
    {
        Rig r(keeping());
        r.w.mobs = { plate(720, 350, "Cao") };        // 50 px up: in reach, off the level
        r.run(2000);
        check(r.w.steps(VK_LEFT) == 0 && !r.w.of('Z').empty(),
              "a target 50 px above -- outside the 31-px band it is fought in without jumping: "
              "no step back");
    }
    {
        Rig r(keeping());
        r.w.mobs = { plate(720, 400, "Moc") };
        r.coord.engage_critical("reviving");
        r.run(2000);
        const bool none = r.w.presses.empty();
        r.coord.disengage_critical();
        r.pass();
        check(none && r.w.steps(VK_LEFT) == 1,
              "under a hold: nothing at all, the step back only once it is over");
    }
}

/// The monster `r` fights, moved `px` to the side.
void nudge(Rig& r, int px) {
    for (Nameplate& n : r.w.mobs) {
        n.bar.x += px;
        n.plate.x += px;
    }
}

void test_keep_distance_bound() {
    section("Giữ khoảng cách: the bound holds through a ledge, a dropout and an old picture");

    {
        // A step back goes down a 25-px ledge; the next pass attacks from far
        // enough, which starts the steps in a row afresh; then the monster
        // walks back in. Judged only when another step was wanted, the step
        // down the ledge went unjudged, and the next went down another.
        Rig r(keeping());                               // 120 px, 31 without jumping
        r.w.ledge = 25;
        r.w.mobs = { plate(720, 400, "Moc", 0.9f) };    // 80 px; it does not follow
        r.pass();
        const bool down = r.w.steps(VK_LEFT) == 1 && r.w.me_y == 425;
        r.pass();
        const auto swings = r.w.of('Z');
        const bool from_far = swings.size() == 1 && r.w.mobs[0].x() - r.w.me_x >= 120;
        r.w.ledge = 25;                                 // a second drop behind it
        r.run(4000, [&] {
            const int dx = r.w.mobs[0].x() - r.w.me_x;
            if (dx > 80) nudge(r, -std::min(7, dx - 80));
        });
        check(down && from_far && r.w.steps(VK_LEFT) == 1 && r.st().backsteps == 1 && r.w.me_y == 425,
              "a step down a 25-px ledge, an attack from 139 px, the monster back at 80 px: no second "
              "step -- " + std::to_string(r.w.steps(VK_LEFT)) + " step(s), the character " +
                  std::to_string(r.w.me_y - 400) + " px down");
    }

    // A monster that walks after the character (dx stays 80), its plate gone
    // for two passes, or its bar read empty for one, every 50 passes. Each
    // ended the fight, and with it the count: 24 steps back in 24 s.
    const auto chased = [](bool view_follows, int what) {
        Rig r(keeping());
        r.w.follow = true;
        r.w.view_follows = view_follows;
        r.w.mobs = { plate(720, 400, "Moc", 0.9f) };
        std::vector<Nameplate> kept;
        int i = 0;
        r.run(24'000, [&] {
            ++i;
            if (what == 0) {
                if (i % 50 == 0) {
                    kept = r.w.mobs;
                    r.w.mobs.clear();
                } else if (i % 50 == 2 && !kept.empty()) {
                    r.w.mobs = kept;
                }
            } else if (!r.w.mobs.empty()) {
                r.w.mobs[0].hp = i % 50 == 0 ? 0.0f : 0.9f;
            }
        });
        return std::make_pair(r.w.steps(VK_LEFT), r.st().backsteps);
    };
    {
        const auto gone = chased(false, 0);
        const auto empty_bar = chased(false, 1);
        check(gone.first == 3 && gone.second == 3 && empty_bar.first == 3,
              "a monster that follows, its plate gone two passes in fifty, or its bar read empty one "
              "pass in fifty: still exactly 3 steps back in 24 s -- " + std::to_string(gone.first) +
                  " and " + std::to_string(empty_bar.first));
        const auto view = chased(true, 0);
        check(view.first == 1,
              "and with the view following the character -- nothing on the screen moves, a wall -- "
              "1 step, dropouts or not -- " + std::to_string(view.first));
    }

    {
        // The detector's pictures come slower than the passes, and the one it
        // has just after a step may have been taken before or during it,
        // showing nothing moved -- "a wall" that is not there, or a ledge not
        // seen. The step is judged on a picture taken after it, and no other
        // goes before.
        Rig r(keeping(), true);
        r.w.follow = true;
        r.w.mobs = { plate(720, 400, "Moc", 0.9f) };
        r.w.picture = 1;
        r.w.freeze();                    // the picture there is, taken before the step
        r.pass();
        const bool one = r.w.steps(VK_LEFT) == 1;
        r.w.picture = 2;                 // put out after the step, taken before it
        r.run(1200);
        const bool waited = r.w.steps(VK_LEFT) == 1 && !r.w.of('Z').empty();
        r.w.thaw();
        r.w.picture = 3;                 // taken after it: it moved, and nothing went wrong
        r.run(600);
        check(one && waited && r.w.steps(VK_LEFT) == 2,
              "a step is judged on a picture taken after it, not on one that still shows the "
              "character where it was: no second step meanwhile, and no wall made of an old picture");
    }
}

void test_keep_distance_log() {
    section("Giữ khoảng cách: how far each fight was fought from, in the log");

    // One monster after another turning up 80 px off: each stepped back from
    // once and fought to its death from ~139 px, for a minute and a second.
    int fights = 0;
    const Logged log = logged(LogLevel::Debug, [&] {
        Rig r(keeping());
        r.w.mobs = { plate(r.w.me_x + 80, 400, "Moc", 1.0f) };
        r.run(61'000, [&] {
            Nameplate& m = r.w.mobs[0];
            if (m.hp <= 0.001f) {
                r.w.mobs = { plate(r.w.me_x + 80, 400, "Moc", 1.0f) };
                ++fights;
            } else {
                m.hp = std::max(0.0f, m.hp - 0.02f);
            }
        });
    });
    check(fights >= 10 && log.has({ "Hunter: fight over (finished)", "median 139 px", "1 back-steps" }),
          "each fight's line: the distance it was fought from (median 139 px) and its one step "
          "back -- " + std::to_string(fights) + " fights");
    check(log.has({ "Hunter: ", " fights, ", " with attacks at >= 120 px, ", " back-steps, ",
                    " 0 given up" }),
          "and once a minute: how many fights, how many fought from 120 px or more, the steps back "
          "and the give-ups");
}

// ------------------------------------------------------------------ facing

void test_facing() {
    section("Facing: with a style or keep_distance, a turn only when needed");

    {
        Rig r(styled("near_wide"));
        r.w.mobs = { plate(800, 400, "Moc", 0.9f) };      // 160 px to the right
        r.pass();
        const std::size_t first = r.w.taps(VK_RIGHT);
        r.run(50 * 60);
        check(first == 1 && r.w.taps(VK_RIGHT) == 1,
              "facing it already: one turn at the start, then none over 50 passes (as before: one "
              "a pass) -- " + std::to_string(r.w.taps(VK_RIGHT)));
        // A hold over: which way it faces is not known any more.
        r.coord.engage_critical("trash cleaning");
        r.run(300);
        r.coord.disengage_critical();
        r.pass();
        r.run(600);
        check(r.w.taps(VK_RIGHT) == 2, "a hold ending: the next pass turns once more, and only once");
        r.w.focused = false;
        r.run(300);
        r.w.focused = true;
        r.run(600);
        check(r.w.taps(VK_RIGHT) == 3, "the game losing focus: once more too");
    }
    {
        Rig r(styled("near_wide"));
        r.w.mobs = { plate(650, 400, "Gan") };            // 10 px: within 12
        r.run(1200);
        check(r.w.taps(VK_RIGHT) == 0 && !r.w.of('Z').empty(), "|dx| within facing_tolerance: no turn");
    }
    {
        const std::string path = temp_path("gobot_facing.ini");
        write_file(path, "[Hunt]\nstyle=near_wide\nfacing_tolerance=0\n");
        HunterConfig zero;
        zero.load(path);
        write_file(path, "[Hunt]\nfacing_tolerance=40\n");
        HunterConfig forty;
        forty.load(path);
        std::remove(path.c_str());
        Rig a(zero);
        a.w.mobs = { plate(645, 400, "Moc") };
        a.pass();
        Rig b(forty);
        b.w.mobs = { plate(670, 400, "Moc") };
        b.run(600);
        const std::size_t within = b.w.taps(VK_RIGHT);
        b.w.mobs = { plate(700, 400, "Moc") };
        b.run(600);
        check(zero.facing_tolerance == 0 && a.w.taps(VK_RIGHT) == 1 && forty.facing_tolerance == 40 &&
                  within == 0 && b.w.taps(VK_RIGHT) == 10,
              "facing_tolerance 0 and 40 from hunt.ini: a turn at 5 px with 0; none at 30 px with 40, "
              "and, with no style, one a pass at 60 px as before");
    }
}

// -------------------------------------------------------- fighting or not

void test_engaged() {
    section("Fighting, for the skills: only attacking a target in reach and facing it");

    {
        Rig r(styled("near_wide"));
        r.w.mobs = { plate(760, 400, "Moc") };
        r.pass();
        Hunter::Status st = r.st();
        check(r.hunter.engaged() && r.hunter.engaged_dx() == 120 && st.engaged &&
                  st.engaged_since_ms == r.w.t - 60 - 50,
              "attacking a target 120 px off, turned to it: fighting, at 120 px");
        r.coord.engage_critical("reviving");
        const bool held = !r.hunter.engaged();
        r.pass();
        r.coord.disengage_critical();
        r.pass();
        check(held && r.hunter.engaged(), "a hold beginning: not fighting at once; over, fighting again");

        // The plate drops out: kept for 500 ms, then not.
        r.w.see_me = false;
        r.pass();
        const bool kept = r.hunter.engaged();
        r.run(480);
        const bool later = r.hunter.engaged();
        check(kept && !later, "the character out of sight: still fighting a moment, not past 500 ms");
        r.w.see_me = true;
        r.run(300);
        check(r.hunter.engaged(), "back in sight: fighting again");

        r.w.mobs[0].hp = 0.0f;
        r.pass();
        check(!r.hunter.engaged(), "the target's bar empty: not fighting, at once");
        r.w.mobs.clear();
        r.pass();
        check(!r.hunter.engaged() && r.st().state == Hunter::State::NoTargets, "nothing left: NoTargets");
    }
    {
        Rig r(styled("near_wide"));
        r.w.wall = true;
        r.w.mobs = { plate(1000, 400, "Xa") };
        r.run(600);
        check(!r.hunter.engaged() && r.st().state == Hunter::State::Approaching,
              "walking to a target: not fighting");
    }
    {
        HunterConfig cfg = styled("near_wide");
        cfg.stay_put = true;
        Rig r(cfg);
        r.w.mobs = { plate(1000, 400, "Xa") };
        r.run(600);
        check(!r.hunter.engaged() && r.st().state == Hunter::State::Waiting,
              "standing still with nothing in reach: not fighting");
    }
    {
        Rig r(keeping());
        r.w.mobs = { plate(720, 400, "Moc") };
        r.pass();
        const bool stepping = !r.hunter.engaged() && r.w.steps(VK_LEFT) == 1;
        r.pass();
        check(stepping && r.hunter.engaged(),
              "the pass that steps back: not fighting; the next, turned to it: fighting");
    }
    {
        Rig r(styled("near_wide"));
        r.w.mobs = { plate(760, 400, "Moc") };
        r.pass();
        r.w.focused = false;
        r.pass();
        const bool unfocused = !r.hunter.engaged();
        r.w.focused = true;
        r.pass();
        r.hunter.stop();
        check(unfocused && !r.hunter.engaged(), "the game out of focus, and a stop: not fighting");
    }
    {
        // No style: turned to it by a tap on every pass, as before -- fighting.
        Rig r(HunterConfig{});
        r.w.mobs = { plate(700, 400, "Moc") };
        r.run(600);
        check(r.hunter.engaged() && r.hunter.engaged_dx() == 60,
              "with no style too: attacking and turned to it is fighting");
    }
}

// ------------------------------------------------- what an old setup does

void test_old_setup() {
    section("An existing hunt.ini fights as before, until a style or keep_distance is chosen");

    const std::string path = temp_path("gobot_old_hunt.ini");
    write_file(path,
               "[Hunt]\nenabled=true\ncharacter_name=X\nattack_key=90\njump_key=32\nreach=150\n"
               "height_tolerance=60\ngive_up_ms=4000\nstep_ms=180\nattack_interval_ms=700\n"
               "jump_to_reach=true\nstay_put=false\n");
    HunterConfig old;
    const bool loaded = old.load(path);
    check(loaded && old.style.empty() && !old.goc_rules() && !old.keep_distance &&
              old.keep_distance_steps == 3 && old.facing_tolerance == 12 && old.min_distance == 0 &&
              old.up_reach == -1 && old.down_reach == -1 && old.up_no_jump == -1 &&
              old.down_no_jump == -1 && old.no_damage_ms == 15000 && old.jump_key == 32 &&
              !old.jump_checked,
          "a hunt.ini from before: no style, keep_distance off, 3 steps, 12 px, the band following "
          "height_tolerance, jump_key 32 kept, not tried");
    IniFile raw;
    check(old.save(path) && raw.load(path) && raw.get("Hunt", "style", "x").empty() &&
              !raw.get_bool("Hunt", "keep_distance", true) &&
              raw.get_int("Hunt", "keep_distance_steps", 0) == 3 &&
              raw.get_int("Hunt", "facing_tolerance", 0) == 12 &&
              raw.get_int("Hunt", "min_distance", 1) == 0 && !raw.has_key("Hunt", "up_reach") &&
              raw.get_int("Hunt", "jump_key", 0) == 32 && !raw.get_bool("Hunt", "jump_checked", true),
          "saved without a change: every new key at what it was before, the band left unwritten");

    // The same scene on the old file and on no file at all: the same keys at
    // the same moments, a face tap on every pass among them.
    HunterConfig fresh;
    fresh.character_name = old.character_name;
    fresh.enabled = old.enabled;
    Rig a(old), b(fresh);
    for (Rig* r : { &a, &b }) {
        r->w.mobs = { plate(700, 400, "Moc", 0.6f), plate(980, 400, "Da"), plate(660, 330, "Cao") };
    }
    a.run(9000, [&] { a.w.mobs[0].hp = std::max(0.0f, a.w.mobs[0].hp - 0.003f); });
    b.run(9000, [&] { b.w.mobs[0].hp = std::max(0.0f, b.w.mobs[0].hp - 0.003f); });
    bool same = a.w.presses.size() == b.w.presses.size();
    for (std::size_t i = 0; same && i < a.w.presses.size(); ++i)
        same = a.w.presses[i].vk == b.w.presses[i].vk && a.w.presses[i].hold == b.w.presses[i].hold &&
               a.w.presses[i].at == b.w.presses[i].at;
    check(same && a.w.taps(VK_RIGHT) + a.w.taps(VK_LEFT) > 20 && a.st().backsteps == 0,
          "the old file and the defaults press the same keys at the same moments, with a turn on "
          "every pass and no step back");

    // The new keys survive a round trip.
    HunterConfig set;
    set.keep_distance = true;
    set.keep_distance_steps = 5;
    set.facing_tolerance = 40;
    set.no_damage_ms = 9000;
    set.set_jump_key(VK_UP);
    set.jump_checked = true;
    HunterConfig back;
    check(set.save(path) && back.load(path) && back.keep_distance && back.keep_distance_steps == 5 &&
              back.facing_tolerance == 40 && back.no_damage_ms == 9000 && back.jump_key == VK_UP &&
              back.jump_checked,
          "keep_distance, its steps, facing_tolerance, no_damage_ms, jump_key and jump_checked read back");
    back.set_jump_key(VK_UP);
    const bool same_key = back.jump_checked;
    back.set_jump_key(VK_SPACE);
    check(same_key && !back.jump_checked && back.jump_key == VK_SPACE,
          "the same jump key again keeps 'tried'; another clears it");
    write_file(path, "[Hunt]\nkeep_distance_steps=99\nfacing_tolerance=-5\nmin_distance=900\nreach=200\n");
    HunterConfig clamped;
    check(clamped.load(path) && clamped.keep_distance_steps == 10 && clamped.facing_tolerance == 0 &&
              clamped.min_distance == 200,
          "out of range in the file: 1-10 steps, 0-100 px, and the minimum never past the reach");
    std::remove(path.c_str());
}

} // namespace

void test_hunter_style(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_style_reach();
    test_style_presets();
    test_goc_give_up();
    test_hop();
    test_keep_distance();
    test_keep_distance_bound();
    test_keep_distance_log();
    test_facing();
    test_engaged();
    test_old_setup();
}
