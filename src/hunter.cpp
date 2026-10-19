#include "gobot/hunter.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"
#include "gobot/monster_list.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace gobot {
namespace {

/// How long a monster on "Không đánh" is remembered where it stood after its
/// name was last read, and how close a nameless plate has to be to that spot
/// to be taken for it. A name drops out for a frame or two all the time --
/// grass, a casting effect, the plate half off the play field -- and a bot
/// that forgets the monster the moment its name goes attacks it.
constexpr std::uint64_t kSkipMemoryMs = 2000;
constexpr int           kSkipNearPx = 40;

/// A monster on "Đánh trước" takes over from an unlisted target at most this
/// often.
constexpr std::uint64_t kSwitchGapMs = 2000;

/// The target is followed from one look to the next by where it was: a plate
/// further than this from it is another monster, the target having died.
constexpr int kSameTargetPx = 120;

/// How far a health bar has to go down before the attacks count as hurting
/// it: the reading wobbles by less than this.
constexpr float kDamageStep = 0.03f;

/// GoC's walking speed in its own sums, for how long a step back is held.
constexpr int kWalkPxPerS = 355;

/// A step back after which nothing on screen moved this far went into a wall;
/// one that changed the height by more than kLedgePx went off a ledge.
constexpr int kWallPx = 10;
constexpr int kLedgePx = 20;

/// GoC waits this long after a step back, and after turning, before the next
/// key: the game takes the one before it first.
constexpr std::uint32_t kSettleMs = 50;

/// After a step back the view may still be catching up for a look or two.
constexpr std::uint64_t kStepSlackMs = 400;

/// A step back is judged on the picture this many on from the one there is
/// as it ends: the detector puts a picture out a moment after taking it, so
/// the next one may have been taken during the step -- and a picture from
/// before a step shows nothing moved, so a ledge went unseen.
constexpr std::uint64_t kJudgeAfterPictures = 2;

bool near(int ax, int ay, int bx, int by, int px) {
    const int dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy <= px * px;
}

/// Whether `target`, seen at `now`, is the monster last seen at `x`, `y` at
/// `seen_ms`: no further from there than kSameTargetPx -- `slack` more, a
/// step back having moved the view -- and not unseen for longer than a plate
/// drops out for.
bool same_monster(const Nameplate& target, int x, int y, std::uint64_t seen_ms, std::uint64_t now,
                  int slack) {
    return std::abs(target.x() - x) + std::abs(target.y() - y) <= kSameTargetPx + slack &&
           now - seen_ms <= MobDetector::kPlayerMemoryMs;
}

/// The band numbers as hunt.ini keeps them: any negative is -1, "follow
/// height_tolerance".
int band_value(int v) { return v < 0 ? -1 : std::min(v, 1000); }

} // namespace

// ---------------------------------------------------------------- config

bool HunterConfig::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) {
        GB_WARN("HunterConfig: cannot read %s -- using defaults", path.c_str());
        return false;
    }
    enabled            = ini.get_bool("Hunt", "enabled", false);
    character_name     = ini.get("Hunt", "character_name", "");
    attack_key         = static_cast<std::uint32_t>(ini.get_int("Hunt", "attack_key", 'Z'));
    left_key           = static_cast<std::uint32_t>(ini.get_int("Hunt", "left_key", 0x25));
    right_key          = static_cast<std::uint32_t>(ini.get_int("Hunt", "right_key", 0x27));
    jump_key           = static_cast<std::uint32_t>(ini.get_int("Hunt", "jump_key", 0x20));
    jump_checked       = ini.get_bool("Hunt", "jump_checked", false);
    reach              = ini.get_int("Hunt", "reach", 150);
    height_tolerance   = ini.get_int("Hunt", "height_tolerance", 60);
    // Missing from a file written before "Kiểu đánh": no style, no minimum
    // distance and the band following height_tolerance -- the hunter as it
    // was. Likewise the keep-distance and facing keys below.
    style              = ini.get("Hunt", "style", "");
    min_distance       = std::clamp(ini.get_int("Hunt", "min_distance", 0), 0, std::max(0, reach));
    up_reach           = band_value(ini.get_int("Hunt", "up_reach", -1));
    down_reach         = band_value(ini.get_int("Hunt", "down_reach", -1));
    up_no_jump         = band_value(ini.get_int("Hunt", "up_no_jump", -1));
    down_no_jump       = band_value(ini.get_int("Hunt", "down_no_jump", -1));
    keep_distance      = ini.get_bool("Hunt", "keep_distance", false);
    keep_distance_steps = std::clamp(ini.get_int("Hunt", "keep_distance_steps", 3), 1, 10);
    facing_tolerance   = std::clamp(ini.get_int("Hunt", "facing_tolerance", 12), 0, 100);
    no_damage_ms       = static_cast<std::uint32_t>(
        std::clamp(ini.get_int("Hunt", "no_damage_ms", 15000), 1000, 600000));
    give_up_ms         = static_cast<std::uint32_t>(ini.get_int("Hunt", "give_up_ms", 4000));
    dead_below         = ini.get_float("Hunt", "dead_below", 0.05f);
    finish_wounded     = ini.get_bool("Hunt", "finish_wounded", true);
    step_ms            = static_cast<std::uint32_t>(ini.get_int("Hunt", "step_ms", 180));
    attack_interval_ms = static_cast<std::uint32_t>(ini.get_int("Hunt", "attack_interval_ms", 700));
    jump_to_reach      = ini.get_bool("Hunt", "jump_to_reach", true);
    loot               = ini.get_bool("Hunt", "loot", false);
    loot_key           = static_cast<std::uint32_t>(ini.get_int("Hunt", "loot_key", 0));
    loot_interval_ms   = static_cast<std::uint32_t>(ini.get_int("Hunt", "loot_interval_ms", 2000));
    stay_put           = ini.get_bool("Hunt", "stay_put", false);
    // Missing from a file written before the lists: both empty and "only
    // those" off, which is the hunter as it was.
    skip_names         = split_monster_names(ini.get("Hunt", "skip_monsters", ""));
    first_names        = split_monster_names(ini.get("Hunt", "first_monsters", ""));
    first_only         = ini.get_bool("Hunt", "first_only", false);
    require_focus      = ini.get_bool("Hunt", "require_focus", true);
    focus_on_start     = ini.get_bool("Hunt", "focus_on_start", true);
    toggle_key         = static_cast<std::uint32_t>(ini.get_int("Hunt", "toggle_key", 122));
    return true;
}

