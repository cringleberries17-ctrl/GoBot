#include "gobot/macro.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <random>
#include <thread>

namespace gobot {
namespace {

int random_percent_offset(int pct) {
    if (pct <= 0) return 0;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(-pct, pct);
    return dist(rng);
}

/// `base` give or take `total_pct` percent, never under 1 ms. Takes no lock:
/// for a caller that holds the config's already.
std::uint32_t jitter(std::uint32_t base, int total_pct) {
    if (total_pct <= 0 || base == 0) return base;
    const double scale = 1.0 + random_percent_offset(total_pct) / 100.0;
    const double out = static_cast<double>(base) * scale;
    return static_cast<std::uint32_t>(std::max(1.0, out));
}

} // namespace

const char* to_string(MacroTrigger t) {
    switch (t) {
        case MacroTrigger::HpBelow: return "hp_below";
        case MacroTrigger::MpBelow: return "mp_below";
        case MacroTrigger::Always:  break;
    }
    return "always";
}

MacroTrigger macro_trigger_from_string(const std::string& s) {
    if (s == "hp_below") return MacroTrigger::HpBelow;
    if (s == "mp_below") return MacroTrigger::MpBelow;
    return MacroTrigger::Always;
}

MacroKeyKind macro_key_kind(const MacroKey& k) {
    switch (k.trigger) {
        case MacroTrigger::HpBelow: return MacroKeyKind::HpPotion;
        case MacroTrigger::MpBelow: return MacroKeyKind::MpPotion;
        case MacroTrigger::Always:  break;
    }
    return k.interval_ms >= kBuffMinIntervalMs ? MacroKeyKind::Buff : MacroKeyKind::Skill;
}

int clamp_buff_seconds(int seconds) {
    return std::clamp(seconds, kBuffMinSeconds, kBuffMaxSeconds);
}

std::uint32_t clamp_skill_ms(std::uint32_t ms) {
    // In tenths of a second, as the window shows them.
    const std::uint32_t tenths = (ms + 50) / 100 * 100;
    return std::clamp(tenths, kSkillMinMs, kSkillMaxMs);
}

std::vector<std::size_t> macro_keys_of_kind(const std::vector<MacroKey>& keys, MacroKeyKind kind) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (macro_key_kind(keys[i]) == kind) out.push_back(i);
    return out;
}

int clamp_potion_pct(int pct) {
    return std::clamp(pct, kPotionMinPct, kPotionMaxPct);
}

int macro_urgent_potion(const std::vector<MacroKey>& keys, MacroKeyKind kind, float pct) {
    // No reading: nothing can be told to drink, which is what holds every
    // potion key whose bar is not being read.
    if (pct < 0.0f) return -1;
    int chosen = -1;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const MacroKey& k = keys[i];
        if (macro_key_kind(k) != kind || !k.enabled || k.keycode == 0 || k.interval_ms == 0) continue;
        if (pct > static_cast<float>(k.threshold_pct)) continue;
        // By the mark and not by the row, so a Lv2 set above its Lv1 works
        // the same way round; the first on a tie.
        if (chosen < 0 || k.threshold_pct < keys[static_cast<std::size_t>(chosen)].threshold_pct)
            chosen = static_cast<int>(i);
    }
    return chosen;
}

MacroKey macro_potion_key(MacroKeyKind kind, std::uint32_t keycode, int tier) {
    MacroKey k;
    const bool hp = kind == MacroKeyKind::HpPotion;
    if (!hp && kind != MacroKeyKind::MpPotion) return k;
    const bool second = tier >= 2;
    k.keycode = keycode;
    k.enabled = true;
    k.trigger = hp ? MacroTrigger::HpBelow : MacroTrigger::MpBelow;
    // The second potion is the one for when the first has not been enough:
    // lower down, as GoC's Lv2 sits under its Lv1.
    k.threshold_pct = hp ? (second ? 30 : 50) : (second ? 20 : 30);
    // The interval is the cooldown between two drinks, not a timer.
    k.interval_ms = 3000;
    k.jitter_pct = 15;
    k.label = std::string(hp ? "HP potion" : "MP potion") + (second ? " 2" : "");
    return k;
}

bool drop_empty_keys(std::vector<MacroKey>& keys) {
    // Which go, in one pass: the first potion of each kind is that row's Lv1.
    // Called on every frame of the simple screen, so nothing is moved unless
    // something goes.
    std::vector<char> drop(keys.size(), 0);
    bool hp_first = true, mp_first = true, any = false;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        bool first = false;
        switch (macro_key_kind(keys[i])) {
            case MacroKeyKind::HpPotion: first = hp_first; hp_first = false; break;
            case MacroKeyKind::MpPotion: first = mp_first; mp_first = false; break;
            case MacroKeyKind::Skill:
            case MacroKeyKind::Buff:     break;
        }
        if (keys[i].keycode == 0 && !first) {
            drop[i] = 1;
            any = true;
        }
    }
    if (!any) return false;
    std::size_t at = 0;
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (!drop[i]) {
            if (at != i) keys[at] = std::move(keys[i]);
            ++at;
        }
    keys.resize(at);
    return true;
}

