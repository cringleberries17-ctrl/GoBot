#include "gobot/macro.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
}

MacroEngine::~MacroEngine() { stop(); }

void MacroEngine::set_config(const MacroConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A timed key made shorter while the macro runs is due at its last press
    // plus the new interval. Left alone, the time armed at that press stood:
    // a buff turned down from 1600 s to 60 s on the simple screen during a
    // run went out next 26 minutes later. The same key in the same place
    // only; a potion's interval is a cooldown its trigger re-arms anyway.
    if (running_.load()) {
        const std::size_t n = std::min({ config_.keys.size(), cfg.keys.size(), due_ms_.size(),
                                         pressed_ms_.size() });
        for (std::size_t i = 0; i < n; ++i) {
            const MacroKey& was = config_.keys[i];
            const MacroKey& now = cfg.keys[i];
            if (now.keycode != was.keycode || now.conditional() || pressed_ms_[i] == 0 ||
                now.interval_ms >= was.interval_ms)
                continue;
            due_ms_[i] = std::min(due_ms_[i], pressed_ms_[i] + now.interval_ms);
        }
    }
    config_ = cfg;
    if (config_.keys.size() > MacroConfig::kMaxKeys)
        config_.keys.resize(MacroConfig::kMaxKeys);
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

MacroEngine::Gate MacroEngine::check_gate(const MacroKey& k) const {
    if (!k.conditional()) return Gate::Pass;

    float hp, mp;
    current_vitals(hp, mp);

    const float v = (k.trigger == MacroTrigger::HpBelow) ? hp : mp;

    // No reading means hold, not fire. A potion key whose bar was never
    // calibrated should do nothing visible rather than drink on every tick --
    // and the held_no_reading counter is what tells the user which it is.
    if (v < 0.0f) return Gate::NoReading;

    return v <= static_cast<float>(k.threshold_pct) ? Gate::Pass : Gate::Blocked;
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
    if (total <= 0 || base == 0) return base;

    const double scale = 1.0 + random_percent_offset(total) / 100.0;
    const double out = static_cast<double>(base) * scale;
    return static_cast<std::uint32_t>(std::max(1.0, out));
}

void MacroEngine::arm(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    due_ms_.assign(config_.keys.size(), 0);
    pressed_ms_.assign(config_.keys.size(), 0);
    // Every key at its own start delay -- except that the buffs keep
    // kBuffGapMs apart, in the order of the keys: the game drops a cast that
    // comes right on top of another, and the simple screen staggers its keys
    // by only 150 ms.
    bool buff_before = false;
    std::uint64_t last_buff = 0;
    for (std::size_t i = 0; i < config_.keys.size(); ++i) {
        const MacroKey& k = config_.keys[i];
        std::uint64_t due = now + k.start_delay_ms;
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
}

void MacroEngine::start() {
    if (running_.exchange(true)) return;

    if (timer_thread_.joinable()) timer_thread_.join();
    if (seq_thread_.joinable())   seq_thread_.join();

    stepped_ = false;
    arm(clock_now());
    seq_pos_.store(0);

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
}

void MacroEngine::step() {
    if (running_.load() && stepped_.load()) run_timers(clock_now());
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

bool MacroEngine::press(std::uint32_t keycode, std::uint32_t hold_ms, const char* owner) {
    if (keycode == 0) return false;

    // Nothing is pressed while the character is not in the world. Checked here
    // rather than in each caller, so there is one place for it to be got
    // wrong, and counted separately so that "the macro did nothing for an
    // hour" has an answer in the statistics instead of needing the log.
    if (suspended_.load()) {
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
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.skipped_unfocused;
        return false;
    }

    // One key at a time. Two timers coming due together would otherwise
    // interleave key-down events and the game would see neither cleanly.
    auto token = coord_.acquire(owner, Priority::Combat);
    if (!token) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.skipped_busy;
        return false;
    }

    const int hold = hold_ms ? static_cast<int>(hold_ms) : -1;
    const bool ok = hands_.key ? hands_.key(keycode, hold) : input_.key_press(keycode, hold);
    if (ok) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.presses;
    }
    return ok;
}

void MacroEngine::timer_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!running_.load()) break;
        run_timers(clock_now());
    }
}