bool HunterConfig::save(const std::string& path) const {
    IniFile ini;
    ini.load(path);        // preserve the rest of the file
    ini.set_bool("Hunt", "enabled", enabled);
    ini.set    ("Hunt", "character_name", character_name);
    ini.set_int ("Hunt", "attack_key", static_cast<int>(attack_key));
    ini.set_int ("Hunt", "left_key", static_cast<int>(left_key));
    ini.set_int ("Hunt", "right_key", static_cast<int>(right_key));
    ini.set_int ("Hunt", "jump_key", static_cast<int>(jump_key));
    ini.set_bool("Hunt", "jump_checked", jump_checked);
    ini.set_int ("Hunt", "reach", reach);
    // Always, for an older GoBot, which knows only this: with a style it is
    // the larger of up and down already (apply_attack_style).
    ini.set_int ("Hunt", "height_tolerance", height_tolerance);
    ini.set     ("Hunt", "style", style);
    ini.set_int ("Hunt", "min_distance", min_distance);
    // The band only when set. -1, "follow height_tolerance", goes in only over
    // a value the file has already, so that it does not stay behind; a file
    // without them is left without them.
    const auto band = [&ini](const char* key, int v) {
        if (v >= 0 || ini.has_key("Hunt", key)) ini.set_int("Hunt", key, band_value(v));
    };
    band("up_reach", up_reach);
    band("down_reach", down_reach);
    band("up_no_jump", up_no_jump);
    band("down_no_jump", down_no_jump);
    ini.set_bool("Hunt", "keep_distance", keep_distance);
    ini.set_int ("Hunt", "keep_distance_steps", keep_distance_steps);
    ini.set_int ("Hunt", "facing_tolerance", facing_tolerance);
    ini.set_int ("Hunt", "no_damage_ms", static_cast<int>(no_damage_ms));
    ini.set_int ("Hunt", "give_up_ms", static_cast<int>(give_up_ms));
    ini.set_float("Hunt", "dead_below", dead_below);
    ini.set_bool("Hunt", "finish_wounded", finish_wounded);
    ini.set_int ("Hunt", "step_ms", static_cast<int>(step_ms));
    ini.set_int ("Hunt", "attack_interval_ms", static_cast<int>(attack_interval_ms));
    ini.set_bool("Hunt", "jump_to_reach", jump_to_reach);
    ini.set_bool("Hunt", "loot", loot);
    ini.set_int ("Hunt", "loot_key", static_cast<int>(loot_key));
    ini.set_int ("Hunt", "loot_interval_ms", static_cast<int>(loot_interval_ms));
    ini.set_bool("Hunt", "stay_put", stay_put);
    ini.set     ("Hunt", "skip_monsters", join_monster_names(skip_names));
    ini.set     ("Hunt", "first_monsters", join_monster_names(first_names));
    ini.set_bool("Hunt", "first_only", first_only);
    ini.set_bool("Hunt", "require_focus", require_focus);
    ini.set_bool("Hunt", "focus_on_start", focus_on_start);
    ini.set_int ("Hunt", "toggle_key", static_cast<int>(toggle_key));
    return ini.save(path, "GoBot hunting");
}

void HunterConfig::set_band(int up_px, int down_px) {
    // Pinned first: they follow height_tolerance, which moves below.
    up_no_jump = up_nj();
    down_no_jump = down_nj();
    up_reach = std::clamp(up_px, 0, 1000);
    down_reach = std::clamp(down_px, 0, 1000);
    height_tolerance = std::max(up_reach, down_reach);
}

// ---------------------------------------------------------------- hunter

const char* Hunter::to_string(State s) {
    switch (s) {
        case State::NoPlayer:    return "cannot see your character";
        case State::NoTargets:   return "no monsters in view";
        case State::Approaching: return "walking to the target";
        case State::Attacking:   return "attacking";
        case State::Unfocused:   return "waiting for the game to have focus";
        case State::Waiting:     return "standing still, waiting for a monster to come within reach";
        case State::Idle:        break;
    }
    return "idle";
}

Hunter::Hunter(InputManager& input, ExecutionCoordinator& coord, MobDetector& detector)
    : input_(input), coord_(coord), detector_(detector) {}

Hunter::~Hunter() { stop(); }

void Hunter::set_config(const HunterConfig& c) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = c;
    attack_key_.store(c.attack_key);
}

bool Hunter::engaged() const {
    // A hold refuses every key from the moment it begins, so the fight is
    // off then, not at the next pass.
    return engaged_.load() && running_.load() && !coord_.critical_active();
}

HunterConfig Hunter::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

Hunter::Status Hunter::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool Hunter::level_with(const HunterConfig& cfg, int dy) {
    // Screen y grows downward: a monster above has a negative dy.
    return dy < 0 ? -dy <= cfg.up() : dy <= cfg.down();
}

bool Hunter::in_reach(const HunterConfig& cfg, int dx, int dy) {
    return std::abs(dx) <= cfg.reach && level_with(cfg, dy);
}

int Hunter::keep_away_px(const HunterConfig& cfg) {
    return cfg.min_distance > 0 ? cfg.min_distance : kDefaultKeepAwayPx;
}