std::vector<int> macro_key_match(const std::vector<MacroKey>& was, const std::vector<MacroKey>& now) {
    std::vector<int> out(now.size(), -1);
    std::vector<char> taken(was.size(), 0);
    for (std::size_t i = 0; i < now.size(); ++i) {
        const MacroKey& k = now[i];
        const MacroKeyKind kind = macro_key_kind(k);
        for (std::size_t j = 0; j < was.size(); ++j) {
            if (taken[j] || was[j].keycode != k.keycode || was[j].trigger != k.trigger ||
                macro_key_kind(was[j]) != kind)
                continue;
            taken[j] = 1;
            out[i] = static_cast<int>(j);
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------- config

bool MacroConfig::needs_vitals() const {
    for (const auto& k : keys)
        if (k.enabled && k.conditional()) return true;
    return false;
}

void MacroConfig::seed_vitals_defaults() {
    // Measured off a 1024x768 GhostOnline client. Both bars start at x=83 and
    // are 102 wide; HP spans rows 7..14 and MP rows 21..28.
    //
    // Only the middle rows are read. The bars are steep vertical gradients --
    // mana runs 0,8,123 at its top edge through 0,181,255 in the middle --
    // so the centre matches one colour where the full height cannot.
    //
    // Exact for that client size and approximate for any other, which is what
    // the HP / MP tab's "find the bars" is for.
    hp_bar = BarSpec{};
    hp_bar.x = 83;  hp_bar.y = 8;  hp_bar.width = 102; hp_bar.height = 5;
    hp_bar.r = 239; hp_bar.g = 0;  hp_bar.b = 0;       hp_bar.tolerance = 80;

    mp_bar = BarSpec{};
    mp_bar.x = 83;  mp_bar.y = 22; mp_bar.width = 102; mp_bar.height = 5;
    mp_bar.r = 0;   mp_bar.g = 49; mp_bar.b = 206;     mp_bar.tolerance = 80;
}

void MacroConfig::apply_defaults() {
    keys.clear();
    // A plausible starting layout: a few attack skills on short timers and a
    // couple of buffs on long ones. Staggered so they do not all come due on
    // the same tick.
    //
    // The two potion keys come pre-wired to a vital rather than to a timer:
    // that is what a potion key is for, and a 3-second cooldown with a
    // threshold behaves correctly even if the bars are never calibrated (it
    // simply holds, and says so) whereas a 30-second timer silently drinks.
    const struct {
        const char*   label;
        std::uint32_t vk;
        std::uint32_t interval;
        bool          on;
        MacroTrigger  trigger;
        int           threshold;
    } kDefaults[] = {
        { "Skill 1",  'Z',  1200,   true,  MacroTrigger::Always,  0  },
        { "Skill 2",  'X',  3000,   true,  MacroTrigger::Always,  0  },
        { "Skill 3",  'C',  6000,   false, MacroTrigger::Always,  0  },
        { "Skill 4",  'V',  9000,   false, MacroTrigger::Always,  0  },
        { "Buff 1",   '3',  300000, false, MacroTrigger::Always,  0  },
        { "Buff 2",   '4',  300000, false, MacroTrigger::Always,  0  },
        { "HP potion",'1',  3000,   false, MacroTrigger::HpBelow, 50 },
        { "MP potion",'2',  3000,   false, MacroTrigger::MpBelow, 30 },
    };

    std::uint32_t stagger = 0;
    for (const auto& d : kDefaults) {
        MacroKey k;
        k.label = d.label;
        k.keycode = d.vk;
        k.interval_ms = d.interval;
        k.enabled = d.on;
        k.trigger = d.trigger;
        k.threshold_pct = d.threshold;
        k.start_delay_ms = stagger;
        stagger += 150;
        keys.push_back(std::move(k));
    }

    sequence.clear();
    sequence.push_back({ 'Z', 0, 250, "Skill 1" });
    sequence.push_back({ 'X', 0, 400, "Skill 2" });
    sequence.push_back({ 'C', 0, 600, "Skill 3" });

    seed_vitals_defaults();
}

bool MacroConfig::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) {
        GB_WARN("MacroConfig: cannot read %s -- using defaults", path.c_str());
        apply_defaults();
        return false;
    }

    timers_enabled    = ini.get_bool("Macro", "timers_enabled", true);
    sequence_enabled  = ini.get_bool("Macro", "sequence_enabled", false);
    sequence_loop     = ini.get_bool("Macro", "sequence_loop", true);
    require_focus     = ini.get_bool("Macro", "require_focus", true);
    focus_on_start    = ini.get_bool("Macro", "focus_on_start", true);
    global_jitter_pct = ini.get_int ("Macro", "global_jitter_pct", 0);
    toggle_key        = static_cast<std::uint32_t>(ini.get_int("Macro", "toggle_key", 121));
    panic_key         = static_cast<std::uint32_t>(ini.get_int("Macro", "panic_key", 123));
    // Missing from a file written before skills could follow the fight: off,
    // and every key on its timer, as it was. The rest are GoC's own numbers.
    skills_follow_fight = ini.get_bool("Macro", "skills_follow_fight", false);
    sequential_skills   = ini.get_bool("Macro", "sequential_skills", true);
    skill_mp_floor_pct  = std::clamp(ini.get_int("Macro", "skill_mp_floor_pct", 10), 0, kSkillMaxMpFloor);
    skill_gap_ms        = static_cast<std::uint32_t>(
        std::clamp(ini.get_int("Macro", "skill_gap_ms", 50), static_cast<int>(kSkillGapMinMs),
                   static_cast<int>(kSkillGapMaxMs)));

    keys.clear();
    for (std::size_t i = 0; i < kMaxKeys; ++i) {
        const std::string p = "key_" + std::to_string(i) + "_";
        if (!ini.has_key("Keys", p + "keycode")) continue;

        MacroKey k;
        k.keycode        = static_cast<std::uint32_t>(ini.get_int("Keys", p + "keycode", 0));
        k.label          = ini.get      ("Keys", p + "label", "");
        k.enabled        = ini.get_bool ("Keys", p + "enabled", false);
        k.interval_ms    = static_cast<std::uint32_t>(ini.get_int("Keys", p + "interval_ms", 1000));
        k.hold_ms        = static_cast<std::uint32_t>(ini.get_int("Keys", p + "hold_ms", 0));
        k.start_delay_ms = static_cast<std::uint32_t>(ini.get_int("Keys", p + "start_delay_ms", 0));
        k.jitter_pct     = ini.get_int ("Keys", p + "jitter_pct", 10);
        k.drop_if_busy   = ini.get_bool("Keys", p + "drop_if_busy", true);
        k.trigger        = macro_trigger_from_string(ini.get("Keys", p + "trigger", "always"));
        k.threshold_pct  = ini.get_int ("Keys", p + "threshold_pct", 50);
        k.always_timer   = ini.get_bool("Keys", p + "always_timer", false);
        k.range_px       = std::clamp(ini.get_int("Keys", p + "range_px", 0), 0, kSkillMaxRangePx);

        if (k.label.empty()) k.label = InputManager::key_name(k.keycode);
        keys.push_back(std::move(k));
    }
    if (keys.empty()) apply_defaults();

    // Seed first, then read over the top, so a file written before the Vitals
    // section existed gets the starting guess rather than a zero-width bar.
    seed_vitals_defaults();
    vitals_enabled = ini.get_bool("Vitals", "enabled", false);
    const struct { const char* prefix; BarSpec* bar; } kBars[] = {
        { "hp_", &hp_bar },
        { "mp_", &mp_bar },
    };
    for (const auto& b : kBars) {
        const std::string p = b.prefix;
        b.bar->x         = ini.get_int("Vitals", p + "x", b.bar->x);
        b.bar->y         = ini.get_int("Vitals", p + "y", b.bar->y);
        b.bar->width     = ini.get_int("Vitals", p + "width", b.bar->width);
        b.bar->height    = ini.get_int("Vitals", p + "height", b.bar->height);
        b.bar->r         = static_cast<std::uint8_t>(ini.get_int("Vitals", p + "r", b.bar->r));
        b.bar->g         = static_cast<std::uint8_t>(ini.get_int("Vitals", p + "g", b.bar->g));
        b.bar->b         = static_cast<std::uint8_t>(ini.get_int("Vitals", p + "b", b.bar->b));
        b.bar->tolerance = ini.get_int("Vitals", p + "tolerance", b.bar->tolerance);
    }

    sequence.clear();
    for (std::size_t i = 0; i < 64; ++i) {
        const std::string p = "step_" + std::to_string(i) + "_";
        if (!ini.has_key("Sequence", p + "keycode")) continue;

        MacroStep s;
        s.keycode        = static_cast<std::uint32_t>(ini.get_int("Sequence", p + "keycode", 0));
        s.hold_ms        = static_cast<std::uint32_t>(ini.get_int("Sequence", p + "hold_ms", 0));
        s.delay_after_ms = static_cast<std::uint32_t>(ini.get_int("Sequence", p + "delay_after_ms", 100));
        s.label          = ini.get("Sequence", p + "label", "");
        sequence.push_back(std::move(s));
    }

    GB_INFO("MacroConfig: %zu timed keys, %zu sequence steps from %s",
            keys.size(), sequence.size(), path.c_str());
    return true;
}

bool MacroConfig::save(const std::string& path) const {
    IniFile ini;

    ini.set_bool("Macro", "timers_enabled", timers_enabled);
    ini.set_bool("Macro", "sequence_enabled", sequence_enabled);
    ini.set_bool("Macro", "sequence_loop", sequence_loop);
    ini.set_bool("Macro", "require_focus", require_focus);
    ini.set_bool("Macro", "focus_on_start", focus_on_start);
    ini.set_int ("Macro", "global_jitter_pct", global_jitter_pct);
    ini.set_int ("Macro", "toggle_key", static_cast<int>(toggle_key));
    ini.set_int ("Macro", "panic_key", static_cast<int>(panic_key));
    ini.set_bool("Macro", "skills_follow_fight", skills_follow_fight);
    ini.set_bool("Macro", "sequential_skills", sequential_skills);
    ini.set_int ("Macro", "skill_mp_floor_pct", skill_mp_floor_pct);
    ini.set_int ("Macro", "skill_gap_ms", static_cast<int>(skill_gap_ms));

    for (std::size_t i = 0; i < keys.size() && i < kMaxKeys; ++i) {
        const auto& k = keys[i];
        const std::string p = "key_" + std::to_string(i) + "_";
        ini.set     ("Keys", p + "label", k.label);
        ini.set_int ("Keys", p + "keycode", static_cast<int>(k.keycode));
        ini.set_bool("Keys", p + "enabled", k.enabled);
        ini.set_int ("Keys", p + "interval_ms", static_cast<int>(k.interval_ms));
        ini.set_int ("Keys", p + "hold_ms", static_cast<int>(k.hold_ms));
        ini.set_int ("Keys", p + "start_delay_ms", static_cast<int>(k.start_delay_ms));
        ini.set_int ("Keys", p + "jitter_pct", k.jitter_pct);
        ini.set_bool("Keys", p + "drop_if_busy", k.drop_if_busy);
        ini.set     ("Keys", p + "trigger", to_string(k.trigger));
        ini.set_int ("Keys", p + "threshold_pct", k.threshold_pct);
        ini.set_bool("Keys", p + "always_timer", k.always_timer);
        ini.set_int ("Keys", p + "range_px", k.range_px);
    }

    ini.set_bool("Vitals", "enabled", vitals_enabled);
    const struct { const char* prefix; const BarSpec* bar; } kBars[] = {
        { "hp_", &hp_bar },
        { "mp_", &mp_bar },
    };
    for (const auto& b : kBars) {
        const std::string p = b.prefix;
        ini.set_int("Vitals", p + "x", b.bar->x);
        ini.set_int("Vitals", p + "y", b.bar->y);
        ini.set_int("Vitals", p + "width", b.bar->width);
        ini.set_int("Vitals", p + "height", b.bar->height);
        ini.set_int("Vitals", p + "r", b.bar->r);
        ini.set_int("Vitals", p + "g", b.bar->g);
        ini.set_int("Vitals", p + "b", b.bar->b);
        ini.set_int("Vitals", p + "tolerance", b.bar->tolerance);
    }

    for (std::size_t i = 0; i < sequence.size(); ++i) {
        const auto& s = sequence[i];
        const std::string p = "step_" + std::to_string(i) + "_";
        ini.set     ("Sequence", p + "label", s.label);
        ini.set_int ("Sequence", p + "keycode", static_cast<int>(s.keycode));
        ini.set_int ("Sequence", p + "hold_ms", static_cast<int>(s.hold_ms));
        ini.set_int ("Sequence", p + "delay_after_ms", static_cast<int>(s.delay_after_ms));
    }

    return ini.save(path, "GoBot keyboard macro");
}

// ---------------------------------------------------------------- engine

MacroEngine::MacroEngine(InputManager& input, ExecutionCoordinator& coord)
    : input_(input), coord_(coord) {
    config_.apply_defaults();
    for (std::size_t i = 0; i < config_.keys.size(); ++i) key_ids_.push_back(next_key_id_++);
}

MacroEngine::~MacroEngine() { stop(); }

void MacroEngine::set_config(const MacroConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MacroKey> keys = cfg.keys;
    if (keys.size() > MacroConfig::kMaxKeys) keys.resize(MacroConfig::kMaxKeys);

    // Each key keeps its own times and tally. By where it stands while the
    // keys stay as they were; by which key it is once one has been added,
    // taken out or changed: kept by place, a skill taken out from in front of
    // a potion left the potion with the skill's due time -- a buff's, minutes
    // off -- and with HP low it did not drink.
    bool same = keys.size() == config_.keys.size();
    for (std::size_t i = 0; same && i < keys.size(); ++i)
        same = keys[i].keycode == config_.keys[i].keycode && keys[i].trigger == config_.keys[i].trigger;
    std::vector<int> from(keys.size(), -1);
    if (same) {
        for (std::size_t i = 0; i < keys.size(); ++i) from[i] = static_cast<int>(i);
    } else {
        from = macro_key_match(config_.keys, keys);
    }
    // And each its id, which a pass of the timers under way goes by.
    std::vector<std::uint64_t> ids(keys.size(), 0);
    for (std::size_t i = 0; i < keys.size(); ++i)
        ids[i] = from[i] >= 0 && static_cast<std::size_t>(from[i]) < key_ids_.size()
                     ? key_ids_[static_cast<std::size_t>(from[i])]
                     : next_key_id_++;

    if (running_.load()) {
        const std::uint64_t now = clock_now();
        std::vector<std::uint64_t> due(keys.size(), 0), pressed(keys.size(), 0);
        std::vector<char> fresh(keys.size(), 0);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const MacroKey& k = keys[i];
            const int j = from[i];
            if (j < 0 || static_cast<std::size_t>(j) >= due_ms_.size()) {
                // New to the list: a potion is due at once -- its due time is
                // its cooldown -- and anything else at its start delay.
                due[i] = k.conditional() ? now : now + k.start_delay_ms;
                fresh[i] = 1;
                continue;
            }
            const std::size_t w = static_cast<std::size_t>(j);
            due[i] = due_ms_[w];
            pressed[i] = w < pressed_ms_.size() ? pressed_ms_[w] : 0;
            // A timed key made shorter while the macro runs is due at its
            // last press plus the new interval. Left alone, the time armed
            // at that press stood: a buff turned down from 1600 s to 60 s on
            // the simple screen during a run went out next 26 minutes later.
            // A potion's interval is a cooldown its trigger re-arms anyway.
            const MacroKey& was = config_.keys[w];
            if (!k.conditional() && pressed[i] != 0 && k.interval_ms < was.interval_ms)
                due[i] = std::min(due[i], pressed[i] + k.interval_ms);
        }
        // A new potion on the key of one already there -- a Lv2 put on Lv1's
        // key -- is the same potion in the game: it waits out that one's
        // cooldown rather than drinking again at once.
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (!fresh[i] || !keys[i].conditional()) continue;
            for (std::size_t s = 0; s < keys.size(); ++s)
                if (!fresh[s] && keys[s].conditional() && keys[s].keycode == keys[i].keycode &&
                    due[s] > due[i]) {
                    due[i] = due[s];
                    pressed[i] = pressed[s];
                }
        }
        due_ms_ = std::move(due);
        pressed_ms_ = std::move(pressed);
    }

    // The presses each key has made go with it, so a tally by key -- the
    // simple screen's "Bình HP" -- does not take a skill's for a potion's.
    // The stats lock inside the config's, never the other way round.
    if (!same) {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        if (!stats_.key_presses.empty()) {
            std::vector<std::uint64_t> counts(keys.size(), 0);
            for (std::size_t i = 0; i < keys.size(); ++i)
                if (from[i] >= 0 && static_cast<std::size_t>(from[i]) < stats_.key_presses.size())
                    counts[i] = stats_.key_presses[static_cast<std::size_t>(from[i])];
            stats_.key_presses = std::move(counts);
        }
    }
    config_ = cfg;
    config_.keys = std::move(keys);
    key_ids_ = std::move(ids);
}