void MacroEngine::run_timers(std::uint64_t now) {
    MacroConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = config_;
        if (due_ms_.size() != cfg.keys.size())
            due_ms_.resize(cfg.keys.size(), now);
        if (pressed_ms_.size() != cfg.keys.size())
            pressed_ms_.resize(cfg.keys.size(), 0);
    }
    if (!cfg.timers_enabled) return;

    for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
        const MacroKey& k = cfg.keys[i];
        if (!k.enabled || k.keycode == 0 || k.interval_ms == 0) continue;

        std::uint64_t due;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (i >= due_ms_.size()) continue;
            due = due_ms_[i];
        }
        if (now < due) continue;

        // Conditional keys re-arm quickly when the trigger is false, so
        // the interval reads as a cooldown between firings rather than as
        // how long you bleed before the potion arrives.
        if (k.conditional()) {
            const Gate g = check_gate(k);
            if (g != Gate::Pass) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (i < due_ms_.size()) due_ms_[i] = now + kGatePollMs;
                }
                std::lock_guard<std::mutex> lock(stats_mutex_);
                if (g == Gate::NoReading) ++stats_.held_no_reading;
                else                      ++stats_.held_condition;
                continue;
            }
        }

        // Never two buffs closer than kBuffGapMs: one that comes due right
        // after another waits its turn. Buffs tried again after a refusal
        // would otherwise go out in a bunch the moment the keys are free, and
        // the game drops all but the first.
        const bool buff = macro_key_kind(k) == MacroKeyKind::Buff;
        if (buff) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (buff_pressed_ && now < last_buff_ms_ + kBuffGapMs) {
                if (i < due_ms_.size()) due_ms_[i] = last_buff_ms_ + kBuffGapMs;
                continue;
            }
        }

        // Schedule the next fire before pressing, so a slow press does not
        // push the whole cadence later and later.
        const std::uint32_t next = jittered(k.interval_ms, k.jitter_pct);
        const std::uint64_t armed = now + next;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (i < due_ms_.size()) due_ms_[i] = armed;
        }

        if (press(k.keycode, k.hold_ms, "macro-timer")) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (i < pressed_ms_.size()) pressed_ms_[i] = now;
                if (buff) {
                    last_buff_ms_ = now;
                    buff_pressed_ = true;
                }
            }
            std::lock_guard<std::mutex> lock(stats_mutex_);
            if (stats_.key_presses.size() < cfg.keys.size())
                stats_.key_presses.resize(cfg.keys.size(), 0);
            ++stats_.key_presses[i];
        } else if (buff) {
            // A buff that did not go out -- held while out of the world, the
            // game not in front, the keys held for a dialog -- is tried again
            // in a couple of seconds. Left to its interval, a 1600-s buff
            // refused during a trash pass was gone for 27 minutes. Unless a
            // re-cast has re-armed it meanwhile, which wins.
            const std::uint64_t retry = now + jittered(kBuffRetryMs, k.jitter_pct);
            std::lock_guard<std::mutex> lock(mutex_);
            if (i < due_ms_.size() && due_ms_[i] == armed) due_ms_[i] = retry;
        }
    }
}

void MacroEngine::sequence_loop_fn() {
    while (running_.load()) {
        MacroConfig cfg;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = config_;
        }

        if (!cfg.sequence_enabled || cfg.sequence.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::size_t pos = seq_pos_.load();
        if (pos >= cfg.sequence.size()) {
            if (!cfg.sequence_loop) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            pos = 0;
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.sequence_loops;
        }

        const MacroStep step = cfg.sequence[pos];
        seq_pos_.store(pos + 1);

        if (step.keycode != 0) press(step.keycode, step.hold_ms, "macro-sequence");

        // Sleep in slices so stopping is responsive even behind a long delay.
        const std::uint32_t wait = jittered(step.delay_after_ms, 8);
        for (std::uint32_t slept = 0; slept < wait && running_.load(); slept += 20)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::uint32_t>(20, wait - slept)));
    }
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