int Hunter::backstep_ms(int away, int dx) {
    const double px = static_cast<double>(away - std::abs(dx) + 20);
    const long ms = std::lround(px * 1000.0 / kWalkPxPerS);
    return static_cast<int>(std::clamp<long>(ms, 50, 800));
}

void Hunter::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    stepped_ = false;
    fresh_run();
    thread_ = std::thread(&Hunter::loop, this);
    GB_INFO("=== HUNT ON ===");
}

void Hunter::start_stepped() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    stepped_ = true;
    fresh_run();
}

void Hunter::step() {
    if (running_.load() && stepped_.load()) pass();
}

void Hunter::fresh_run() {
    have_target_ = false;
    target_first_ = false;
    switched_ = false;
    skip_spots_.clear();
    said_skipped_.clear();
    // Nothing is known of which way the character faces, nor of any fight.
    facing_ = Facing::Unknown;
    held_before_ = false;
    just_stepped_ = false;
    step_slack_px_ = 0;
    step_slack_until_ms_ = 0;
    fight_ = FightLog{};
    watch_ = DamageWatch{};
    back_ = BackSteps{};
    gave_up_.clear();
    minute_from_ms_ = 0;
    minute_fights_ = minute_far_ = minute_backsteps_ = 0;
    last_engaged_ms_ = 0;
    disengage();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.only_skipped = false;
    status_.no_target_since_ms = 0;
}

void Hunter::stop() {
    if (!running_.exchange(false)) return;
    stepped_ = false;
    if (thread_.joinable()) thread_.join();
    disengage();
    end_fight("stopped");
    release_movement();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = State::Idle;
        status_.has_target = false;
        status_.only_skipped = false;
    }
    GB_INFO("=== HUNT OFF ===");
}

// The seams: the real detector, keyboard and clock unless a test has put its
// own in their place.

bool Hunter::see_player(Nameplate& out, bool* fresh) const {
    return eyes_.player ? eyes_.player(out, fresh) : detector_.player(out, fresh);
}

std::vector<Nameplate> Hunter::see_mobs() const {
    return eyes_.mobs ? eyes_.mobs() : detector_.mobs();
}

bool Hunter::focused() const {
    return hands_.focused ? hands_.focused() : input_.window_focused();
}

bool Hunter::press(std::uint32_t vk, int hold_ms) {
    return hands_.key ? hands_.key(vk, hold_ms) : input_.key_press(vk, hold_ms);
}

void Hunter::let_go(std::uint32_t vk) {
    if (hands_.key_up) hands_.key_up(vk);
    else               input_.key_up(vk);
}

bool Hunter::is_down(std::uint32_t vk) const {
    return hands_.held ? hands_.held(vk) : input_.is_held(vk);
}

std::uint64_t Hunter::clock_now() const {
    return clock_.now_ms ? clock_.now_ms() : now_ms();
}

std::uint64_t Hunter::picture() const {
    if (eyes_.frame) return eyes_.frame();
    if (eyes_.player || eyes_.mobs) return passes_;
    return detector_.stats().frames;
}

void Hunter::pause(std::uint32_t ms) {
    if (ms == 0) return;
    if (clock_.sleep_ms) clock_.sleep_ms(ms);
    else                 std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void Hunter::engage(int dx, std::uint64_t now) {
    engaged_dx_.store(std::abs(dx));
    last_engaged_ms_ = now;
    const bool was = engaged_.exchange(true);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!was) status_.engaged_since_ms = now;
    status_.engaged = true;
    status_.last_engaged_ms = now;
}

void Hunter::disengage() {
    if (!engaged_.exchange(false)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    status_.engaged = false;
}

void Hunter::release_movement() {
    // release_all() rather than key_up(), which is gated on the game having
    // focus. The moment that matters is exactly the moment focus is lost with
    // a direction held: the game still has the key down, the character keeps
    // running, and a gated release does nothing about it. release_all() sends
    // the key-up regardless, and is a no-op when nothing is held.
    if (hands_.release_all) hands_.release_all();
    else                    input_.release_all();
}

void Hunter::let_go_of_moves(const HunterConfig& cfg) {
    // Every key the hunter presses is down and up again inside its own pass,
    // so between passes it holds nothing -- unless a key-up did not go out,
    // the game having lost focus during the hold, which leaves that key down.
    // Only the direction keys, then, and only when down. This used to be
    // release_all() on every pass, which let go of whatever any thread was
    // holding: a potion pressed by the macro was cut off mid-press by the
    // hunter's 60-ms rhythm.
    for (const std::uint32_t vk : { cfg.left_key, cfg.right_key })
        if (vk != 0 && is_down(vk)) let_go(vk);
}

bool Hunter::press_loot(const HunterConfig& cfg) {
    auto token = coord_.acquire("hunt-loot", Priority::Loot);
    if (!token) return false;
    press(cfg.loot_key, 40);
    last_loot_ms_ = clock_now();
    std::lock_guard<std::mutex> lock(mutex_);
    ++status_.loots;
    return true;
}

// ---------------------------------------------------------------- choosing

int Hunter::choose_target(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                          const Nameplate* me, const std::vector<Listed>& listed) {
    const auto out = [&](std::size_t i) { return i < listed.size() && refused(listed[i]); };
    const auto first = [&](std::size_t i) {
        return i < listed.size() && listed[i] == Listed::First;
    };
    const auto hp_of = [&](int i) { return mobs[static_cast<std::size_t>(i)].hp; };

    int target = -1;
    if (me) {
        // Prefer something it can actually reach.
        //
        // The list is sorted nearest-first, but nearest includes things
        // standing on a ledge overhead. Chasing one of those means walking
        // back and forth underneath it until the give-up timer fires, over
        // and over, which is what the bot spent its first live run doing:
        // seventy-nine steps and one swing.
        //
        // Those on "Đánh trước" come first, and among them the same rules.
        int nearest = -1, wounded = -1, first_nearest = -1, first_wounded = -1;
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            const Nameplate& n = mobs[i];
            if (!level_with(cfg, n.y() - me->y())) continue;
            if (n.hp <= cfg.dead_below) continue;    // already finished
            if (out(i)) continue;                    // the lists say no
            const int at = static_cast<int>(i);
            if (nearest < 0) nearest = at;
            if (cfg.finish_wounded && (wounded < 0 || n.hp < hp_of(wounded))) wounded = at;
            if (first(i)) {
                if (first_nearest < 0) first_nearest = at;
                if (cfg.finish_wounded && (first_wounded < 0 || n.hp < hp_of(first_wounded)))
                    first_wounded = at;
            }
        }
        if (first_nearest >= 0) {
            nearest = first_nearest;
            wounded = first_wounded;
        }
        target = nearest;
        // Finish something already hurt instead of starting afresh. Only
        // among those it could reach anyway, so this never sends it chasing
        // across the screen.
        if (wounded >= 0 && hp_of(wounded) < 1.0f) target = wounded;
    }

    // Nothing level, or no idea where we are: the nearest that still has
    // health, and let the timer decide.
    if (target < 0) {
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            if (mobs[i].hp > cfg.dead_below && !out(i)) {
                target = static_cast<int>(i);
                break;
            }
        }
    }
    return target;
}