int MacroEngine::slot(std::size_t i) const {
    if (i >= pass_ids_.size()) return -1;
    const std::uint64_t id = pass_ids_[i];
    // Where it was, nearly always.
    if (i < key_ids_.size() && key_ids_[i] == id) return static_cast<int>(i);
    for (std::size_t s = 0; s < key_ids_.size(); ++s)
        if (key_ids_[s] == id) return static_cast<int>(s);
    return -1;
}

void MacroEngine::count_press(std::size_t i) {
    // The stats lock inside the config's, as set_config takes them.
    std::lock_guard<std::mutex> lock(mutex_);
    const int s = slot(i);
    if (s < 0) return;
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    if (stats_.key_presses.size() < config_.keys.size())
        stats_.key_presses.resize(config_.keys.size(), 0);
    if (static_cast<std::size_t>(s) < stats_.key_presses.size())
        ++stats_.key_presses[static_cast<std::size_t>(s)];
}

MacroConfig MacroEngine::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

std::uint64_t MacroEngine::clock_now() const {
    return clock_.now_ms ? clock_.now_ms() : now_ms();
}

void MacroEngine::set_vitals(float hp_pct, float mp_pct) {
    const std::uint64_t now = clock_now();
    vit_hp_.store(hp_pct);
    vit_mp_.store(mp_pct);
    vit_at_ms_.store(now);
    if (hp_pct >= 0.0f && hp_pct < kDownHpPct) down_ms_.store(now);
    box_vitals(now, hp_pct, mp_pct);
}

// ------------------------------------------------------------- black box

const char* MacroEngine::potion_why_name(PotionWhy why) {
    switch (why) {
        case PotionWhy::Pressed:    return "pressed";
        case PotionWhy::AboveMark:  return "above the mark";
        case PotionWhy::NoReading:  return "no reading";
        case PotionWhy::Cooling:    return "cooling";
        case PotionWhy::OtherTier:  return "other tier";
        case PotionWhy::NotInFront: return "game not in front";
        case PotionWhy::NotInWorld: return "not in the world";
        case PotionWhy::KeysHeld:   return "keys held";
        case PotionWhy::KeysBusy:   return "keys busy";
        case PotionWhy::KeyFailed:  return "key failed";
        case PotionWhy::BehindHp:   return "behind the HP potion";
    }
    return "?";
}

namespace {

/// A percent as the black box keeps it: rounded, -1 for no reading.
int box_pct(float pct) { return pct < 0.0f ? -1 : static_cast<int>(pct + 0.5f); }

/// A vitals record changes so seldom that one at most this often is plenty:
/// the reader pushes every frame of the window.
constexpr std::uint64_t kBoxVitalsEveryMs = 200;

} // namespace

void MacroEngine::box_vitals(std::uint64_t t, float hp_pct, float mp_pct) {
    const BoxVitals v{ t, box_pct(hp_pct), box_pct(mp_pct) };
    std::lock_guard<std::mutex> lock(box_mutex_);
    if (box_have_vitals_) {
        const BoxVitals& last = box_last_vitals_;
        if (v.hp == last.hp && v.mp == last.mp) return;
        if (t < last.t + kBoxVitalsEveryMs) return;
    }
    box_last_vitals_ = v;
    box_have_vitals_ = true;
    box_vitals_.push_back(v);
    box_trim(t);
}

void MacroEngine::box_decide(std::uint64_t t, std::uint64_t key, const MacroKey& k, bool hp, PotionWhy why,
                             const std::string& text) {
    std::lock_guard<std::mutex> lock(box_mutex_);
    // The minute's tally: the reason that ends now had lasted since it began.
    PotionMinute& m = potion_minute_[key];
    m.keycode = k.keycode;
    m.mark = k.threshold_pct;
    m.hp = hp;
    if (m.since == 0) m.since = t;
    const auto last = box_last_.find(key);
    const bool same = last != box_last_.end() && last->second.first == why && last->second.second == text;
    if (why == PotionWhy::Pressed) ++m.presses;
    if (why != PotionWhy::AboveMark) m.active = true;
    if (why == PotionWhy::KeysHeld && !text.empty()) m.held_by = text;
    if (same && why != PotionWhy::Pressed) return;
    m.ms[static_cast<int>(m.why)] += t > m.since ? t - m.since : 0;
    m.why = why;
    m.text = text;
    m.since = t;
    box_last_[key] = { why, text };
    box_decisions_.push_back(BoxDecision{ t, key, k.keycode, k.threshold_pct, hp, why, text });
    box_trim(t);
}

void MacroEngine::box_trim(std::uint64_t now) {
    // Older than kBlackBoxKeepMs goes -- but not the newest such record of
    // the vitals, nor of each key: what was so when the window opens. HP above
    // the mark for a minute before a burst is one record a minute old.
    const std::uint64_t cut = now > kBlackBoxKeepMs ? now - kBlackBoxKeepMs : 0;
    while (box_vitals_.size() >= 2 && box_vitals_[1].t < cut) box_vitals_.pop_front();
    if (!box_decisions_.empty() && box_decisions_.front().t < cut) {
        std::map<std::uint64_t, std::size_t> newest_old;      // key -> the newest record before the cut
        for (std::size_t i = 0; i < box_decisions_.size() && box_decisions_[i].t < cut; ++i)
            newest_old[box_decisions_[i].key] = i;
        std::deque<BoxDecision> kept;
        for (std::size_t i = 0; i < box_decisions_.size(); ++i) {
            const BoxDecision& d = box_decisions_[i];
            if (d.t >= cut || newest_old[d.key] == i) kept.push_back(std::move(box_decisions_[i]));
        }
        box_decisions_.swap(kept);
    }
    // And never more than kBlackBoxMax records: the oldest go first.
    while (box_vitals_.size() + box_decisions_.size() > kBlackBoxMax) {
        const bool vitals_older = !box_vitals_.empty() &&
                                  (box_decisions_.empty() || box_vitals_.front().t <= box_decisions_.front().t);
        if (vitals_older) box_vitals_.pop_front();
        else              box_decisions_.pop_front();
    }
}