int Hunter::choose_in_reach(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                            const Nameplate& me, const std::vector<Listed>& listed) {
    int best = -1, nearest = 0;
    int best_first = -1, nearest_first = 0;
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.hp <= cfg.dead_below) continue;
        if (i < listed.size() && refused(listed[i])) continue;
        if (!in_reach(cfg, n.x() - me.x(), n.y() - me.y())) continue;
        const int ndx = std::abs(n.x() - me.x());
        const int at = static_cast<int>(i);
        if (best < 0 || ndx < nearest) { best = at; nearest = ndx; }
        if (i < listed.size() && listed[i] == Listed::First &&
            (best_first < 0 || ndx < nearest_first)) {
            best_first = at;
            nearest_first = ndx;
        }
    }
    return best_first >= 0 ? best_first : best;
}

std::vector<Hunter::Listed> Hunter::judge(const HunterConfig& cfg,
                                          const std::vector<Nameplate>& mobs,
                                          std::uint64_t now) {
    std::vector<Listed> out(mobs.size(), Listed::Free);
    if (cfg.skip_names.empty() && cfg.first_names.empty() && !cfg.first_only) {
        skip_spots_.clear();
        return out;
    }

    // By name. A name on both lists is skipped: the safe way round, since a
    // wrong skip costs a kill and a wrong "first" drags the bot after it.
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const std::string& name = mobs[i].name;
        if (name.empty()) continue;
        if (monster_on_list(cfg.skip_names, name))       out[i] = Listed::Skip;
        else if (monster_on_list(cfg.first_names, name)) out[i] = Listed::First;
    }

    // Where the skipped ones stand. Each is followed from frame to frame --
    // by its name while that reads, and by the nearest nameless plate while
    // it does not -- and forgotten kSkipMemoryMs after its name last read.
    skip_spots_.erase(std::remove_if(skip_spots_.begin(), skip_spots_.end(),
                                     [now](const SkipSpot& s) { return s.until_ms <= now; }),
                      skip_spots_.end());
    std::vector<char> claimed(skip_spots_.size(), 0);
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        if (out[i] != Listed::Skip) continue;
        const Nameplate& n = mobs[i];
        int spot = -1, spot_d = 0;
        for (std::size_t s = 0; s < skip_spots_.size(); ++s) {
            if (claimed[s] || !near(n.x(), n.y(), skip_spots_[s].x, skip_spots_[s].y, kSkipNearPx))
                continue;
            const int d = std::abs(n.x() - skip_spots_[s].x) + std::abs(n.y() - skip_spots_[s].y);
            if (spot < 0 || d < spot_d) { spot = static_cast<int>(s); spot_d = d; }
        }
        if (spot < 0) {
            // One not seen a moment ago: counted, and named in the log once a
            // run, so the player can see the list at work.
            skip_spots_.push_back(SkipSpot{ n.x(), n.y(), now + kSkipMemoryMs });
            claimed.push_back(1);
            say_skipped(n.name);
            std::lock_guard<std::mutex> lock(mutex_);
            ++status_.skipped_by_list;
        } else {
            SkipSpot& s = skip_spots_[static_cast<std::size_t>(spot)];
            s.x = n.x();
            s.y = n.y();
            s.until_ms = now + kSkipMemoryMs;
            claimed[static_cast<std::size_t>(spot)] = 1;
        }
    }
    // Those whose name did not read this time: moved to the nearest nameless
    // plate, without being kept any longer for it.
    for (std::size_t s = 0; s < skip_spots_.size(); ++s) {
        if (claimed[s]) continue;
        int to = -1, to_d = 0;
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            const Nameplate& n = mobs[i];
            if (!n.name.empty() ||
                !near(n.x(), n.y(), skip_spots_[s].x, skip_spots_[s].y, kSkipNearPx))
                continue;
            const int d = std::abs(n.x() - skip_spots_[s].x) + std::abs(n.y() - skip_spots_[s].y);
            if (to < 0 || d < to_d) { to = static_cast<int>(i); to_d = d; }
        }
        if (to >= 0) {
            skip_spots_[s].x = mobs[static_cast<std::size_t>(to)].x();
            skip_spots_[s].y = mobs[static_cast<std::size_t>(to)].y();
        }
    }

    // A nameless plate where a skipped one stands is that one.
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (out[i] != Listed::Free || !n.name.empty()) continue;
        for (const SkipSpot& s : skip_spots_) {
            if (near(n.x(), n.y(), s.x, s.y, kSkipNearPx)) {
                out[i] = Listed::NearSkip;
                break;
            }
        }
    }

    // Only those on "Đánh trước", and nothing else at all.
    if (cfg.first_only)
        for (Listed& l : out)
            if (l == Listed::Free) l = Listed::NotFirst;
    return out;
}