std::size_t MacroEngine::black_box_size() const {
    std::lock_guard<std::mutex> lock(box_mutex_);
    return box_vitals_.size() + box_decisions_.size();
}

std::vector<std::string> MacroEngine::black_box_lines() const {
    const std::uint64_t now = clock_now();
    std::deque<BoxVitals> vitals;
    std::deque<BoxDecision> decisions;
    {
        std::lock_guard<std::mutex> lock(box_mutex_);
        vitals = box_vitals_;
        decisions = box_decisions_;
    }
    // The keys in the order of the config, HP's first; one taken out since
    // goes after them.
    std::vector<std::uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const bool hp : { true, false })
            for (std::size_t i = 0; i < config_.keys.size() && i < key_ids_.size(); ++i) {
                const MacroKeyKind kind = macro_key_kind(config_.keys[i]);
                if (kind == (hp ? MacroKeyKind::HpPotion : MacroKeyKind::MpPotion)) ids.push_back(key_ids_[i]);
            }
    }
    for (const bool hp : { true, false })
        for (const BoxDecision& d : decisions)
            if (d.hp == hp && std::find(ids.begin(), ids.end(), d.key) == ids.end()) ids.push_back(d.key);

    std::vector<std::string> lines;
    if (vitals.empty() && decisions.empty()) return lines;
    const std::uint64_t start = now > kBlackBoxMs ? now - kBlackBoxMs : 0;
    const auto ago = [&](std::uint64_t t) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "-%.1f s", static_cast<double>(now - std::min(now, std::max(t, start))) / 1000.0);
        return std::string(buf);
    };
    const auto seconds = [](std::uint64_t ms) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.1f s", static_cast<double>(ms) / 1000.0);
        return std::string(buf);
    };
    // Entries joined, the newest kept when they do not all fit.
    const auto fit = [](const std::string& head, const std::vector<std::string>& parts) {
        std::size_t from = 0, len = head.size();
        for (const std::string& p : parts) len += p.size() + 2;
        while (len > kBlackBoxLineChars && from < parts.size()) len -= parts[from++].size() + 2;
        std::string line = head + (from > 0 ? "..., " : "");
        for (std::size_t i = from; i < parts.size(); ++i) line += (i > from ? ", " : "") + parts[i];
        if (line.size() > kBlackBoxLineChars) line.resize(kBlackBoxLineChars);
        return line;
    };

    // ---- HP and MP as read ----
    {
        const auto point = [&](const BoxVitals& v) {
            const auto pct = [](int p) { return p < 0 ? std::string("?") : std::to_string(p); };
            return ago(v.t) + " " + (v.hp < 0 && v.mp < 0 ? std::string("no read") : pct(v.hp) + "/" + pct(v.mp));
        };
        std::vector<std::string> parts;
        // What was read when the window opened, then the lowest HP of each
        // 2 s up to the last 5 s -- no reading lowest of all -- then every
        // change.
        const BoxVitals* before = nullptr;
        for (const BoxVitals& v : vitals)
            if (v.t < start) before = &v;
        if (before) parts.push_back(point(*before));
        const std::uint64_t close = now > 5000 ? now - 5000 : 0;
        const BoxVitals* low = nullptr;
        std::uint64_t bucket = 0;
        const auto lower = [](const BoxVitals& a, const BoxVitals& b) { return a.hp < b.hp; };
        for (const BoxVitals& v : vitals) {
            if (v.t < start) continue;
            if (v.t >= close) {
                if (low) parts.push_back(point(*low));
                low = nullptr;
                parts.push_back(point(v));
                continue;
            }
            const std::uint64_t b = (v.t - start) / 2000;
            if (low && b != bucket) {
                parts.push_back(point(*low));
                low = nullptr;
            }
            if (!low || lower(v, *low)) low = &v;
            bucket = b;
        }
        if (low) parts.push_back(point(*low));
        if (parts.empty()) parts.push_back("none pushed");
        lines.push_back(fit("Black box, the last 30 s: HP/MP read (%): ", parts));
    }

    // ---- each potion key ----
    std::string presses_line;
    for (const std::uint64_t id : ids) {
        if (lines.size() + 2 > kBlackBoxLines) break;      // room for the presses' line
        const BoxDecision* before = nullptr;
        std::vector<const BoxDecision*> mine;
        for (const BoxDecision& d : decisions) {
            if (d.key != id) continue;
            if (d.t < start) before = &d;
            else mine.push_back(&d);
        }
        if (before) mine.insert(mine.begin(), before);
        if (mine.empty()) continue;
        const BoxDecision& k = *mine.back();
        const std::string name = InputManager::key_name(k.keycode);
        std::vector<std::string> parts;
        int pressed = 0;
        for (std::size_t i = 0; i < mine.size(); ++i) {
            const BoxDecision& d = *mine[i];
            if (d.why == PotionWhy::Pressed) {
                if (d.t >= start) ++pressed;
                parts.push_back(ago(d.t) + " PRESSED");
                continue;
            }
            const std::uint64_t from = std::max(d.t, start);
            const std::uint64_t until = i + 1 < mine.size() ? mine[i + 1]->t : now;
            parts.push_back(ago(d.t) + " " + potion_why_name(d.why) + (d.text.empty() ? "" : " (" + d.text + ")") +
                            " " + seconds(until > from ? until - from : 0));
        }
        lines.push_back(fit(std::string(k.hp ? "HP" : "MP") + " potion key " + name + " (under " +
                                std::to_string(k.mark) + "%): ",
                            parts));
        presses_line += (presses_line.empty() ? "" : ", ") + std::string(k.hp ? "HP" : "MP") + " key " + name + " " +
                        std::to_string(pressed);
    }
    if (!presses_line.empty()) lines.push_back(fit("Potion presses in those 30 s: ", { presses_line }));
    return lines;
}

void MacroEngine::log_potions(std::uint64_t now) {
    std::string line;
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(box_mutex_);
        if (potion_log_ms_ == 0) {
            potion_log_ms_ = std::max<std::uint64_t>(now, 1);
            return;
        }
        if (now < potion_log_ms_ + 60'000) return;
        potion_log_ms_ = now;
        // HP's keys first, each in the order it was first seen.
        for (const bool hp : { true, false })
            for (auto& [id, m] : potion_minute_) {
                if (m.hp != hp) continue;
                m.ms[static_cast<int>(m.why)] += now > m.since ? now - m.since : 0;
                m.since = now;
                // A press's moment is its cooldown's first.
                m.ms[static_cast<int>(PotionWhy::Cooling)] += m.ms[static_cast<int>(PotionWhy::Pressed)];
                m.ms[static_cast<int>(PotionWhy::Pressed)] = 0;
                any = any || m.active;
                std::string part = std::string(hp ? "HP" : "MP") + " key " + InputManager::key_name(m.keycode) +
                                   " (under " + std::to_string(m.mark) + "%): " + std::to_string(m.presses) +
                                   " pressed";
                std::string held;
                for (int w = 0; w <= static_cast<int>(PotionWhy::BehindHp); ++w) {
                    if (m.ms[w] < 50) continue;
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%.1f s ", static_cast<double>(m.ms[w]) / 1000.0);
                    held += (held.empty() ? "" : ", ") + std::string(buf) +
                            potion_why_name(static_cast<PotionWhy>(w)) +
                            (static_cast<PotionWhy>(w) == PotionWhy::KeysHeld && !m.held_by.empty()
                                 ? " (" + m.held_by + ")"
                                 : "");
                }
                if (!held.empty()) part += "; " + held;
                line += (line.empty() ? "" : " | ") + part;
                // The next minute starts from here, in the reason it is in.
                m.presses = 0;
                std::fill(std::begin(m.ms), std::end(m.ms), 0);
                m.active = m.why != PotionWhy::AboveMark;
                m.held_by = m.why == PotionWhy::KeysHeld ? m.text : std::string();
            }
    }
    if (any && !line.empty()) GB_INFO("Potions: %s", line.c_str());
}

void MacroEngine::current_vitals(float& hp_pct, float& mp_pct) const {
    const std::uint64_t at = vit_at_ms_.load();
    if (at == 0 || clock_now() - at > kVitalsStaleMs) {
        hp_pct = mp_pct = -1.0f;
        return;
    }
    hp_pct = vit_hp_.load();
    mp_pct = vit_mp_.load();
}

MacroEngine::Gate MacroEngine::check_gate(const MacroKey& k, float pct) {
    if (!k.conditional()) return Gate::Pass;

    // No reading means hold, not fire. A potion key whose bar was never
    // calibrated should do nothing visible rather than drink on every tick --
    // and the held_no_reading counter is what tells the user which it is.
    if (pct < 0.0f) return Gate::NoReading;

    return pct <= static_cast<float>(k.threshold_pct) ? Gate::Pass : Gate::Blocked;
}

MacroEngine::Stats MacroEngine::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void MacroEngine::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Stats{};
}

std::uint32_t MacroEngine::jittered(std::uint32_t base, int pct) const {
    int total;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        total = pct + config_.global_jitter_pct;
    }
    return jitter(base, total);
}

void MacroEngine::arm(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    due_ms_.assign(config_.keys.size(), 0);
    pressed_ms_.assign(config_.keys.size(), 0);
    // Every key at its own start delay -- except that the buffs keep
    // kBuffGapMs apart, in the order of the keys: the game drops a cast that
    // comes right on top of another, and the simple screen staggers its keys
    // by only 150 ms. And a potion at none: its due time is its cooldown, and
    // one under its mark at BẮT ĐẦU drinks on the first look, before any
    // skill or buff. The simple screen gave the potions a start delay too --
    // 900 ms and more, in a player's file -- and every skill went first.
    bool buff_before = false;
    std::uint64_t last_buff = 0;
    for (std::size_t i = 0; i < config_.keys.size(); ++i) {
        const MacroKey& k = config_.keys[i];
        std::uint64_t due = k.conditional() ? now : now + k.start_delay_ms;
        if (k.enabled && k.keycode != 0 && macro_key_kind(k) == MacroKeyKind::Buff) {
            if (buff_before && due < last_buff + kBuffGapMs) due = last_buff + kBuffGapMs;
            last_buff = due;
            buff_before = true;
        }
        due_ms_[i] = due;
    }
    // A start is not a re-cast. Counted as one, it swallowed the re-cast
    // after a death and revive in the first half minute of a run -- and the
    // buffs a start sends at a death panel, a bot started on a dead
    // character, are no buffs at all. Nothing asks for one at a start anyway:
    // a cold start's login comes before the macro is running.
    recast_once_ = false;
    recast_pending_ = false;
    buff_pressed_ = false;
    // The potions are looked at on the first pass. Until then the sequence,
    // whose thread starts with the timers', waits: a potion under its mark
    // is due at once, and the sequence's first step went before it -- the
    // timer thread's first pass comes 10 ms in. The first look clears this
    // when nothing is due, and with no potion to look at there is no wait.
    for (PotionKind& pk : potion_kind_) pk = PotionKind{};
    potion_count_ms_ = 0;
    hp_up_ms_ = 0;
    bool potions = false;
    for (const MacroKey& k : config_.keys)
        if (k.conditional() && k.enabled && k.keycode != 0 && k.interval_ms != 0) potions = true;
    potion_since_provisional_ = potions && config_.timers_enabled;
    potion_waiting_since_.store(potion_since_provisional_ ? std::max<std::uint64_t>(now, 1) : 0);
    // So are the skills that follow the fight; their minute starts on the
    // first pass.
    skill_look_ms_ = 0;
    skill_log_ms_ = 0;
}

void MacroEngine::start() {
    if (running_.exchange(true)) return;

    if (timer_thread_.joinable()) timer_thread_.join();
    if (seq_thread_.joinable())   seq_thread_.join();

    stepped_ = false;
    arm(clock_now());
    seq_pos_.store(0);
    seq_next_ms_ = 0;
    seq_noticed_ = 0;
    seq_wrapped_ = false;
    {
        // The black box goes on across runs; each key's reasons and minute
        // start afresh.
        std::lock_guard<std::mutex> lock(box_mutex_);
        box_last_.clear();
        potion_minute_.clear();
        potion_log_ms_ = 0;
    }

    timer_thread_ = std::thread(&MacroEngine::timer_loop, this);
    seq_thread_   = std::thread(&MacroEngine::sequence_loop_fn, this);

    GB_INFO("=== MACRO ON ===");
}

void MacroEngine::start_stepped() {
    if (running_.exchange(true)) return;

    if (timer_thread_.joinable()) timer_thread_.join();
    if (seq_thread_.joinable())   seq_thread_.join();

    stepped_ = true;
    arm(clock_now());
    seq_pos_.store(0);
    seq_next_ms_ = 0;
    seq_noticed_ = 0;
    seq_wrapped_ = false;
    {
        std::lock_guard<std::mutex> lock(box_mutex_);
        box_last_.clear();
        potion_minute_.clear();
        potion_log_ms_ = 0;
    }
}

void MacroEngine::step() {
    if (!running_.load() || !stepped_.load()) return;
    run_timers(clock_now());
    // The sequence's turn, when its wait is over by the clock: what its
    // thread does between its sleeps.
    const std::uint64_t now = clock_now();
    if (now >= seq_next_ms_) seq_next_ms_ = now + sequence_turn(now);
}

void MacroEngine::step_timers() {
    if (!running_.load() || !stepped_.load()) return;
    run_timers(clock_now());
}

void MacroEngine::step_sequence() {
    if (!running_.load() || !stepped_.load()) return;
    const std::uint64_t now = clock_now();
    if (now < seq_next_ms_) return;
    // Its wait from the end of the turn, as the thread sleeps it after the
    // turn: a step's press may take some of the clock.
    const std::uint32_t wait = sequence_turn(now);
    seq_next_ms_ = clock_now() + wait;
}

void MacroEngine::stop() {
    if (!running_.exchange(false)) return;
    stepped_ = false;

    if (timer_thread_.joinable()) timer_thread_.join();
    if (seq_thread_.joinable())   seq_thread_.join();

    // A key left held keeps repeating in the game long after the macro has
    // "stopped", which is the single most alarming way for this to misbehave.
    input_.release_all();

    GB_INFO("=== MACRO OFF ===");
}

void MacroEngine::set_suspended(bool suspended, const std::string& why) {
    const bool was = suspended_.exchange(suspended);
    std::uint64_t out_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        suspended_why_ = suspended ? why : std::string{};
        if (suspended && !was) suspended_at_ms_ = clock_now();
        if (!suspended && was) out_ms = clock_now() - suspended_at_ms_;
    }
    if (was == suspended) return;
    // Out of the world, up to this moment: a re-cast asked for from here on
    // is for this absence, not one more ask about the last return.
    down_ms_.store(clock_now());

    if (suspended) {
        // Let go of anything being held. A direction key held across a
        // disconnect is a character that runs into a wall for an hour the
        // moment it comes back.
        input_.release_all();
        GB_INFO("Macro suspended: %s", why.empty() ? "not in the world" : why.c_str());
    } else {
        GB_INFO("Macro resumed");
        // Long enough out of the world to have died, been revived or logged
        // back in -- each of which takes the buffs off the character. A map
        // change is a few seconds and leaves them on. Or a re-cast was asked
        // for while it was out, and is done now that it can be.
        bool pending = false;
        std::string asked;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending = recast_pending_;
            asked = recast_why_;
            recast_pending_ = false;
        }
        if (pending)
            apply_recast(asked);
        else if (out_ms >= kRecastAfterSuspendMs)
            apply_recast(std::to_string(out_ms / 1000) + " s out of the world");
    }
}

bool MacroEngine::recast_buffs(const std::string& why) {
    if (!running_.load()) return false;
    // Out of the world nothing can be cast, and a buff due now would only be
    // tried and tried again: re-cast the moment the character is back.
    if (suspended_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recast_pending_)
            GB_INFO("Buffs: re-cast after %s once the character is back in the world",
                    why.c_str());
        recast_pending_ = true;
        recast_why_ = why;
        return true;
    }
    return apply_recast(why);
}

bool MacroEngine::apply_recast(const std::string& why) {
    if (!running_.load()) return false;
    const std::uint64_t now = clock_now();
    std::size_t n = 0;
    bool merged = false;
    std::uint64_t ago = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // One more ask about the return just re-cast for -- the revive, HP
        // back from 0 and the HUD back all ask within a second or two -- is
        // nothing to do. Only that, though: a few seconds at most, and only
        // with the character not seen down since. The 30 s this once was
        // swallowed the re-cast after a second death soon after a revive,
        // and with it the buffs for up to a whole interval.
        if (recast_once_ && down_ms_.load() <= recast_ms_ && now - recast_ms_ < kRecastMergeMs) {
            merged = true;
            ago = now - recast_ms_;
        } else {
            if (due_ms_.size() != config_.keys.size()) due_ms_.resize(config_.keys.size(), now);
            for (std::size_t i = 0; i < config_.keys.size(); ++i) {
                const MacroKey& k = config_.keys[i];
                if (!k.enabled || k.keycode == 0 || k.interval_ms == 0 ||
                    macro_key_kind(k) != MacroKeyKind::Buff)
                    continue;
                // One after another, give or take a tenth of the gap: never
                // closer than kBuffGapMs, and never on the same beat.
                const long long spread =
                    static_cast<long long>(kRecastGapMs) * random_percent_offset(10) / 100;
                due_ms_[i] = static_cast<std::uint64_t>(
                    static_cast<long long>(now + kRecastFirstMs + n * kRecastGapMs) + spread);
                ++n;
            }
            if (n > 0) {
                recast_ms_ = now;
                recast_once_ = true;
            }
        }
    }
    if (merged) {
        GB_INFO("Buffs: no second re-cast for %s -- they were re-armed %.1f s ago for the same "
                "return",
                why.c_str(), static_cast<double>(ago) / 1000.0);
        return false;
    }
    if (n == 0) return false;
    GB_INFO("Buffs: re-casting %zu key(s) after %s", n, why.c_str());
    return true;
}