void Hunter::say_skipped(const std::string& name) {
    if (monster_on_list(said_skipped_, name)) return;
    if (said_skipped_.size() < 64) said_skipped_.push_back(name);
    GB_INFO("Hunter: not attacking %s (Không đánh)", name.c_str());
}

void Hunter::mark_gave_up(const std::vector<Nameplate>& mobs, std::uint64_t now,
                          std::vector<Listed>& listed) {
    gave_up_.erase(std::remove_if(gave_up_.begin(), gave_up_.end(),
                                  [now](const GaveUpSpot& s) { return s.until_ms <= now; }),
                   gave_up_.end());
    // Each spot follows the monster it was, by the plate nearest it: given up
    // on, it would otherwise be picked again the moment it was let go of.
    for (GaveUpSpot& s : gave_up_) {
        int to = -1, to_d = 0;
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            const Nameplate& n = mobs[i];
            if (!near(n.x(), n.y(), s.x, s.y, kSkipNearPx)) continue;
            const int d = std::abs(n.x() - s.x) + std::abs(n.y() - s.y);
            if (to < 0 || d < to_d) { to = static_cast<int>(i); to_d = d; }
        }
        if (to < 0) continue;
        const std::size_t i = static_cast<std::size_t>(to);
        s.x = mobs[i].x();
        s.y = mobs[i].y();
        if (listed[i] == Listed::Free) listed[i] = Listed::GaveUp;
    }
}

// ---------------------------------------------------------------- a fight

void Hunter::track_fight(const HunterConfig& cfg, const Nameplate& target, std::uint64_t now,
                         int slack) {
    FightLog& f = fight_;
    // Somewhere other than where the target was, or after longer than the
    // character goes unseen for a moment: another monster, another fight.
    if (f.on && !same_monster(target, f.x, f.y, f.seen_ms, now, slack)) end_fight("another target");
    if (!f.on) {
        f = FightLog{};
        f.on = true;
        f.keep_away = keep_away_px(cfg);
    }
    f.x = target.x();
    f.y = target.y();
    f.seen_ms = now;
}

void Hunter::end_fight(const char* why) {
    FightLog& f = fight_;
    if (!f.on) return;
    if (f.attacks > 0) {
        // How far it was fought from: what tells a ranged character kept
        // its distance from one that stood toe to toe.
        std::vector<int> d = f.attack_dx;
        std::sort(d.begin(), d.end());
        const int median = d[d.size() / 2];
        ++minute_fights_;
        if (median >= f.keep_away) ++minute_far_;
        GB_DEBUG("Hunter: fight over (%s): %d attacks, |dx| at attack min %d px, median %d px, "
                 "%d back-steps", why, f.attacks, d.front(), median, f.backsteps);
    } else if (f.backsteps > 0) {
        GB_DEBUG("Hunter: fight over (%s): no attack, %d back-steps", why, f.backsteps);
    }
    f = FightLog{};
}

void Hunter::fight_unseen(std::uint64_t now, const char* why) {
    if (fight_.on && now - fight_.seen_ms > MobDetector::kPlayerMemoryMs) end_fight(why);
}

void Hunter::note_attack(int dx, int away) {
    const int adx = std::abs(dx);
    // Attacking from far enough at last: the steps back in a row count
    // afresh -- but a wall or a ledge met stays behind the character.
    if (adx >= away) back_.in_row = 0;
    ++watch_.attacks;
    FightLog& f = fight_;
    if (!f.on) return;
    ++f.attacks;
    if (f.attack_dx.size() < 64) f.attack_dx.push_back(adx);
}

void Hunter::follow_back_steps(const Nameplate& target, std::uint64_t now, int slack) {
    BackSteps& b = back_;
    // Somewhere other than where the monster stepped back from was, or not
    // seen for longer than the character goes unseen for a moment: another
    // monster, and its own count. A fight ended and begun again on the same
    // one -- its bar misread as empty -- is not.
    if (b.seen_ms != 0 && !same_monster(target, b.x, b.y, b.seen_ms, now, slack)) b = BackSteps{};
    b.x = target.x();
    b.y = target.y();
    b.seen_ms = now;
}

void Hunter::follow_watch(const Nameplate& target, std::uint64_t now, int slack) {
    DamageWatch& w = watch_;
    // As the steps back: another monster, or this one gone too long, and the
    // watch begins again. A plate missing for a look, or a bar misread as
    // empty once, leaves it where it was.
    if (w.seen_ms != 0 && !same_monster(target, w.x, w.y, w.seen_ms, now, slack)) w = DamageWatch{};
    w.x = target.x();
    w.y = target.y();
    w.seen_ms = now;
}

bool Hunter::no_damage(const HunterConfig& cfg, const Nameplate& target, std::uint64_t now) {
    DamageWatch& w = watch_;
    if (w.ref_hp < 0.0f || target.hp > w.ref_hp) {
        // The first look, or the bar has grown back: measured from here on.
        w.ref_hp = target.hp;
    } else if (target.hp <= w.ref_hp - kDamageStep) {
        // Hurt: the clock starts again.
        w.ref_hp = target.hp;
        w.quiet_ms = 0;
    }
    // Only time spent attacking it counts -- not walking to it, not a hold
    // that refused every key, not the moment before the first swing. The
    // time across a look or two without its plate counts too, while the
    // attacks go on: up to 250 ms at a time.
    const bool attacking = w.attacks > 0 && now - last_attack_ms_ <= cfg.attack_interval_ms + 300;
    if (attacking && w.quiet_seen_ms != 0 && now > w.quiet_seen_ms)
        w.quiet_ms += std::min<std::uint64_t>(now - w.quiet_seen_ms, 250);
    w.quiet_seen_ms = now;
    return w.quiet_ms >= cfg.no_damage_ms;
}