std::string MacroEngine::suspended_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return suspended_why_;
}

bool MacroEngine::press(std::uint32_t keycode, std::uint32_t hold_ms, const char* owner,
                        Refused* why) {
    Refused dummy;
    Refused& out = why ? *why : dummy;
    out = Refused::Failed;
    if (keycode == 0) return false;

    // Nothing is pressed while the character is not in the world. Checked here
    // rather than in each caller, so there is one place for it to be got
    // wrong, and counted separately so that "the macro did nothing for an
    // hour" has an answer in the statistics instead of needing the log.
    if (suspended_.load()) {
        out = Refused::Suspended;
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.held_suspended;
        return false;
    }

    bool need_focus;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        need_focus = config_.require_focus;
    }

    const bool focused = hands_.focused ? hands_.focused() : input_.window_focused();
    if (need_focus && !focused) {
        out = Refused::Unfocused;
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.skipped_unfocused;
        return false;
    }

    // One key at a time. Two timers coming due together would otherwise
    // interleave key-down events and the game would see neither cleanly.
    auto token = coord_.acquire(owner, Priority::Combat);
    if (!token) {
        // Held for a dialog or a revive, or only another key being pressed
        // this moment: a potion waits out the first and tries again at once
        // after the second.
        out = coord_.critical_active() ? Refused::Held : Refused::Busy;
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.skipped_busy;
        return false;
    }

    const int hold = hold_ms ? static_cast<int>(hold_ms) : -1;
    bool ok = hands_.key ? hands_.key(keycode, hold) : input_.key_press(keycode, hold);
    // The key went down, and the game lost focus before it came up -- the
    // player clicking another window in the moment it was held: the key-up
    // was refused, and the key is still down, in the game and in Windows,
    // where it would go on repeating. Let go of it -- release_all sends the
    // key-up whatever has focus -- and count the press as gone out: the game
    // had the key, and a potion pressed again for it would be a second drink.
    const bool left_down = !ok && (hands_.held ? hands_.held(keycode)
                                               : !hands_.key && input_.is_held(keycode));
    if (left_down) {
        if (hands_.release_all) hands_.release_all();
        else                    input_.release_all();
        GB_INFO("Macro: the game lost focus while %s was down; let go of it",
                InputManager::key_name(keycode).c_str());
        ok = true;
    }
    if (ok) {
        out = Refused::None;
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.presses;
    }
    return ok;
}

bool MacroEngine::still_refused(const MacroConfig& cfg, Refused why) const {
    switch (why) {
        case Refused::Held:      return coord_.critical_active();
        case Refused::Suspended: return suspended_.load();
        case Refused::Unfocused:
            return cfg.require_focus && !(hands_.focused ? hands_.focused() : input_.window_focused());
        case Refused::None:
        case Refused::Busy:
        case Refused::Failed:    break;
    }
    return false;
}

void MacroEngine::pause_ms(std::uint32_t ms) {
    if (ms == 0) return;
    if (clock_.sleep_ms) clock_.sleep_ms(ms);
    else                 std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool MacroEngine::potion_first(std::uint64_t now) const {
    const std::uint64_t since = potion_waiting_since_.load();
    return since != 0 && now < since + kPotionFirstMs;
}

bool MacroEngine::potion_due(std::uint64_t now) const {
    float hp, mp;
    current_vitals(hp, mp);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.timers_enabled) return false;
    // The tier the timer pass would press, by the same rule, and whether its
    // cooldown is over.
    for (const MacroKeyKind kind : { MacroKeyKind::HpPotion, MacroKeyKind::MpPotion }) {
        const int i = macro_urgent_potion(config_.keys, kind, kind == MacroKeyKind::HpPotion ? hp : mp);
        if (i >= 0 && static_cast<std::size_t>(i) < due_ms_.size() &&
            now >= due_ms_[static_cast<std::size_t>(i)])
            return true;
    }
    return false;
}

void MacroEngine::run_potions(const MacroConfig& cfg, std::uint64_t now) {
    // The held counters count once a kGatePollMs, as when the gates were
    // looked at only that often, though now they are looked at every pass.
    const bool count = now >= potion_count_ms_;
    if (count) potion_count_ms_ = now + kGatePollMs;

    bool restart = false;     // refused as every key is: the wait begins again
    for (std::size_t n = 0; n < 2; ++n) {
        const MacroKeyKind kind = n == 0 ? MacroKeyKind::HpPotion : MacroKeyKind::MpPotion;
        PotionKind& pk = potion_kind_[n];
        // Looked at on every pass -- a new reading comes with each, and GoC
        // looks at every HP update -- so a potion goes the pass its reading,
        // its cooldown's end or the end of a hold comes, before any skill of
        // that pass: looked at only every 100 ms, a skill due in between went
        // first. But not for kGatePollMs after one of its kind went out --
        // GoC's 100 ms -- nor for kPotionRetryMs after another job kept the
        // keys. One refused for a hold, the game not in front or the
        // character out of the world is tried the moment that is over, and
        // again at the beat meanwhile, which keeps the counters as they were.
        const bool over = refuses_all(pk.refused) && !still_refused(cfg, pk.refused);
        if (now < pk.look_ms && !over) continue;
        pk.look_ms = 0;
        pk.refused = Refused::None;

        // Which of this kind's keys want to drink, and which of them may: the
        // one with the lowest threshold, the first on a tie -- all judged on
        // one reading. Each key's cooldown is found by which key it is: the
        // keys may have changed since this pass took its copy of them.
        float hp_now, mp_now;
        current_vitals(hp_now, mp_now);
        const bool hp_kind = kind == MacroKeyKind::HpPotion;
        const float v = hp_kind ? hp_now : mp_now;
        const int chosen = macro_urgent_potion(cfg.keys, kind, v);
        const auto id_of = [&](std::size_t i) { return i < pass_ids_.size() ? pass_ids_[i] : 0; };
        std::uint64_t held_condition = 0, held_no_reading = 0, held_tier = 0;
        bool ready = false;
        for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
            const MacroKey& k = cfg.keys[i];
            if (macro_key_kind(k) != kind || !k.enabled || k.keycode == 0 || k.interval_ms == 0)
                continue;
            bool cooled;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const int s = slot(i);
                cooled = s >= 0 && static_cast<std::size_t>(s) < due_ms_.size() &&
                         now >= due_ms_[static_cast<std::size_t>(s)];
            }
            const Gate g = check_gate(k, v);
            // Counted as before: only for a key that would otherwise have
            // been free to drink.
            if (g == Gate::NoReading)              { if (cooled) ++held_no_reading; }
            else if (g == Gate::Blocked)           { if (cooled) ++held_condition; }
            else if (static_cast<int>(i) == chosen) ready = cooled;
            else if (cooled)                        ++held_tier;
            // The black box: why this key does not drink on this look. The
            // one that may has its press's outcome recorded below.
            if (g == Gate::NoReading)                    box_decide(now, id_of(i), k, hp_kind, PotionWhy::NoReading);
            else if (g == Gate::Blocked)                 box_decide(now, id_of(i), k, hp_kind, PotionWhy::AboveMark);
            else if (static_cast<int>(i) != chosen)      box_decide(now, id_of(i), k, hp_kind, PotionWhy::OtherTier);
            else if (!cooled)                            box_decide(now, id_of(i), k, hp_kind, PotionWhy::Cooling);
        }
        if (count) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.held_condition += held_condition;
            stats_.held_no_reading += held_no_reading;
            stats_.held_tier += held_tier;
        }
        // Nothing wants to drink, or the one that may is on its cooldown --
        // and the less urgent ones wait with it.
        if (!ready) {
            pk.waiting = false;
            continue;
        }

        // Due: nothing else of the macro starts from here until it has gone,
        // the sequence on its own thread included. Set at the end of the
        // pass only, the wait came too late for the gap between the HP
        // potion and the MP one: a step of the sequence went in it, and the
        // MP potion found the keys held.
        if (potion_waiting_since_.load() == 0) potion_waiting_since_.store(std::max<std::uint64_t>(now, 1));

        // The MP potion after the HP one: never while the HP one waits only
        // for the keys, and no sooner than kPotionGapMs after its key came up.
        if (kind == MacroKeyKind::MpPotion) {
            const PotionKind& hp = potion_kind_[0];
            if (hp.waiting && hp.refused == Refused::Busy) {
                pk.waiting = true;
                box_decide(now, id_of(static_cast<std::size_t>(chosen)), cfg.keys[static_cast<std::size_t>(chosen)],
                           false, PotionWhy::BehindHp);
                continue;
            }
            const std::uint64_t t = clock_now();
            if (hp_up_ms_ != 0 && t < hp_up_ms_ + kPotionGapMs)
                pause_ms(static_cast<std::uint32_t>(hp_up_ms_ + kPotionGapMs - t));
        }

        const std::size_t i = static_cast<std::size_t>(chosen);
        const MacroKey& k = cfg.keys[i];
        const std::uint64_t at = clock_now();
        Refused why = Refused::None;
        const bool went = press(k.keycode, k.hold_ms, "macro-potion", &why);
        switch (went ? Refused::None : why) {
            case Refused::None:      box_decide(at, id_of(i), k, hp_kind, went ? PotionWhy::Pressed : PotionWhy::KeyFailed); break;
            case Refused::Suspended: box_decide(at, id_of(i), k, hp_kind, PotionWhy::NotInWorld); break;
            case Refused::Unfocused: box_decide(at, id_of(i), k, hp_kind, PotionWhy::NotInFront); break;
            case Refused::Held:      box_decide(at, id_of(i), k, hp_kind, PotionWhy::KeysHeld, coord_.critical_reason()); break;
            case Refused::Busy:      box_decide(at, id_of(i), k, hp_kind, PotionWhy::KeysBusy); break;
            case Refused::Failed:    box_decide(at, id_of(i), k, hp_kind, PotionWhy::KeyFailed); break;
        }
        if (went) {
            if (kind == MacroKeyKind::HpPotion) hp_up_ms_ = clock_now();
            pk.waiting = false;
            pk.look_ms = at + kGatePollMs;
            // The cooldown counts from the press that went out, and is the
            // key's: every potion on this keycode -- Lv1 and Lv2 put on one
            // key -- starts its own now, each its own interval. Otherwise HP
            // read between their two marks after a drink chose the other
            // tier, whose cooldown had not started, and the key went again
            // 200 ms later. Among the keys as they are now, not as this pass
            // found them: one taken out, put in or moved during the press
            // has its own cooldown set, and no other key's.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (std::size_t s = 0; s < config_.keys.size(); ++s) {
                    const MacroKey& o = config_.keys[s];
                    if (!o.conditional() || o.keycode != k.keycode) continue;
                    if (s < due_ms_.size())
                        due_ms_[s] = at + jitter(o.interval_ms, o.jitter_pct + config_.global_jitter_pct);
                    if (s < pressed_ms_.size()) pressed_ms_[s] = at;
                }
            }
            count_press(i);
        } else {
            // Not gone out: the cooldown has not started, and the potion is
            // still due. Another job's key is over in a moment, so a potion
            // refused for that alone is tried again kPotionRetryMs on. One
            // refused as every key is -- a hold, the game not in front, the
            // character out of the world -- goes the moment that is over,
            // and that time does not count against kPotionFirstMs: nothing
            // else of the macro can go meanwhile either.
            pk.waiting = true;
            pk.refused = why;
            pk.look_ms = now + (why == Refused::Busy ? kPotionRetryMs : kGatePollMs);
            if (refuses_all(why)) restart = true;
        }
    }

    // Cleared only now, with both kinds done: the MP potion went after the
    // HP one, or nothing waits. The wait set at the start counts from this
    // first look, as one set by any other look would.
    const bool waiting = potion_kind_[0].waiting || potion_kind_[1].waiting;
    if (!waiting)
        potion_waiting_since_.store(0);
    else if (restart || potion_since_provisional_ || potion_waiting_since_.load() == 0)
        potion_waiting_since_.store(std::max<std::uint64_t>(now, 1));
    potion_since_provisional_ = false;

    // The minute's line, on every pass that looks at the potions: a pass that
    // goes no further -- a potion waiting -- still writes it.
    log_potions(clock_now());
}