const char* Hunter::step_went_wrong(const Nameplate& me, const Nameplate& target) const {
    const BackSteps& b = back_;
    // A step moves the character across the screen, or the view under it --
    // the target with it -- or, at a wall, neither.
    const int moved = std::max(std::abs(me.x() - b.from_me_x), std::abs(target.x() - b.from_x));
    const int dy = target.y() - me.y();
    const int height = std::max(std::abs(me.y() - b.from_me_y), std::abs(dy - b.from_dy));
    if (height > kLedgePx) return "a ledge";
    if (moved < kWallPx) return "a wall";
    return nullptr;
}

void Hunter::minute_log(const HunterConfig& cfg, std::uint64_t now) {
    std::uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (minute_from_ms_ == 0) {
            minute_from_ms_ = now;
            minute_dropped_ = status_.targets_dropped;
            return;
        }
        if (now - minute_from_ms_ < 60'000) return;
        dropped = status_.targets_dropped - minute_dropped_;
        minute_dropped_ = status_.targets_dropped;
    }
    if (minute_fights_ > 0 || minute_backsteps_ > 0 || dropped > 0)
        GB_INFO("Hunter: %d fights, %d with attacks at >= %d px, %d back-steps, %llu given up",
                minute_fights_, minute_far_, keep_away_px(cfg), minute_backsteps_,
                static_cast<unsigned long long>(dropped));
    minute_from_ms_ = now;
    minute_fights_ = minute_far_ = minute_backsteps_ = 0;
}

// ---------------------------------------------------------------- the loop

void Hunter::loop() {
    // A thin wrapper: the pass is the same one step() runs, so the tests and
    // the thread cannot drift apart.
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (!running_.load()) break;
        pass();
    }

    release_movement();
}

void Hunter::pass() {
    HunterConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = config_;
    }
    // Which picture this look is at least: asked before the plates are, so
    // the plates are from that picture or a newer one.
    ++passes_;
    const std::uint64_t pic = picture();

    const auto set_state = [this](State s, bool only_skipped) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = s;
        status_.only_skipped = only_skipped;
        if (s == State::NoTargets && status_.no_target_since_ms == 0)
            status_.no_target_since_ms = clock_now();
    };

    // A hold -- a revive, a trash pass, a chat line, a popup being closed --
    // refuses every key while it is on, so there is no fight meanwhile; and
    // whatever it did may have turned the character, so which way it faces
    // is not known once it is over.
    const bool held = coord_.critical_active();
    if (held_before_ && !held) facing_ = Facing::Unknown;
    held_before_ = held;
    if (held) disengage();

    if (cfg.require_focus && !focused()) {
        release_movement();
        facing_ = Facing::Unknown;
        disengage();
        set_state(State::Unfocused, false);
        return;
    }
    minute_log(cfg, clock_now());

    // What the last kill dropped. A few presses, spaced so the game sees
    // each one, before anything else is chased -- walking off to the next
    // monster first leaves the drop behind.
    if (cfg.loots() && loot_left_ > 0 && clock_now() - last_loot_ms_ >= 220) {
        if (press_loot(cfg)) --loot_left_;
        return;
    }

    Nameplate me;
    bool me_fresh = true;
    if (!see_player(me, &me_fresh)) {
        let_go_of_moves(cfg);
        // Which way it faces is not known while it cannot be seen. The fight
        // is kept a moment -- a name plate drops out for a frame or two all
        // the time -- and the target picked again once the character is
        // back, where it was, is the same fight.
        facing_ = Facing::Unknown;
        if (clock_now() - last_engaged_ms_ > kEngagedGraceMs) disengage();
        set_state(State::NoPlayer, false);
        have_target_ = false;
        return;
    }

    const std::vector<Nameplate> mobs = see_mobs();
    if (mobs.empty()) {
        let_go_of_moves(cfg);
        disengage();
        set_state(State::NoTargets, false);
        // The last one in view has just gone, and the likeliest reason is
        // that it died. Or its plate dropped out for a look, which they do
        // all the time: the fight is over once it has not come back where it
        // was for a moment, not at once.
        if (have_target_) loot_left_ = 3;
        have_target_ = false;
        fight_unseen(clock_now(), "gone");
        return;
    }

    const std::uint64_t now = clock_now();

    // A step back moves the view with the character, and the target's plate
    // with it: allowed for in following the target, for a look or two.
    const int slack = now < step_slack_until_ms_ ? step_slack_px_ : 0;

    // What the monster lists make of each plate. With both lists empty they
    // make nothing of any of them, and what follows is the hunter as it was.
    std::vector<Listed> listed = judge(cfg, mobs, now);
    // With a style: those given up on a moment ago for taking no damage.
    if (!gave_up_.empty()) mark_gave_up(mobs, now, listed);

    // Stay on the current target while it is still there. Re-picking the
    // nearest every frame makes the bot oscillate between two monsters a
    // similar distance away and never reach either.
    const Nameplate* target = nullptr;
    if (have_target_) {
        int best = 0;
        std::size_t at = 0;
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            const Nameplate& n = mobs[i];
            const int d = std::abs(n.x() - target_x_) + std::abs(n.y() - target_y_);
            if (!target || d < best) { target = &n; best = d; at = i; }
        }
        // Too far from where it was: it died and this is a different one.
        if (best > kSameTargetPx + slack) {
            target = nullptr;
            loot_left_ = 3;
            disengage();
            end_fight("gone");
        }

        // Its bar is empty. Let go now rather than going on swinging at it
        // until the give-up timer fires several seconds later.
        if (target && target->hp <= cfg.dead_below) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.targets_finished;
            }
            target = nullptr;
            loot_left_ = 3;
            disengage();
            end_fight("finished");
        }

        // The lists rule it out: its name has come to read as one on "Không
        // đánh" -- a name often reads a frame or two after the plate -- or it
        // is nameless where one of those stands, or it is not on "Đánh trước"
        // with only those to be attacked. Let go at once. Not a failure to
        // reach it, so not counted as one. A target of "only those" whose
        // name merely did not read this time is still the one it was.
        if (target && refused(listed[at]) &&
            !(listed[at] == Listed::NotFirst && target->name.empty())) {
            target = nullptr;
            end_fight("the lists");
            watch_ = DamageWatch{};
        }

        // With a style, as GoC: the give-up clock runs only while the target
        // is out of reach, and one within reach is fought until it dies.
        // Without one it runs from the pick, in reach or not, and every fight
        // is picked afresh this often -- as it always was.
        if (target && cfg.goc_rules() &&
            in_reach(cfg, target->x() - me.x(), target->y() - me.y()))
            target_since_ms_ = now;
        if (target && now - target_since_ms_ > cfg.give_up_ms) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.targets_dropped;
            }
            target = nullptr;
            // Without a style the same monster is usually picked straight
            // back, and that is the same fight still: its steps back are not
            // counted afresh. With one, out of reach this long is a fight
            // given up -- and the watch on it with it, so that whatever is
            // fought next where it stood is watched from nothing.
            if (cfg.goc_rules()) {
                end_fight("out of reach");
                watch_ = DamageWatch{};
            }
        }

        // A monster on "Đánh trước" that has come within reach takes over
        // from an unlisted one -- never the other way round, and not more
        // than once in kSwitchGapMs, so that two of them cannot pass the bot
        // back and forth between them.
        if (target) {
            // Whether it is on "Đánh trước", by its name whenever that reads.
            // A frame without the name changes nothing.
            if (!target->name.empty()) target_first_ = listed[at] == Listed::First;
            if (!target_first_ && (!switched_ || now - last_switch_ms_ >= kSwitchGapMs)) {
                for (std::size_t i = 0; i < mobs.size(); ++i) {
                    const Nameplate& n = mobs[i];
                    if (listed[i] != Listed::First || n.hp <= cfg.dead_below) continue;
                    if (!in_reach(cfg, n.x() - me.x(), n.y() - me.y())) continue;
                    GB_INFO("Hunter: going for %s first (Đánh trước)", n.name.c_str());
                    end_fight("Đánh trước");
                    watch_ = DamageWatch{};
                    target = &n;
                    target_first_ = true;
                    target_since_ms_ = now;
                    last_switch_ms_ = now;
                    switched_ = true;
                    break;
                }
            }
        }
    }
    if (!target) {
        Nameplate me_now;
        const bool have_me = see_player(me_now);
        const int pick = choose_target(cfg, mobs, have_me ? &me_now : nullptr, listed);

        // Everything in view reads as finished, or the lists rule all of it
        // out. Standing still beats swinging at a corpse, which holds the
        // hunter in place while something living walks past.
        if (pick < 0) {
            // Ruled out by the lists, that is: one given up on a moment ago
            // is not the lists' doing.
            bool ruled_out = false;
            for (std::size_t i = 0; i < mobs.size(); ++i)
                if (refused(listed[i]) && listed[i] != Listed::GaveUp &&
                    mobs[i].hp > cfg.dead_below)
                    ruled_out = true;
            let_go_of_moves(cfg);
            disengage();
            set_state(State::NoTargets, ruled_out);
            have_target_ = false;
            // A target whose plate dropped out for this look, with only
            // monsters that may not be attacked left in view: as with no
            // plate at all, its fight goes on until it has been gone a
            // moment.
            fight_unseen(now, "nothing to attack");
            return;
        }
        target = &mobs[static_cast<std::size_t>(pick)];
        target_first_ = listed[static_cast<std::size_t>(pick)] == Listed::First;
        target_since_ms_ = now;
    }

    target_x_ = target->x();
    target_y_ = target->y();
    have_target_ = true;
    track_fight(cfg, *target, now, slack);
    follow_back_steps(*target, now, slack);
    follow_watch(*target, now, slack);

    // The last step back, judged on the first picture taken wholly after it
    // with the character seen in it -- whatever this pass goes on to do, and
    // before it does it. Judged only when another step was wanted, a step
    // down a ledge followed by an attack from far enough went unjudged, and
    // the next step went down another.
    if (back_.judge && me_fresh && pic >= back_.judge_from) {
        back_.judge = false;
        if (const char* wrong = step_went_wrong(me, *target)) {
            back_.no_more = true;
            GB_INFO("Hunter: no more steps back from this target -- the last one met %s", wrong);
        }
    }

    const int dx = target->x() - me.x();
    const int dy = target->y() - me.y();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.target = *target;
        status_.has_target = true;
        status_.dx = dx;
        status_.dy = dy;
        status_.no_target_since_ms = 0;
    }

    if (in_reach(cfg, dx, dy)) {
        let_go_of_moves(cfg);
        const int adx = std::abs(dx);
        const Facing toward = dx < 0 ? Facing::Left : Facing::Right;

        // With a style: a target whose bar has not gone down in no_damage_ms
        // of attacking it is one the attacks do not reach -- something in
        // the way, or a monster not to be hurt. Given up on, and passed over
        // for as long again, or it would be picked straight back. Never one
        // on "Đánh trước", which the player asked for by name.
        if (cfg.goc_rules() && !target_first_ && no_damage(cfg, *target, now)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.targets_dropped;
            }
            GB_INFO("Hunter: giving up on %s -- không thấy mất máu: its bar has not gone down in "
                    "%u s of attacking",
                    target->name.empty() ? "a monster" : target->name.c_str(),
                    cfg.no_damage_ms / 1000);
            if (gave_up_.size() < 16)
                gave_up_.push_back(GaveUpSpot{ target->x(), target->y(), now + cfg.no_damage_ms });
            disengage();
            end_fight("no damage");
            // Watched afresh from here: a monster fought next close by would
            // otherwise take on this one's quiet time and be given up on at
            // its first look.
            watch_ = DamageWatch{};
            have_target_ = false;
            return;
        }

        // "Giữ khoảng cách": too close, so a step back first, as GoC's
        // backing_away does -- the arrow away from the target, held as long
        // as walking takes to get to keep_away px, then a moment for the
        // game. The next pass turns to the target and attacks. Never standing
        // still (GoC's lazy mode does not either) and never from a target off
        // the character's own level: GoBot cannot see where a platform ends,
        // and stepping back off one is the worst this could do. So too never
        // more than keep_distance_steps steps in a row from one monster, never
        // before the last step has been seen on the screen, nor again after a
        // step that met a wall or went down a ledge.
        const int away = keep_away_px(cfg);
        if (cfg.keep_distance && !cfg.stay_put && !just_stepped_ && adx < away && !held &&
            !back_.no_more && !back_.judge &&
            back_.in_row < std::clamp(cfg.keep_distance_steps, 1, 10) &&
            std::abs(dy) <= std::max(cfg.up_nj(), cfg.down_nj())) {
            // Off before the key goes out: no skill is thrown as the step
            // starts.
            disengage();
            const int hold = backstep_ms(away, adx);
            bool stepped = false;
            if (auto token = coord_.acquire("hunt-move", Priority::Movement)) {
                back_.from_me_x = me.x();
                back_.from_me_y = me.y();
                back_.from_x = target->x();
                back_.from_dy = dy;
                stepped = press(dx < 0 ? cfg.right_key : cfg.left_key, hold);
            }
            if (stepped) {
                // GoC takes itself to be facing the way it stepped.
                facing_ = dx < 0 ? Facing::Right : Facing::Left;
                ++back_.in_row;
                back_.judge = true;
                back_.judge_from = picture() + kJudgeAfterPictures;
                ++fight_.backsteps;
                ++minute_backsteps_;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++status_.backsteps;
                }
                just_stepped_ = true;
                step_slack_px_ = hold * kWalkPxPerS / 1000;
                step_slack_until_ms_ = now + static_cast<std::uint64_t>(hold) + kSettleMs + kStepSlackMs;
                pause(kSettleMs);
                set_state(State::Attacking, false);
                return;
            }
        }
        just_stepped_ = false;

        // Face it. A tap turns the character without moving it far, and a
        // skill thrown the wrong way hits nothing at all. With a style or
        // keep_distance, as GoC: only when it is not facing that way already,
        // then a moment for the game to turn it -- a tap on every pass walked
        // the character in on the target ten pixels at a time. Without
        // either, a tap on every pass, as it always was.
        const bool memory = cfg.goc_rules() || cfg.keep_distance;
        bool tapped = false;
        if (adx > cfg.facing_tolerance && (!memory || facing_ != toward)) {
            if (auto token = coord_.acquire("hunt-face", Priority::Combat))
                tapped = press(dx < 0 ? cfg.left_key : cfg.right_key, 30);
            if (tapped) {
                facing_ = toward;
                if (memory) pause(kSettleMs);
            }
        }

        // Fighting, as far as the skills are concerned: facing it, with the
        // game in front and no hold on.
        const bool facing = tapped || facing_ == toward || adx <= cfg.facing_tolerance;
        if (facing && !held && focused()) engage(adx, now);
        else                              disengage();

        if (now - last_attack_ms_ >= cfg.attack_interval_ms) {
            if (auto token = coord_.acquire("hunt-attack", Priority::Combat)) {
                press(cfg.attack_key, 60);
                last_attack_ms_ = now;
                note_attack(dx, away);
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.attacks;
            }
        }
        if (cfg.loots() && now - last_loot_ms_ >= cfg.loot_interval_ms)
            press_loot(cfg);
        set_state(State::Attacking, false);
        return;
    }

    // Not attacking: standing waiting, or walking to it.
    disengage();
    just_stepped_ = false;

    // Standing still: never walk. Take whatever is already within reach
    // instead, and attack it next time round; with nothing in reach, wait
    // for something to come. The lists hold here as everywhere.
    if (cfg.stay_put) {
        let_go_of_moves(cfg);
        const int in = choose_in_reach(cfg, mobs, me, listed);
        if (in >= 0) {
            const std::size_t i = static_cast<std::size_t>(in);
            target_x_ = mobs[i].x();
            target_y_ = mobs[i].y();
            target_first_ = listed[i] == Listed::First;
            target_since_ms_ = now;
        } else {
            have_target_ = false;
        }
        set_state(State::Waiting, false);
        return;
    }

    // Walk towards it.
    set_state(State::Approaching, false);
    if (auto token = coord_.acquire("hunt-move", Priority::Movement)) {
        const std::uint32_t go = dx < 0 ? cfg.left_key : cfg.right_key;
        const std::uint32_t other = dx < 0 ? cfg.right_key : cfg.left_key;
        let_go(other);
        if (press(go, static_cast<int>(cfg.step_ms)))
            facing_ = dx < 0 ? Facing::Left : Facing::Right;

        // Up and out of reach: a hop is the cheapest thing that might help,
        // and the give-up timer catches the case where it does not. Higher
        // than the band it can hit without jumping -- half of
        // height_tolerance without a style -- and only with a jump key: with
        // none set it never hops.
        if (cfg.jump_to_reach && cfg.jump_key != 0 && dy < -cfg.up_nj())
            press(cfg.jump_key, 50);

        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.steps;
    }
}

} // namespace gobot