void MacroEngine::timer_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!running_.load()) break;
        run_timers(clock_now());
    }
}

void MacroEngine::run_timers(std::uint64_t now) {
    // This pass's own copy of the keys, and which key each is: the keys may
    // be changed while it presses one, and from then on each is found by
    // which it is (slot), never by where it stood here.
    MacroConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = config_;
        pass_ids_ = key_ids_;
        if (due_ms_.size() != cfg.keys.size())
            due_ms_.resize(cfg.keys.size(), now);
        if (pressed_ms_.size() != cfg.keys.size())
            pressed_ms_.resize(cfg.keys.size(), 0);
    }
    if (!cfg.timers_enabled) {
        // No potion is pressed with the timers off, so none holds the
        // sequence either.
        potion_waiting_since_.store(0);
        potion_since_provisional_ = false;
        return;
    }

    // The potions first, whatever the order of the keys: a potion that is
    // due goes before any skill or buff (the player's rule, as GoC). While
    // one is due and has not gone out, nothing else starts -- for a moment
    // only, so a potion that cannot be pressed does not hold the rest up.
    if (potions_first(cfg, now)) return;

    // With skills following the fight and a hunter running, the skill keys
    // are run_skills', after the rest. Otherwise every key keeps its own
    // timer, as it always has.
    const Fight fight = cfg.skills_follow_fight ? fight_now() : Fight{};
    const bool follow = following(cfg, fight);

    for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
        const MacroKey& k = cfg.keys[i];
        if (!k.enabled || k.keycode == 0 || k.interval_ms == 0) continue;
        // The potion keys -- every conditional key is one -- are run_potions'.
        if (k.conditional()) continue;
        if (follow && macro_key_kind(k) == MacroKeyKind::Skill && !k.always_timer) continue;

        // Its due time, wherever it stands now. One taken out since the pass
        // began is passed over.
        std::uint64_t due;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const int s = slot(i);
            if (s < 0 || static_cast<std::size_t>(s) >= due_ms_.size()) continue;
            due = due_ms_[static_cast<std::size_t>(s)];
        }
        if (now < due) continue;

        // Never two buffs closer than kBuffGapMs: one that comes due right
        // after another waits its turn. Buffs tried again after a refusal
        // would otherwise go out in a bunch the moment the keys are free, and
        // the game drops all but the first.
        const bool buff = macro_key_kind(k) == MacroKeyKind::Buff;
        if (buff) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (buff_pressed_ && now < last_buff_ms_ + kBuffGapMs) {
                const int s = slot(i);
                if (s >= 0 && static_cast<std::size_t>(s) < due_ms_.size())
                    due_ms_[static_cast<std::size_t>(s)] = last_buff_ms_ + kBuffGapMs;
                continue;
            }
        }

        // A potion that has fallen due while this pass pressed the keys
        // before -- a reading that came in as one was down -- goes first.
        if (potions_first(cfg, clock_now())) return;

        // Schedule the next fire before pressing, so a slow press does not
        // push the whole cadence later and later. Taken out while the
        // potions were seen to: not pressed.
        const std::uint32_t next = jittered(k.interval_ms, k.jitter_pct);
        const std::uint64_t armed = now + next;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const int s = slot(i);
            if (s < 0 || static_cast<std::size_t>(s) >= due_ms_.size()) continue;
            due_ms_[static_cast<std::size_t>(s)] = armed;
        }

        if (press(k.keycode, k.hold_ms, "macro-timer")) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const int s = slot(i);
                if (s >= 0 && static_cast<std::size_t>(s) < pressed_ms_.size())
                    pressed_ms_[static_cast<std::size_t>(s)] = now;
                if (buff) {
                    last_buff_ms_ = now;
                    buff_pressed_ = true;
                }
            }
            count_press(i);
        } else if (buff) {
            // A buff that did not go out -- held while out of the world, the
            // game not in front, the keys held for a dialog -- is tried again
            // in a couple of seconds. Left to its interval, a 1600-s buff
            // refused during a trash pass was gone for 27 minutes. Unless a
            // re-cast has re-armed it meanwhile, which wins.
            const std::uint64_t retry = now + jittered(kBuffRetryMs, k.jitter_pct);
            std::lock_guard<std::mutex> lock(mutex_);
            const int s = slot(i);
            if (s >= 0 && static_cast<std::size_t>(s) < due_ms_.size() &&
                due_ms_[static_cast<std::size_t>(s)] == armed)
                due_ms_[static_cast<std::size_t>(s)] = retry;
        }
    }

    if (follow) run_skills(cfg, clock_now());
    log_skills(clock_now());
}

bool MacroEngine::potions_first(const MacroConfig& cfg, std::uint64_t now) {
    run_potions(cfg, now);
    return potion_first(clock_now());
}

MacroEngine::Fight MacroEngine::fight_now() const {
    return fight_ ? fight_() : Fight{};
}

bool MacroEngine::following(const MacroConfig& cfg, const Fight& fight) {
    return cfg.skills_follow_fight && fight.mode != Fight::Mode::NoHunter;
}

MacroEngine::SkillGate MacroEngine::skill_gate(int mp_floor_pct, const Fight& fight) const {
    if (fight.mode != Fight::Mode::Fighting) return SkillGate::NotFighting;
    float hp, mp;
    current_vitals(hp, mp);
    // HP read at nothing is the character down, at the death panel: no use
    // for a skill. (A potion still drinks then -- an alive character can
    // read that low too.)
    if (hp >= 0.0f && hp < kDownHpPct) return SkillGate::Dead;
    // MP under the floor, read just now: the skills wait and the hunter's
    // own swing, which costs none, carries on. No reading is no hold.
    if (mp_floor_pct > 0 && mp >= 0.0f && mp < static_cast<float>(mp_floor_pct))
        return SkillGate::LowMp;
    return SkillGate::Pass;
}

void MacroEngine::count_held(SkillGate g, std::uint64_t n) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    switch (g) {
        case SkillGate::NotFighting: stats_.held_not_fighting += n; break;
        case SkillGate::LowMp:       stats_.held_low_mp += n; break;
        case SkillGate::Dead:        stats_.held_dead += n; break;
        case SkillGate::Pass:        break;
    }
}

void MacroEngine::run_skills(const MacroConfig& cfg, std::uint64_t now) {
    if (now < skill_look_ms_) return;

    // A potion that has fallen due meanwhile goes first; the skills wait,
    // ready, and are looked at again on the next pass.
    if (potions_first(cfg, clock_now())) return;

    // The fight as it is now, not as it was at the start of the pass: the
    // buffs, timed keys and potions pressed since held the thread for their
    // presses, and the hunter may have begun a step back meanwhile -- a skill
    // thrown then goes out over the step, at nothing. Nothing that waits
    // comes between this and the press. Hunting stopped meanwhile: the
    // skills are the timer loop's again, from the next pass.
    const Fight fight = fight_now();
    if (!following(cfg, fight)) return;

    // The keys ready to go: skills on, not on their own timers, cooldown
    // over -- and not the hunter's own attack key, which it presses itself.
    // Each found by which key it is, the keys being free to change since the
    // pass began.
    std::vector<std::size_t> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
            const MacroKey& k = cfg.keys[i];
            if (!k.enabled || k.keycode == 0 || k.interval_ms == 0 || k.always_timer ||
                macro_key_kind(k) != MacroKeyKind::Skill)
                continue;
            if (fight.attack_key != 0 && k.keycode == fight.attack_key) continue;
            const int s = slot(i);
            if (s >= 0 && static_cast<std::size_t>(s) < due_ms_.size() &&
                now >= due_ms_[static_cast<std::size_t>(s)])
                ready.push_back(i);
        }
    }
    if (ready.empty()) return;

    // No fight, MP too low, the character down: they wait, ready, keeping
    // their turn, and are looked at again in a moment.
    const SkillGate gate = skill_gate(cfg.skill_mp_floor_pct, fight);
    if (gate != SkillGate::Pass) {
        count_held(gate, ready.size());
        skill_look_ms_ = now + kGatePollMs;
        return;
    }

    // The first in the order of the keys that reaches the target.
    std::size_t pick = cfg.keys.size();
    std::uint64_t too_far = 0;
    for (const std::size_t i : ready) {
        const int range = cfg.keys[i].range_px;
        if (range > 0 && fight.dx > range) {
            ++too_far;
            continue;
        }
        pick = i;
        break;
    }
    if (too_far > 0) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.held_range += too_far;
    }
    if (pick == cfg.keys.size()) {
        skill_look_ms_ = now + kGatePollMs;
        return;
    }

    const MacroKey& k = cfg.keys[pick];
    const std::uint64_t at = clock_now();
    if (press(k.keycode, k.hold_ms, "macro-skill")) {
        const std::uint64_t up = clock_now();
        // Its own cooldown, from this press. Worked out before the lock:
        // jittered() takes it too.
        const std::uint32_t cooldown = jittered(k.interval_ms, k.jitter_pct);
        {
            // On the key as it stands now: taken out during the press, on no
            // key at all.
            std::lock_guard<std::mutex> lock(mutex_);
            const int s = slot(pick);
            if (s >= 0 && static_cast<std::size_t>(s) < due_ms_.size())
                due_ms_[static_cast<std::size_t>(s)] = at + cooldown;
            if (s >= 0 && static_cast<std::size_t>(s) < pressed_ms_.size())
                pressed_ms_[static_cast<std::size_t>(s)] = at;
        }
        count_press(pick);
        GB_DEBUG("Skills: %s, the target %d px to the side", InputManager::key_name(k.keycode).c_str(),
                 fight.dx);
        // Sequential: the next one ready goes skill_gap_ms after this one's
        // key came up -- the rest of a burst, in the order of the keys. One
        // at a time otherwise: the next chance kSkillChanceMs on.
        skill_look_ms_ = cfg.sequential_skills ? up + cfg.skill_gap_ms : at + kSkillChanceMs;
    } else {
        // Not gone out -- another job's key, a hold, the game not in front:
        // still ready, its cooldown not started and its turn not lost.
        skill_look_ms_ = now + kSkillRetryMs;
    }
}

void MacroEngine::log_skills(std::uint64_t now) {
    if (skill_log_ms_ != 0 && now - skill_log_ms_ < 60'000) return;
    // The tallies and the keys they belong to, read together: each tally
    // follows its key when the keys change.
    Stats s;
    std::uint64_t pressed = 0;
    int mp_floor = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        s = stats_;
        mp_floor = config_.skill_mp_floor_pct;
        pressed = s.sequence_presses;
        for (std::size_t i = 0; i < config_.keys.size() && i < s.key_presses.size(); ++i)
            if (macro_key_kind(config_.keys[i]) == MacroKeyKind::Skill) pressed += s.key_presses[i];
    }
    if (skill_log_ms_ != 0) {
        // Since the last line. Counts set back meanwhile count from nothing.
        const auto since = [](std::uint64_t now_n, std::uint64_t then_n) {
            return now_n >= then_n ? now_n - then_n : now_n;
        };
        const std::uint64_t p = since(pressed, skill_log_pressed_);
        const std::uint64_t idle = since(s.held_not_fighting, skill_log_held_.held_not_fighting);
        const std::uint64_t low = since(s.held_low_mp, skill_log_held_.held_low_mp);
        const std::uint64_t dead = since(s.held_dead, skill_log_held_.held_dead);
        const std::uint64_t range = since(s.held_range, skill_log_held_.held_range);
        if (p || idle || low || dead || range)
            GB_INFO("Skills: %llu pressed; held %llu not fighting, %llu MP under %d%%, %llu dead, "
                    "%llu range",
                    static_cast<unsigned long long>(p), static_cast<unsigned long long>(idle),
                    static_cast<unsigned long long>(low), mp_floor,
                    static_cast<unsigned long long>(dead), static_cast<unsigned long long>(range));
    }
    skill_log_ms_ = std::max<std::uint64_t>(now, 1);
    skill_log_pressed_ = pressed;
    skill_log_held_ = s;
}

void MacroEngine::sequence_loop_fn() {
    while (running_.load()) {
        const std::uint32_t wait = sequence_turn(clock_now());
        // Sleep in slices so stopping is responsive even behind a long delay.
        for (std::uint32_t slept = 0; slept < wait && running_.load(); slept += 20)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::uint32_t>(20, wait - slept)));
    }
}

std::uint32_t MacroEngine::sequence_turn(std::uint64_t now) {
    MacroConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = config_;
    }

    // Off, or switched off while a step waited: nothing of it goes, and a
    // potion waited for then is waited for afresh once it is on again.
    if (!cfg.sequence_enabled || cfg.sequence.empty()) {
        seq_noticed_ = 0;
        return 50;
    }

    // Round from the last step to the first: counted once, however long that
    // first step then waits before it goes.
    std::size_t pos = seq_pos_.load();
    if (seq_wrapped_) {
        pos = 0;
    } else if (pos >= cfg.sequence.size()) {
        if (!cfg.sequence_loop) return 50;
        pos = 0;
        seq_wrapped_ = true;
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.sequence_loops;
    }

    const MacroStep step = cfg.sequence[pos];

    // A potion that is due goes first: the step waits where it is, for a
    // moment at most (see kPotionFirstMs), rather than being pressed over it
    // or skipped -- looked at again 10 ms on.
    //
    // With skills following the fight and a hunter running, a step is one of
    // the skills: it waits where it is too -- the sequence does not move on --
    // while there is no fight, MP is under the floor or the character is
    // down, looked at again every 50 ms. A step on the hunter's own attack
    // key is its swing, not the sequence's to press: passed over, with its
    // pause kept.
    bool pass_over = false;
    if (step.keycode != 0) {
        if (potion_first(now)) return 10;
        // A potion due that the timer pass has not taken up yet -- a reading
        // come in a moment ago, a cooldown just over: the pass comes within
        // 10 ms, and a step pressed meanwhile held the keys while the potion
        // was tried. It is waited for, kPotionNoticeMs at most, should the
        // pass not take it up after all: looked at again every 2 ms.
        if (potion_waiting_since_.load() == 0 && potion_due(now)) {
            if (seq_noticed_ == 0) seq_noticed_ = std::max<std::uint64_t>(now, 1);
            if (now - seq_noticed_ < kPotionNoticeMs) return 2;
        } else {
            seq_noticed_ = 0;
        }
        const Fight fight = cfg.skills_follow_fight ? fight_now() : Fight{};
        if (cfg.skills_follow_fight && fight.mode != Fight::Mode::NoHunter) {
            if (fight.attack_key != 0 && step.keycode == fight.attack_key) {
                pass_over = true;
            } else {
                const SkillGate gate = skill_gate(cfg.skill_mp_floor_pct, fight);
                if (gate != SkillGate::Pass) {
                    count_held(gate, 1);
                    return 50;
                }
            }
        }
    }
    if (!running_.load()) return 0;

    seq_noticed_ = 0;
    seq_wrapped_ = false;
    seq_pos_.store(pos + 1);

    if (step.keycode != 0 && !pass_over && press(step.keycode, step.hold_ms, "macro-sequence")) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.sequence_presses;
    }
    return jittered(step.delay_after_ms, 8);
}

std::vector<std::uint64_t> MacroEngine::next_due_ms() const {
    std::vector<std::uint64_t> out;
    if (!running_.load()) return out;

    const std::uint64_t now = clock_now();
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(due_ms_.size());
    for (std::uint64_t d : due_ms_) out.push_back(d > now ? d - now : 0);
    return out;
}

} // namespace gobot
