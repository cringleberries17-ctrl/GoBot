#include "gobot/hunter.hpp"
#include "gobot/attack_style.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/load_meter.hpp"
#include "gobot/logger.hpp"
#include "gobot/monster_list.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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

/// A plate from the packets is followed by its slot and generation, which no
/// other monster shares: as far as this from where it was, so that two
/// monsters crossing are told apart without one being lost in a long look.
constexpr int kSlotSamePx = 3 * kSameTargetPx;

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

/// With the walk held, the most a kill's pick-ups are stood for: three
/// presses 220 ms apart, and some to spare for an input that is busy.
constexpr std::uint64_t kLootStandMs = 1500;

/// A step back is judged on the picture this many on from the one there is
/// as it ends: the detector puts a picture out a moment after taking it, so
/// the next one may have been taken during the step -- and a picture from
/// before a step shows nothing moved, so a ledge went unseen.
constexpr std::uint64_t kJudgeAfterPictures = 2;

bool near(int ax, int ay, int bx, int by, int px) {
    const int dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy <= px * px;
}

/// Whether `target`, seen at `now`, is the monster last seen at `x`, `y` --
/// in `slot`, of `gen` -- at `seen_ms`: no further from there than
/// kSameTargetPx -- `slack` more, a step back having moved the view -- and not
/// unseen for longer than a plate drops out for. Both from the packets, with
/// slots: the same slot and generation, within kSlotSamePx and `slack`.
bool same_monster(const Nameplate& target, int x, int y, int slot, std::uint32_t gen, std::uint64_t seen_ms,
                  std::uint64_t now, int slack) {
    if (now - seen_ms > MobDetector::kPlayerMemoryMs) return false;
    const int d = std::abs(target.x() - x) + std::abs(target.y() - y);
    if (target.slot >= 0 && slot >= 0) return target.slot == slot && target.gen == gen && d <= kSlotSamePx + slack;
    return d <= kSameTargetPx + slack;
}

/// The band numbers as hunt.ini keeps them: any negative is -1, "follow
/// height_tolerance".
int band_value(int v) { return v < 0 ? -1 : std::min(v, 1000); }

int round_px(float v) { return static_cast<int>(std::lround(v)); }

/// A hunting area sent from the map tab as the tab keeps it, for its rules
/// (hunt_zone.hpp).
HuntZone zone_box(const Hunter::Zone& z) {
    HuntZone h;
    h.home_x = round_px(z.home_x);
    h.home_y = round_px(z.home_y);
    h.left = round_px(z.left);
    h.top = round_px(z.top);
    h.right = round_px(z.right);
    h.bottom = round_px(z.bottom);
    return h;
}

} // namespace

// ---------------------------------------------------------------- config

const char* packet_positions_name(PacketPositions p) {
    switch (p) {
        case PacketPositions::Off:   return "off";
        case PacketPositions::On:    return "on";
        case PacketPositions::Watch: break;
    }
    return "watch";
}

PacketPositions packet_positions_from(const std::string& word) {
    std::string w;
    for (const char c : word)
        if (!std::isspace(static_cast<unsigned char>(c)))
            w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w == "off") return PacketPositions::Off;
    if (w == "on") return PacketPositions::On;
    return PacketPositions::Watch;
}

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
    // One this GoBot does not know -- typed by hand, or from a newer one --
    // is none: the picker shows "Như trước" for it, and GoC's rules must not
    // be on behind that.
    if (!style.empty() && find_attack_style(style) == nullptr) {
        GB_WARN("HunterConfig: unknown style \"%s\" in %s -- fighting as with none (Như trước)",
                style.c_str(), path.c_str());
        style.clear();
    }
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
    // Missing from a file written before it: held, as a player walks. The
    // steps of old are a word away, should the game ever take a held key
    // amiss.
    hold_walk          = ini.get_bool("Hunt", "hold_walk", true);
    step_ms            = static_cast<std::uint32_t>(ini.get_int("Hunt", "step_ms", 180));
    attack_interval_ms = static_cast<std::uint32_t>(ini.get_int("Hunt", "attack_interval_ms", 700));
    jump_to_reach      = ini.get_bool("Hunt", "jump_to_reach", true);
    // Missing from a file written before "Nhảy xuống bậc dưới": off, and its
    // numbers the drop plan's, within reason -- a slip of the keyboard must
    // not let a drop go a gate's width from a gate, nor past every floor.
    drop_down          = ini.get_bool("Hunt", "drop_down", false);
    drop_min_px        = std::clamp(ini.get_int("Hunt", "drop_min_px", 100), 60, 470);
    drop_side_px       = std::clamp(ini.get_int("Hunt", "drop_side_px", 120), 20, 300);
    drop_gate_px       = std::clamp(ini.get_int("Hunt", "drop_gate_px", 150), 40, 400);
    // Missing from a file written before "Nhảy lên bục trên": on -- the
    // owner's answer 5 of 2026-11-05, "Bật sẵn".
    jump_up            = ini.get_bool("Hunt", "jump_up", true);
    // Missing from a file written before "Tự tìm quái trong vùng": on -- the
    // owner's Q7 (a).
    seek_in_zone       = ini.get_bool("Hunt", "seek_in_zone", true);
    loot               = ini.get_bool("Hunt", "loot", false);
    loot_key           = static_cast<std::uint32_t>(ini.get_int("Hunt", "loot_key", 0));
    loot_interval_ms   = static_cast<std::uint32_t>(ini.get_int("Hunt", "loot_interval_ms", 2000));
    stay_put           = ini.get_bool("Hunt", "stay_put", false);
    // Missing from a file written before "Chỗ đánh quái": off, and the area
    // and its timings GoC's, within reason -- a slip of the keyboard must not
    // make an area of the whole map, nor one too small to stand in.
    spot               = ini.get_bool("Hunt", "spot", false);
    spot_half_w        = std::clamp(ini.get_int("Hunt", "spot_half_w", 300), 50, 2000);
    spot_half_h        = std::clamp(ini.get_int("Hunt", "spot_half_h", 100), 20, 500);
    spot_tolerance     = std::clamp(ini.get_int("Hunt", "spot_tolerance", 48), 10, 1000);
    spot_return_delay_ms = static_cast<std::uint32_t>(
        std::clamp(ini.get_int("Hunt", "spot_return_delay_ms", 1500), 0, 60000));
    // Not under 3 s: a walk on a clear floor is reported only as it starts and
    // at the stop of its renewal, 2.5 s on (Hunter::kWalkRenewMs).
    spot_give_up_ms    = static_cast<std::uint32_t>(
        std::clamp(ini.get_int("Hunt", "spot_give_up_ms", 4000), 3000, 60000));
    spot_retry_ms      = static_cast<std::uint32_t>(
        std::clamp(ini.get_int("Hunt", "spot_retry_ms", 30000), 1000, 600000));
    // Missing from a file written before the lists: both empty and "only
    // those" off, which is the hunter as it was.
    skip_names         = split_monster_names(ini.get("Hunt", "skip_monsters", ""));
    first_names        = split_monster_names(ini.get("Hunt", "first_monsters", ""));
    first_only         = ini.get_bool("Hunt", "first_only", false);
    require_focus      = ini.get_bool("Hunt", "require_focus", true);
    focus_on_start     = ini.get_bool("Hunt", "focus_on_start", true);
    // Missing from a file written before "Vị trí từ gói tin": Watch, as an
    // unknown word is -- the capture only watches, and the screen fights as
    // it did. The two numbers that lay the packets over the screen likewise
    // read as one to one, kept within reason: a slip of the keyboard must not
    // send the hunter after monsters a whole map away.
    packet_positions   = packet_positions_from(ini.get("Hunt", "packet_positions", "watch"));
    const float scale  = ini.get_float("Hunt", "packet_scale", 1.0f);
    packet_scale       = std::isfinite(scale) ? std::clamp(scale, 0.25f, 4.0f) : 1.0f;
    packet_dy          = std::clamp(ini.get_int("Hunt", "packet_dy", 0), -200, 200);
    toggle_key         = static_cast<std::uint32_t>(ini.get_int("Hunt", "toggle_key", 122));
    return true;
}

bool HunterConfig::save(const std::string& path) const {
    IniFile ini;
    ini.load(path);        // preserve the rest of the file
    store(ini);
    return ini.save(path, "GoBot hunting");
}

void HunterConfig::store(IniFile& ini) const {
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
    ini.set_bool("Hunt", "hold_walk", hold_walk);
    ini.set_int ("Hunt", "step_ms", static_cast<int>(step_ms));
    ini.set_int ("Hunt", "attack_interval_ms", static_cast<int>(attack_interval_ms));
    ini.set_bool("Hunt", "jump_to_reach", jump_to_reach);
    ini.set_bool("Hunt", "drop_down", drop_down);
    ini.set_int ("Hunt", "drop_min_px", drop_min_px);
    ini.set_int ("Hunt", "drop_side_px", drop_side_px);
    ini.set_int ("Hunt", "drop_gate_px", drop_gate_px);
    ini.set_bool("Hunt", "jump_up", jump_up);
    ini.set_bool("Hunt", "seek_in_zone", seek_in_zone);
    ini.set_bool("Hunt", "loot", loot);
    ini.set_int ("Hunt", "loot_key", static_cast<int>(loot_key));
    ini.set_int ("Hunt", "loot_interval_ms", static_cast<int>(loot_interval_ms));
    ini.set_bool("Hunt", "stay_put", stay_put);
    ini.set_bool("Hunt", "spot", spot);
    ini.set_int ("Hunt", "spot_half_w", spot_half_w);
    ini.set_int ("Hunt", "spot_half_h", spot_half_h);
    ini.set_int ("Hunt", "spot_tolerance", spot_tolerance);
    ini.set_int ("Hunt", "spot_return_delay_ms", static_cast<int>(spot_return_delay_ms));
    ini.set_int ("Hunt", "spot_give_up_ms", static_cast<int>(spot_give_up_ms));
    ini.set_int ("Hunt", "spot_retry_ms", static_cast<int>(spot_retry_ms));
    ini.set     ("Hunt", "skip_monsters", join_monster_names(skip_names));
    ini.set     ("Hunt", "first_monsters", join_monster_names(first_names));
    ini.set_bool("Hunt", "first_only", first_only);
    ini.set_bool("Hunt", "require_focus", require_focus);
    ini.set_bool("Hunt", "focus_on_start", focus_on_start);
    ini.set     ("Hunt", "packet_positions", packet_positions_name(packet_positions));
    ini.set_float("Hunt", "packet_scale", packet_scale);
    ini.set_int ("Hunt", "packet_dy", packet_dy);
    ini.set_int ("Hunt", "toggle_key", static_cast<int>(toggle_key));
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
        case State::Returning:   return "walking back to the hunting spot";
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

Hunter::Spot Hunter::spot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spot_;
}

// Asked from the GUI's thread, done at the hunter's next pass: the spot is
// the hunter's own, and a change made beside a pass would be written over.
void Hunter::retake_spot() {
    std::lock_guard<std::mutex> lock(mutex_);
    retake_asked_ = true;
    suspend_asked_ = false;
    zone_asked_.reset();
}

void Hunter::suspend_spot() {
    std::lock_guard<std::mutex> lock(mutex_);
    suspend_asked_ = true;
    // And the map drops go by: where the character now stands may be
    // another map's (drop_watch).
    drop_suspend_asked_ = true;
}

// The map tab's asks, done at the next pass as retake_spot's is -- a delete
// or an aside before it, then the new area: an area asked before a delete or
// an aside of its map goes with it, so one asked after is the latest.
void Hunter::set_zone(const Zone& z) {
    if (zone_stem(z.stem).empty() || !(z.left < z.right) || !(z.top < z.bottom) || z.home_x < z.left ||
        z.home_x > z.right || z.home_y < z.top || z.home_y > z.bottom) {
        GB_WARN("Hunter: a hunting area from the map tab not taken: '%s', %s", z.stem.c_str(),
                zone_text(zone_box(z)).c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    zone_asked_ = z;
    zone_asked_->stem = zone_stem(z.stem);
}

void Hunter::clear_zone(const std::string& stem) {
    const std::string s = zone_stem(stem);
    if (s.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (zone_asked_ && zone_asked_->stem == s) zone_asked_.reset();
    zone_clear_asked_ = s;
}

void Hunter::zone_aside(const std::string& stem) {
    const std::string s = zone_stem(stem);
    if (s.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (zone_asked_ && zone_asked_->stem == s) zone_asked_.reset();
    zone_aside_asked_ = s;
}

bool Hunter::level_with(const HunterConfig& cfg, int dy) {
    // Screen y grows downward: a monster above has a negative dy.
    return dy < 0 ? -dy <= cfg.up() : dy <= cfg.down();
}

bool Hunter::in_reach(const HunterConfig& cfg, int dx, int dy) {
    return std::abs(dx) <= cfg.reach && level_with(cfg, dy);
}

bool Hunter::hops_to_targets(const HunterConfig& cfg) {
    // hop()'s own: no jump key, or one that never goes out (↓), jump_to_reach
    // off, or the spot on -- no hop toward a monster at all.
    return cfg.jump_to_reach && cfg.jump_key != 0 && !InputManager::never_pressed(cfg.jump_key) && !cfg.spot;
}

std::optional<int> Hunter::monster_floor(const GameMapBook::Page& p, int x, int y) {
    // Top first: the first floor not more than kDropFloorMatchPx above the
    // point is the one it stands on, or is over.
    for (const MapFloor& f : p.floors_at(x)) {
        if (f.wall() || f.y < y - kDropFloorMatchPx) continue;
        if (f.y - y > kDropMonsterFloorPx) return std::nullopt;
        return f.y;
    }
    return std::nullopt;
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
    // Nothing is known of which way the character faces, nor of any fight --
    // but for what the packets say, any report of theirs being newer than a
    // key of this run's.
    facing_ = Facing::Unknown;
    facing_key_ms_ = 0;
    facing_key_way_ = Facing::Unknown;
    held_before_ = false;
    walk_vk_ = 0;
    walk_until_ms_.store(0);
    walk_since_ms_ = 0;
    walk_adx_ = 0;
    walk_progress_ms_ = 0;
    last_hop_ms_ = 0;
    loot_since_ms_ = 0;
    just_stepped_ = false;
    step_slack_px_ = 0;
    step_slack_until_ms_ = 0;
    fight_ = FightLog{};
    watch_ = DamageWatch{};
    back_ = BackSteps{};
    retreat_[0] = retreat_[1] = Retreat{};
    walk_side_ = 0;
    walk_run_ms_ = 0;
    walk_pass_ = 0;
    walk_at_ms_ = 0;
    gave_up_.clear();
    minute_from_ms_ = 0;
    minute_fights_ = minute_far_ = minute_backsteps_ = 0;
    minute_returns_ = minute_return_give_ups_ = 0;
    minute_turns_ = minute_packet_facings_ = minute_missed_keys_ = 0;
    minute_wounded_ = minute_slot_kept_ = minute_swaps_refused_ = minute_new_gen_ = 0;
    minute_picks_ = minute_nearest_below_ = minute_nearest_above_ = 0;
    minute_below_wait_ms_ = 0;
    below_wait_pass_ = below_wait_at_ms_ = 0;
    target_slot_ = -1;
    target_gen_ = 0;
    last_engaged_ms_ = 0;
    // The frame of reference the run starts in: a change from here on is a
    // change, and the one before the start is not.
    view_ = eyes_.view ? eyes_.view() : 0;
    // A new start, a new hunting spot: taken where the character stands at
    // the first look that has it settled, and the map entries counted from
    // that look's. Nothing fought yet, so the idle time counts from here.
    fix_ = WorldFix{};
    spot_mode_ = SpotMode::Off;
    spot_why_ = SpotWhy::None;
    epoch_seen_on_ = false;
    known_x_ = known_y_ = 0.0f;
    login_gap_ = false;
    login_report_ms_ = 0;
    spot_busy_ms_ = clock_now();
    return_on_ = returning_ = returned_last_ = false;
    return_quiet_ms_ = 0;
    return_pass_ = 0;
    return_strikes_ = 0;
    return_retry_ms_ = 0;
    said_too_high_ = false;
    // Nor anything the map made of the area's floors.
    zone_ = ZoneHere{};
    way_asked_ = false;
    way_.reset();
    return_goal_on_ = false;
    said_no_way_ = false;
    minute_walks_kept_ = 0;
    walk_kept_said_ms_ = 0;
    // No area of the map tab's waits either: the tab sends it again for the
    // new run.
    zone_pending_.reset();
    zone_wait_ = ZoneWait::None;
    zone_wait_px_ = 0;
    said_zone_wait_ = false;
    // The drop's run: a settle or a fall under way, the latch, the fails, the
    // limits' counts and the back-off. The press record stays -- a map change
    // after a press is the press's, a start between or not -- and so do the
    // map book's memories, the files found wrong and the map trusted.
    drop_ = DropState{};
    drop_presses_.clear();
    drop_targets_.clear();
    last_press_ms_ = 0;
    last_drop_worked_ms_ = 0;
    settle_backoff_ms_ = 0;
    drop_fails_row_ = drop_fails_run_ = 0;
    drop_latch_ = DropOff::None;
    drop_halt_ = false;
    owed_passes_ = 0;
    drop_stop_.store(DropStop::None);
    drop_said_slot_ = -2;
    drop_said_why_ = DropWhy::None;
    mob_stands_.clear();
    minute_drop_looks_ = minute_drop_settles_ = 0;
    for (int& n : minute_drop_why_) n = 0;
    minute_drops_ = minute_drop_worked_ = minute_drop_nothing_ = minute_drop_jump_ = 0;
    minute_drop_rose_ = minute_drop_off_map_ = minute_drop_gate_ = minute_drop_unknown_ = 0;
    // The jumps up's run likewise (2026.11.05b): a jump under way, the latch,
    // the keys off, the fails and the limits' counts. The press record, the
    // in-air guard and what the map book learned stay -- the floors solid
    // from below, the no-jump points, the session's own.
    jump_ = JumpUpState{};
    last_jump_worked_ms_ = 0;
    jump_backoff_ms_ = 0;
    jump_fails_row_ = jump_fails_run_ = 0;
    jump_latch_ = JumpUpOff::None;
    jump_kinds_off_ = 0;
    for (int& n : jump_kind_misses_) n = 0;
    jump_gravity_out_ = 0;
    jump_presses_.clear();
    jump_targets_.clear();
    way_jumps_ = false;
    jump_dry_way_.reset();
    jump_dry_asked_ = false;
    jump_legs_known_ = false;
    jump_wait_home_ = false;
    jump_said_slot_ = jump_would_slot_ = -2;
    jump_said_why_ = JumpUpWhy::None;
    jump_would_ms_ = 0;
    minute_jump_looks_ = minute_jump_would_ = minute_jump_settles_ = 0;
    for (int& n : minute_jumps_) n = 0;
    minute_jump_worked_ = minute_jump_fell_ = minute_jump_kind_miss_ = minute_jump_nothing_ = 0;
    minute_jump_lower_ = minute_jump_higher_ = minute_jump_unknown_ = minute_jump_gate_ = 0;
    minute_jump_g_sum_ = 0.0;
    minute_jump_g_n_ = 0;
    for (int& n : minute_jump_why_) n = 0;
    // And the seek's (2026.11.05b): the monster gone for, the floors worked
    // out, the passes over, what was said and the minute's counts.
    seek_wanted_ = seek_world_on_ = seek_on_ = false;
    seek_world_.clear();
    seek_map_ = SeekMap{};
    seek_map_known_ = false;
    seek_map_page_ = nullptr;
    seek_looks_.clear();
    seek_looked_ms_ = seek_planned_ms_ = 0;
    seek_ = SeekState{};
    seek_skip_.clear();
    seek_no_drop_.clear();
    drop_why_pass_ = DropWhy::None;
    seek_said_goal_ms_ = 0;
    seek_said_no_way_.clear();
    seek_none_ = false;
    seek_none_why_ = SeekNone::None;
    seek_none_since_ms_ = seek_none_said_ms_ = 0;
    seek_snapshot_ms_ = 0;
    seek_minute_reset();
    pass_at_ms_ = 0;
    disengage();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.seek_active = status_.seek_on = status_.seek_none = false;
    status_.seek_grey_slots.clear();
    status_.seek_grey.clear();
    status_.jump_phase = JumpUpPhase::Idle;
    status_.jumps = status_.jumps_worked = status_.jumps_fell_back = status_.jumps_would = 0;
    status_.jump_would_plan = JumpPlan{};
    status_.jump_would_ms = 0;
    status_.jump_wait = JumpUpWhy::None;
    status_.jump_wait_px = 0;
    status_.jump_wait_no = JumpNo::None;
    status_.jump_wait_ms = 0;
    status_.jump_wait_name.clear();
    status_.jump_off = JumpUpOff::None;
    status_.jump_kinds_off = 0;
    status_.jump_map_changed = false;
    status_.dropping = false;
    status_.drops = status_.drops_worked = 0;
    status_.drop_wait = DropWhy::None;
    status_.drop_wait_px = 0;
    status_.drop_wait_ms = 0;
    status_.drop_off = DropOff::None;
    status_.drop_map_changed = status_.drop_keys_stuck = false;
    spot_ = Spot{};
    retake_asked_ = suspend_asked_ = false;
    zone_asked_.reset();
    zone_clear_asked_.clear();
    zone_aside_asked_.clear();
    status_.only_skipped = false;
    status_.passed_over = false;
    status_.outside_area = false;
    status_.no_target_since_ms = 0;
    status_.spot_set = false;
    status_.spot_suspended = false;
    status_.spot_login = false;
    status_.spot_map_aside = false;
    status_.spot_why = SpotWhy::None;
    status_.spot_dx = status_.spot_dy = 0.0f;
    status_.spot_left = status_.spot_top = status_.spot_right = status_.spot_bottom = 0.0f;
    status_.spot_from_map = false;
    status_.spot_stem.clear();
    status_.spot_epoch = 0;
    status_.zone_wait = ZoneWait::None;
    status_.zone_wait_stem.clear();
    status_.zone_wait_px = 0;
    status_.spot_outside_px = -1;
    status_.spot_home = false;
    status_.spot_way = status_.spot_back_first = false;
    status_.spot_way_x = status_.spot_way_y = 0;
    status_.spot_way_by_jumps = status_.spot_way_jump_first = false;
    status_.spot_way_jumps = status_.spot_way_jump_x = 0;
    // This run's walks back: the card and the Positions line show them
    // beside this run's spot, never an earlier run's.
    status_.returns = 0;
    status_.return_give_ups = 0;
}

void Hunter::forget_positions() {
    // Not loot_left_ = 3, as a target gone from where it was is: the target
    // is not gone, only seen from elsewhere, and three presses of the pick-up
    // key at every switch would be three for nothing.
    have_target_ = false;
    target_first_ = false;
    switched_ = false;
    skip_spots_.clear();
    gave_up_.clear();
    end_fight("view changed");
    watch_ = DamageWatch{};
    // The step back still to be judged goes, being judged by where things
    // stand; the steps counted each way (retreat_) stay -- the character
    // took them, wherever anything is seen from.
    back_ = BackSteps{};
    step_slack_px_ = 0;
    step_slack_until_ms_ = 0;
    just_stepped_ = false;
    // The hunting spot and the walk back to it stay: they are in the
    // packets' world pixels, whichever frame the plates are in.
    //
    // A drop's settle is let go of and its fall not judged; the press record
    // and the in-air guard stay. The map drops go by is trusted again only
    // by standing points, or a new entry (MapTrust::lose).
    drop_hold("view changed");
    jump_hold("view changed");
    trust_.lose("the plates' view changed");
    // The monster the seek goes for is let go of as the target is; what it
    // passed over stays, by the world's own slots (2026.11.05b).
    seek_let_go(nullptr);
}

void Hunter::stop() {
    if (!running_.exchange(false)) return;
    stepped_ = false;
    if (thread_.joinable()) thread_.join();
    disengage();
    end_fight("stopped");
    drop_hold("stopped");
    jump_hold("stopped");
    release_movement();
    // The minute cut short, said when the seek was on in it (2026.11.05b): the
    // last minute of a hunt is what "why idle" is asked of.
    if (minute_seek_passes_ > 0 && minute_from_ms_ != 0) {
        const std::uint64_t now = clock_now();
        GB_INFO("Hunter (last %.1f s, hunt stopped): %d fights%s",
                static_cast<double>(now > minute_from_ms_ ? now - minute_from_ms_ : 0) / 1000.0, minute_fights_,
                seek_minute_text().c_str());
    }
    seek_minute_reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = State::Idle;
        status_.has_target = false;
        status_.only_skipped = false;
        status_.passed_over = false;
        status_.outside_area = false;
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

bool Hunter::hold_down(std::uint32_t vk) {
    return hands_.key_down ? hands_.key_down(vk) : input_.key_down(vk);
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
    set_walk(0);
}

void Hunter::set_walk(std::uint32_t vk) {
    // Followed by the packets while held, and kWalkStopGraceMs after it is let
    // go of, for the stop the game reports a moment after the key comes up.
    if (vk != 0)            walk_until_ms_.store(UINT64_MAX);
    else if (walk_vk_ != 0) walk_until_ms_.store(clock_now() + kWalkStopGraceMs);
    walk_vk_ = vk;
}

void Hunter::let_go_of_moves(const HunterConfig& cfg) {
    // Every key the hunter presses is down and up again inside its own pass,
    // but for a held walk's -- and a key-up that did not go out, the game
    // having lost focus during the hold, leaves a key down too. Only the
    // direction keys, then, and only when down. This used to be
    // release_all() on every pass, which let go of whatever any thread was
    // holding: a potion pressed by the macro was cut off mid-press by the
    // hunter's 60-ms rhythm.
    for (const std::uint32_t vk : { cfg.left_key, cfg.right_key })
        if (vk != 0 && is_down(vk)) let_go(vk);
    set_walk(0);
}

void Hunter::hop(const HunterConfig& cfg, int dy, std::uint64_t now, WalkFor what) {
    // Up and out of reach: a hop is the cheapest thing that might help, and
    // the give-up timer catches the case where it does not. Higher than the
    // band it can hit without jumping -- half of height_tolerance without a
    // style -- and only with a jump key: with none set it never hops.
    //
    // With a hunting spot, never toward a monster: a hop may land the
    // character on the platform above, and it has no way back down to its
    // spot. Toward the spot, when it is more than a floor's slope above --
    // but on a way back the map shows (Floor), never: it goes along the floor
    // the character stands on, and a hop off it leaves the way.
    if (what == WalkFor::Floor) return;
    if (!cfg.jump_to_reach || cfg.jump_key == 0) return;
    // "Nhảy lên bục trên" on, where jumps up may be planned -- the map tab's
    // area in use with its map's file, the packets steering, MapTrust
    // trusting that map, the jump key ↑ (jump_area): every jump there is
    // planned (jump_toward, go_home_zone), never a hop -- off for the run
    // too. Anywhere else (Watch, another jump key, the map not trusted yet)
    // the way back has no jumps in it, and its as_before walk hops as in
    // 2026.11.05 (2026.11.05b).
    if (cfg.jump_up && jump_area(cfg)) return;
    if (what == WalkFor::Target && (cfg.spot || dy >= -cfg.up_nj())) return;
    // Nor toward a monster for kDropStayMs after a drop that worked: the hop
    // would take the character back up the floor it came down from.
    if (what == WalkFor::Target && last_drop_worked_ms_ != 0 && now - last_drop_worked_ms_ < kDropStayMs) return;
    if (what == WalkFor::Spot) {
        const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
        if (dy >= -static_cast<int>(std::lround(kSpotFloorPx / scale))) return;
    }
    if (InputManager::never_pressed(cfg.jump_key)) return;     // ↓: would not go out anyway
    // Not again while the last one is still in the air, walking held or
    // stepping: there it is a second jump.
    if (in_air(now)) return;
    if (press(cfg.jump_key, 50)) {
        last_hop_ms_ = now;
        // Each press, for the log to match against the take-offs (2026.11.05b).
        GB_DEBUG("Hunter: hop (key 0x%02X) toward %s, %d px up", static_cast<unsigned>(cfg.jump_key),
                 what == WalkFor::Target ? "the target" : "the spot", -dy);
    }
}

void Hunter::walk(const HunterConfig& cfg, int dx, int dy, std::uint64_t now, WalkFor what) {
    const std::uint32_t go = dx < 0 ? cfg.left_key : cfg.right_key;
    const std::uint32_t other = dx < 0 ? cfg.right_key : cfg.left_key;
    const Facing way = dx < 0 ? Facing::Left : Facing::Right;

    // The other way already, with the key still held from the last look: let
    // go of it now, input free or not. Letting go takes nothing from anyone,
    // and walking on the wrong way until the input is free would.
    if (walk_vk_ != 0 && walk_vk_ != go) {
        let_go(walk_vk_);
        set_walk(0);
    }

    // Held from one look to the next and still down: on it goes, with nothing
    // to press -- but with the packets to steer by, a walk held kWalkRenewMs
    // is let go of a moment and taken up again, below, for the stop and the
    // start the game then reports. Without them nothing reads those, and the
    // walk goes on unbroken. Down no longer -- the macro let go of every key
    // as the character left the world, the game lost focus a moment -- it is
    // pressed again as a new walk.
    //
    // And taken up again at once when it is getting no closer: kWalkStuckMs
    // with the target never kWalkProgressPx nearer. A walk start the capture
    // missed leaves the packets' you standing, and never uncertain, while the
    // character walks -- 900 px past the target by the renewal; the stop and
    // the start of a renewal put it right inside half a second. A target
    // further off than at the last mark -- another one, or one walking away
    // -- is a new mark.
    //
    // A walk back to the hunting spot is renewed the same way with the
    // packets only watched too: its give-up goes by where the reports put
    // the character, and the packets follow it no further than kOwnReckonMs.
    const int adx = std::abs(dx);
    const bool walking = cfg.hold_walk && walk_vk_ == go && is_down(go);
    if (walking && (adx + kWalkProgressPx <= walk_adx_ || adx > walk_adx_)) {
        walk_adx_ = adx;
        walk_progress_ms_ = now;
    }
    const bool stuck = walking && now - walk_progress_ms_ >= kWalkStuckMs;
    const bool steered = cfg.packet_positions == PacketPositions::On || what != WalkFor::Target;
    const bool renew = stuck || (steered && now - walk_since_ms_ >= kWalkRenewMs);
    const int side = dx < 0 ? 0 : 1;
    if (walking && !renew) {
        if (auto token = coord_.acquire("hunt-move", Priority::Movement)) hop(cfg, dy, now, what);
        note_walk(side, true, now);
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.steps;
        return;
    }

    bool stepped = false;
    if (auto token = coord_.acquire("hunt-move", Priority::Movement)) {
        let_go(other);
        if (!cfg.hold_walk) {
            stepped = press(go, static_cast<int>(cfg.step_ms));
            if (stepped) {
                facing_ = way;
                facing_key_ms_ = clock_now();
                facing_key_way_ = way;
            }
        } else {
            if (walking) {
                let_go(go);
                pause(kWalkRenewGapMs);
            }
            set_walk(0);
            // Not again into a hold begun in that pause -- the trash pass or a
            // revive lets go of every key as it starts, and would have the
            // arrow down again under it -- nor into a game no longer in front.
            const bool may = !walking || (!coord_.critical_active() && focused());
            if (may && hold_down(go)) {
                set_walk(go);
                walk_since_ms_ = clock_now();
                walk_adx_ = adx;
                walk_progress_ms_ = walk_since_ms_;
                facing_ = way;
                facing_key_ms_ = walk_since_ms_;
                facing_key_way_ = way;
            }
        }
        hop(cfg, dy, now, what);

        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.steps;
    }
    // Held, the walk is the key still down -- the input busy leaves a held
    // one down, and one let go of the other way is none.
    note_walk(side, cfg.hold_walk ? walk_vk_ == go : stepped, now);
}

void Hunter::note_walk(int side, bool moved, std::uint64_t now) {
    if (!moved) {
        walk_pass_ = 0;
        return;
    }
    // Looks in a row, the same way: the time between them is walked -- no
    // more than a look and a step take, whatever the clock did meanwhile.
    if (walk_pass_ != 0 && walk_pass_ + 1 == passes_ && walk_side_ == side && now > walk_at_ms_)
        walk_run_ms_ += std::min<std::uint64_t>(now - walk_at_ms_, 250);
    else
        walk_run_ms_ = 0;
    walk_side_ = side;
    walk_pass_ = passes_;
    walk_at_ms_ = now;
    // Walked back this far: the steps back the other way gave up ground this
    // has won back, and a wall or a ledge met that way is not behind it now.
    Retreat& r = retreat_[1 - side];
    if (walk_run_ms_ >= kRegainWalkMs && (r.in_row != 0 || r.blocked)) {
        GB_DEBUG("Hunter: walked %s for %llu ms -- steps back to the %s counted afresh (%d taken%s)",
                 side == 0 ? "left" : "right", static_cast<unsigned long long>(walk_run_ms_),
                 side == 0 ? "right" : "left", r.in_row, r.blocked ? ", one met a wall or a ledge" : "");
        r = Retreat{};
    }
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
                          const Nameplate* me, const std::vector<Listed>& listed, bool* wounded_picked,
                          bool below_first, const std::vector<char>* own_floor) {
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
            // Hurt, and within reach of where the character stands: one
            // across the screen at 90% -- or a bar misread at 0.97 -- beat a
            // whole one in reach, and the character turned its back on what
            // was hitting it. A plate from the packets has its bar lowered by
            // anyone's blows: finished first only once your id is proven the
            // hitter's, and not while another player is hitting it -- theirs
            // to finish, not a kill of yours.
            const bool may_finish = n.slot < 0 || (n.actor_checked && !n.hurt_by_other);
            const bool hurt_here =
                cfg.finish_wounded && may_finish && in_reach(cfg, n.x() - me->x(), n.y() - me->y());
            if (nearest < 0) nearest = at;
            if (hurt_here && (wounded < 0 || n.hp < hp_of(wounded))) wounded = at;
            if (first(i)) {
                if (first_nearest < 0) first_nearest = at;
                if (hurt_here && (first_wounded < 0 || n.hp < hp_of(first_wounded)))
                    first_wounded = at;
            }
        }
        if (first_nearest >= 0) {
            nearest = first_nearest;
            wounded = first_wounded;
        }
        target = nearest;
        // Finish something already hurt instead of starting afresh. Only
        // among those it can reach from where it stands, so this never sends
        // it chasing across the screen.
        if (wounded >= 0 && hp_of(wounded) < 1.0f) {
            if (wounded_picked && wounded != target) *wounded_picked = true;
            target = wounded;
        }
    }

    // Nothing level, with a drop able to go: the nearest from the packets far
    // enough below to be dropped to, before one above that the plates' measure
    // -- height counted three times -- puts nearer, which a hop would chase.
    // The player's answer 1 of 2026-11-04: a monster on the floor below is
    // dropped to when the character's own floor has none.
    if (target < 0 && me && below_first) {
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            const Nameplate& n = mobs[i];
            if (n.slot >= 0 && n.hp > cfg.dead_below && !out(i) &&
                n.y() - me->y() > std::max(cfg.down(), cfg.drop_min_px)) {
                target = static_cast<int>(i);
                break;
            }
        }
    }

    // Nothing level, or no idea where we are: the nearest that still has
    // health, and let the timer decide. With no hop to follow and the map to
    // say (`own_floor`), not one above that only a hop would get to -- the
    // owner's Q3 of 04d: walked under for give_up_ms, the walk carried the
    // character down t52_s2's slope out of the box. One up the character's
    // own floor is walked to as ever.
    const auto unreached = [&](std::size_t i) {
        return kSkipAboveUnreached && own_floor != nullptr && me != nullptr && i < own_floor->size() &&
               (*own_floor)[i] == 0 && mobs[i].y() - me->y() < -cfg.up();
    };
    // One a jump up gets onto (2026.11.05b): last of all -- only once
    // nothing on the level, below or up the character's own floor is left.
    const auto by_jump = [&](std::size_t i) {
        return kJumpUpTargetsLast && own_floor != nullptr && i < own_floor->size() && (*own_floor)[i] == 2;
    };
    if (target < 0) {
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            if (mobs[i].hp > cfg.dead_below && !out(i) && !unreached(i) && !by_jump(i)) {
                target = static_cast<int>(i);
                break;
            }
        }
    }
    if (target < 0) {
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            if (mobs[i].hp > cfg.dead_below && !out(i) && by_jump(i)) {
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

int Hunter::nearest_off_level(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                              const std::vector<Listed>& listed) {
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.hp <= cfg.dead_below) continue;
        if (i < listed.size() && refused(listed[i])) continue;
        const int dy = n.y() - me.y();
        if (level_with(cfg, dy)) return 0;
        return dy > 0 ? 1 : -1;
    }
    return 0;
}

bool Hunter::below_in_reach(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                            const std::vector<Listed>& listed) {
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.hp <= cfg.dead_below) continue;
        if (i < listed.size() && refused(listed[i]) && listed[i] != Listed::GaveUp) continue;
        if (n.y() - me.y() > cfg.down() && std::abs(n.x() - me.x()) <= cfg.reach) return true;
    }
    return false;
}

void Hunter::note_pick(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                       const std::vector<Listed>& listed) {
    ++minute_picks_;
    const int off = nearest_off_level(cfg, mobs, me, listed);
    if (off > 0) ++minute_nearest_below_;
    if (off < 0) ++minute_nearest_above_;
}

void Hunter::note_wait(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                       const std::vector<Listed>& listed, std::uint64_t now) {
    if (!below_in_reach(cfg, mobs, me, listed)) return;
    // From the last pass that waited so to this one: a pass between that did
    // not breaks the run, and no gap counts for more than 250 ms.
    if (below_wait_pass_ != 0 && below_wait_pass_ + 1 == passes_ && now > below_wait_at_ms_)
        minute_below_wait_ms_ += std::min<std::uint64_t>(now - below_wait_at_ms_, 250);
    below_wait_pass_ = passes_;
    below_wait_at_ms_ = now;
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
        if (listed[i] == Listed::Free || (s.beyond && listed[i] == Listed::First))
            listed[i] = Listed::GaveUp;
    }
}

// ---------------------------------------------------------------- a fight

void Hunter::track_fight(const HunterConfig& cfg, const Nameplate& target, std::uint64_t now,
                         int slack) {
    FightLog& f = fight_;
    // Somewhere other than where the target was, or after longer than the
    // character goes unseen for a moment: another monster, another fight.
    if (f.on && !same_monster(target, f.x, f.y, f.slot, f.gen, f.seen_ms, now, slack)) end_fight("another target");
    if (!f.on) {
        f = FightLog{};
        f.on = true;
        f.keep_away = keep_away_px(cfg);
    }
    f.x = target.x();
    f.y = target.y();
    f.slot = target.slot;
    f.gen = target.gen;
    f.seen_ms = now;
}

void Hunter::aim_at(const Nameplate& n) {
    target_x_ = n.x();
    target_y_ = n.y();
    target_slot_ = n.slot;
    target_gen_ = n.gen;
}

int Hunter::find_target(const std::vector<Nameplate>& mobs, int slack, bool* respawned, int* nearest) const {
    // The plate nearest where the target was: the rule for the screen's
    // plates, and what that rule would have taken.
    int near_at = -1, near_d = 0;
    bool slots = false;
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.slot >= 0) slots = true;
        const int d = std::abs(n.x() - target_x_) + std::abs(n.y() - target_y_);
        if (near_at < 0 || d < near_d) {
            near_at = static_cast<int>(i);
            near_d = d;
        }
    }
    if (nearest) *nearest = near_at;
    if (target_slot_ < 0 || !slots) return near_at >= 0 && near_d <= kSameTargetPx + slack ? near_at : -1;

    // From the packets: its slot, and in it the same monster. A plate nearer
    // where it was is another one walking past; its slot with a new
    // generation is the next monster, spawned where it died.
    int at = -1, at_d = 0;
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.slot != target_slot_) continue;
        if (n.gen != target_gen_) {
            if (respawned) *respawned = true;
            continue;
        }
        const int d = std::abs(n.x() - target_x_) + std::abs(n.y() - target_y_);
        if (d > kSlotSamePx + slack) continue;
        if (at < 0 || d < at_d) {
            at = static_cast<int>(i);
            at_d = d;
        }
    }
    return at;
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

void Hunter::note_attack(int dx) {
    // An attack from far enough is no ground won back, so the steps back are
    // not counted afresh here -- only by a walk the other way (note_walk).
    const int adx = std::abs(dx);
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
    // monster, by which the step cannot be judged. A fight ended and begun
    // again on the same one -- its bar misread as empty -- is not. The steps
    // counted each way go on either way (retreat_).
    if (b.seen_ms != 0 && !same_monster(target, b.x, b.y, b.slot, b.gen, b.seen_ms, now, slack)) b = BackSteps{};
    b.x = target.x();
    b.y = target.y();
    b.slot = target.slot;
    b.gen = target.gen;
    b.seen_ms = now;
}

void Hunter::follow_watch(const Nameplate& target, std::uint64_t now, int slack) {
    DamageWatch& w = watch_;
    // As the steps back: another monster, or this one gone too long, and the
    // watch begins again. A plate missing for a look, or a bar misread as
    // empty once, leaves it where it was.
    if (w.seen_ms != 0 && !same_monster(target, w.x, w.y, w.slot, w.gen, w.seen_ms, now, slack)) w = DamageWatch{};
    w.x = target.x();
    w.y = target.y();
    w.slot = target.slot;
    w.gen = target.gen;
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
    // A plate from the packets says when you last hurt it: a blow of yours
    // since the last look is damage too, though a big monster's bar moves
    // less than kDamageStep for it. The screen's plates have only the bar.
    if (target.slot >= 0 && target.hurt_ms > w.hurt_seen_ms) {
        w.hurt_seen_ms = target.hurt_ms;
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
    std::uint64_t dropped = 0, repicked = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (minute_from_ms_ == 0) {
            minute_from_ms_ = now;
            minute_dropped_ = status_.targets_dropped;
            minute_repicked_ = status_.targets_repicked;
            return;
        }
        if (now - minute_from_ms_ < 60'000) return;
        dropped = status_.targets_dropped - minute_dropped_;
        minute_dropped_ = status_.targets_dropped;
        repicked = status_.targets_repicked - minute_repicked_;
        minute_repicked_ = status_.targets_repicked;
    }
    // Of those given up, the ones picked straight back, in reach a moment
    // before: said apart, or every fight's re-pick reads as a monster out of
    // reach -- ten a minute in the farm of 2026-11-03, when none was.
    char repicks[128] = "";
    if (repicked > 0 && !cfg.goc_rules())
        std::snprintf(repicks, sizeof repicks, " (%llu of them in reach: re-picked every %u s, Như trước)",
                      static_cast<unsigned long long>(repicked), cfg.give_up_ms / 1000);
    else if (repicked > 0)
        std::snprintf(repicks, sizeof repicks,
                      " (%llu of them picked straight back: a hold or the game behind, not out of reach)",
                      static_cast<unsigned long long>(repicked));
    // With the hunting spot on, the walks back to it and those given up on too.
    // Last, the turns to a target the hunter tapped, and the times the
    // packets put right which way it took the character to face -- Left or
    // Right it had, the other way they said; not the Unknown a hold, the game
    // out of focus or the character unseen leaves, which they fill in at
    // every one of those. With a turn tapped only when it is not facing the
    // target already, put right is a character turned by something else, or
    // a key of the hunter's own the game never took -- which the last number
    // counts on its own (kFacingMissedMs).
    const bool turned = minute_turns_ > 0 || minute_packet_facings_ > 0;
    // Then the most wounded taken over the nearest; and with the packets'
    // plates, the target kept by its slot, the swaps that refused, and the
    // respawns in its slot not taken for it.
    const bool slotted = minute_slot_kept_ > 0 || minute_swaps_refused_ > 0 || minute_new_gen_ > 0;
    char packets[160] = "";
    if (slotted)
        std::snprintf(packets, sizeof packets,
                      "; with the packets' plates: slot kept %d, swaps refused %d, new generation %d",
                      minute_slot_kept_, minute_swaps_refused_, minute_new_gen_);
    // And for the drop-down to come: the picks whose nearest monster stood
    // off the level, and the time spent waiting, nothing in reach, with one
    // below within reach across -- each only when there was any.
    char levels[256] = "";
    int used = 0;
    if (minute_nearest_below_ > 0 || minute_nearest_above_ > 0)
        used = std::snprintf(levels, sizeof levels, "; nearest off level: below %d, above %d of %d picks",
                             minute_nearest_below_, minute_nearest_above_, minute_picks_);
    if (minute_below_wait_ms_ > 0 && used >= 0 && static_cast<std::size_t>(used) < sizeof levels)
        std::snprintf(levels + used, sizeof levels - static_cast<std::size_t>(used),
                      "; waiting while a monster below was in reach: %.1f s",
                      static_cast<double>(minute_below_wait_ms_) / 1000.0);
    // And the walks toward a target the map tab's area kept in its box, when
    // any was.
    if (minute_walks_kept_ > 0) {
        const std::size_t at = std::strlen(levels);
        if (at < sizeof levels)
            std::snprintf(levels + at, sizeof levels - at, "; walks stopped at the hunting area's edge %d",
                          minute_walks_kept_);
    }
    // And the drops this minute, only when one was pressed or a target below
    // judged: what came of each, and what refused it. Apart from the given
    // up: a target a drop passed over is not one.
    // And the jumps up likewise (2026.11.05b): appended to the drops' part.
    // And the seek's (2026.11.05b): whenever it was on this minute -- what it
    // went for, and where the minute's time went.
    const std::string drops = drop_minute_text() + jump_minute_text() + seek_minute_text();
    const bool more = minute_wounded_ > 0 || slotted || levels[0] != '\0' || !drops.empty();
    if (cfg.spot && (minute_fights_ > 0 || minute_backsteps_ > 0 || dropped > 0 || minute_returns_ > 0 ||
                     minute_return_give_ups_ > 0 || turned || more))
        GB_INFO("Hunter: %d fights, %d with attacks at >= %d px, %d back-steps, %llu given up%s, "
                "%d walks back to the spot, %d of them given up, "
                "%d turns (%d facings put right by the packets, %d of them keys the game missed), "
                "wounded picked %d%s%s%s",
                minute_fights_, minute_far_, keep_away_px(cfg), minute_backsteps_,
                static_cast<unsigned long long>(dropped), repicks, minute_returns_, minute_return_give_ups_,
                minute_turns_, minute_packet_facings_, minute_missed_keys_, minute_wounded_, packets, levels,
                drops.c_str());
    else if (!cfg.spot && (minute_fights_ > 0 || minute_backsteps_ > 0 || dropped > 0 || turned || more))
        GB_INFO("Hunter: %d fights, %d with attacks at >= %d px, %d back-steps, %llu given up%s, "
                "%d turns (%d facings put right by the packets, %d of them keys the game missed), "
                "wounded picked %d%s%s%s",
                minute_fights_, minute_far_, keep_away_px(cfg), minute_backsteps_,
                static_cast<unsigned long long>(dropped), repicks, minute_turns_, minute_packet_facings_,
                minute_missed_keys_, minute_wounded_, packets, levels, drops.c_str());
    minute_from_ms_ = now;
    minute_fights_ = minute_far_ = minute_backsteps_ = 0;
    minute_returns_ = minute_return_give_ups_ = 0;
    minute_turns_ = minute_packet_facings_ = minute_missed_keys_ = 0;
    minute_wounded_ = minute_slot_kept_ = minute_swaps_refused_ = minute_new_gen_ = 0;
    minute_picks_ = minute_nearest_below_ = minute_nearest_above_ = 0;
    minute_below_wait_ms_ = 0;
    minute_walks_kept_ = 0;
    minute_drop_looks_ = minute_drop_settles_ = 0;
    for (int& n : minute_drop_why_) n = 0;
    minute_drops_ = minute_drop_worked_ = minute_drop_nothing_ = minute_drop_jump_ = 0;
    minute_drop_rose_ = minute_drop_off_map_ = minute_drop_gate_ = minute_drop_unknown_ = 0;
    minute_jump_looks_ = minute_jump_would_ = minute_jump_settles_ = 0;
    for (int& n : minute_jumps_) n = 0;
    minute_jump_worked_ = minute_jump_fell_ = minute_jump_kind_miss_ = minute_jump_nothing_ = 0;
    minute_jump_lower_ = minute_jump_higher_ = minute_jump_unknown_ = minute_jump_gate_ = 0;
    minute_jump_g_sum_ = 0.0;
    minute_jump_g_n_ = 0;
    for (int& n : minute_jump_why_) n = 0;
    seek_minute_reset();
}

void Hunter::face_from_packets(const HunterConfig& cfg) {
    // Only with the packets steering. Watched only, the screen fights, as
    // hunt.ini says of packet_positions=watch: the hunter's own idea of the
    // facing stands, as with no capture at all.
    if (!eyes_.where || cfg.packet_positions != PacketPositions::On) return;
    // With the spot on, the fix track_spot took at the start of this pass --
    // not asked again, and fix_ left as the spot has it. Off, asked here.
    WorldFix f;
    if (cfg.spot) f = fix_;
    else if (!eyes_.where(f)) return;
    // Only a fix that knows the character, with a report that said which way
    // it faces. Lost, or nothing said yet, and the hunter's own idea stands --
    // the screen's memory, with its resets.
    if (f.state != WorldFix::State::Known || f.facing == 0) return;
    const Facing said = f.facing > 0 ? Facing::Right : Facing::Left;
    if (said == facing_) return;
    // Taken at once when the report came more than kFacingTrustMs after the
    // hunter's own last key that turned the character: the character as that
    // key left it, or as something since turned it. Sooner, it may have been
    // sent before the key reached the game -- taken all the same when it says
    // the way the key went, which is so either way: the key's own report, the
    // last there is when a hold has left facing_ not known. Saying the other
    // way, it is the character as it faced before the key; and with no report
    // since for kFacingMissedMs, the game never took the key, which would
    // have sent one -- facing_ goes back, and the next pass turns it again.
    const bool after_key = f.facing_ms > facing_key_ms_ + kFacingTrustMs;
    const bool as_key = said == facing_key_way_;
    const bool missed = !after_key && !as_key && clock_now() > facing_key_ms_ + kFacingMissedMs;
    if (!after_key && !as_key && !missed) return;
    GB_DEBUG("Hunter: the packets have the character facing %s -- the hunter %s; %s",
             said == Facing::Right ? "right" : "left",
             facing_ == Facing::Unknown  ? "did not know which way"
             : facing_ == Facing::Right ? "took it to face right"
                                        : "took it to face left",
             after_key ? "a report since its last turning key"
             : as_key  ? "the way its last turning key went"
                       : "the game never took its last turning key");
    // Counted only where the hunter had it Left or Right: filling in a facing
    // not known is no mistake put right.
    if (facing_ != Facing::Unknown) {
        ++minute_packet_facings_;
        if (missed) ++minute_missed_keys_;
    }
    facing_ = said;
}

// ---------------------------------------------------------------- the spot

void Hunter::track_spot(const HunterConfig& cfg) {
    fix_ = WorldFix{};
    if (!eyes_.where || !eyes_.where(fix_)) fix_ = WorldFix{};
    const WorldFix& f = fix_;

    Spot spot;
    bool retake = false, suspend = false;
    std::optional<Zone> zone_ask;
    std::string clear_ask, aside_ask;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        spot = spot_;
        retake = retake_asked_;
        suspend = suspend_asked_;
        retake_asked_ = suspend_asked_ = false;
        zone_ask.swap(zone_asked_);
        clear_ask.swap(zone_clear_asked_);
        aside_ask.swap(zone_aside_asked_);
    }

    // "Đặt chỗ này": taken again below, at the first settled look -- this one
    // if it is. Whatever stood in the way of the old one goes with it, and an
    // area of the map tab's waiting: the tab asks it only where none is to
    // be had (a map it does not know, or none drawn for it).
    if (retake) {
        spot = Spot{};
        return_strikes_ = 0;
        return_retry_ms_ = 0;
        said_too_high_ = false;
        end_return();
        zone_pending_.reset();
        GB_INFO("Hunter: Đặt chỗ này -- the hunting spot is taken again where the character stands");
    }
    // Logged back in -- by the keeper, or a game opened again and found: it
    // may be anywhere now. Aside as a log in: back in use once the character
    // stands near where it was, below. One a map entry set aside already
    // stays as that entry left it: the character was on another map before
    // the log in, and only coming back through the door brings it back --
    // standing near where it left from, on that other map, would not.
    if (suspend && spot.set) {
        if (!spot.suspended) {
            spot.suspended = true;
            spot.login = true;
            spot.left_x = known_x_;
            spot.left_y = known_y_;
            login_gap_ = false;
            login_report_ms_ = f.report_ms;
            end_return();
            GB_INFO("Hunter: logged back in -- the hunting spot (%d,%d) waits until the character stands within "
                    "%d px of %d,%d, where it was last known, or of the spot",
                    static_cast<int>(std::lround(spot.x)), static_cast<int>(std::lround(spot.y)), kSpotBackPx,
                    static_cast<int>(std::lround(spot.left_x)), static_cast<int>(std::lround(spot.left_y)));
        } else if (!spot.login && !spot.map_aside) {
            GB_INFO("Hunter: logged back in -- the hunting spot stays aside after its map entry, until the "
                    "character comes back through a door near %d,%d",
                    static_cast<int>(std::lround(spot.left_x)), static_cast<int>(std::lround(spot.left_y)));
        }
    }
    // Whether the packets put the character on a place that cannot be the
    // spot's: another than the spot's own, or the one the entry that set it
    // aside led to. A place they do not know is not judged.
    const auto elsewhere = [&f](const Spot& s) {
        if (!f.map_known) return false;
        if (s.map_known) return s.map != f.map || s.area != f.area;
        return s.away_known && s.away_map == f.map && s.away_area == f.area;
    };
    // Back in use, on the place the packets say it is when they know it.
    const auto back_in_use = [this, &f](Spot& s) {
        s.suspended = false;
        s.login = false;
        s.epoch = f.epoch;
        if (f.map_known) {
            s.map_known = true;
            s.map = f.map;
            s.area = f.area;
        }
        return_strikes_ = 0;
        return_retry_ms_ = 0;
        said_too_high_ = false;
    };

    // Map entries since the last pass. Only a fix from a capture counts them:
    // with none, the number means nothing. The first such look is where the
    // count starts, not an entry.
    if (f.state != WorldFix::State::NoCapture) {
        if (!epoch_seen_on_) {
            epoch_seen_on_ = true;
            epoch_seen_ = f.epoch;
        } else if (f.epoch != epoch_seen_) {
            epoch_seen_ = f.epoch;
            const int ex = static_cast<int>(std::lround(f.entry_x)), ey = static_cast<int>(std::lround(f.entry_y));
            if (spot.set && !spot.suspended && spot.epoch != f.epoch) {
                // Through a door, or put somewhere by the server: the spot's
                // world pixels may be another map's now. Where it was last
                // known is where it left from, and where the entry led is a
                // place it is not.
                spot.suspended = true;
                spot.login = false;
                spot.left_x = known_x_;
                spot.left_y = known_y_;
                spot.away_known = f.map_known;
                spot.away_map = f.map;
                spot.away_area = f.area;
                end_return();
                GB_INFO("Hunter: map entry %u/%u at %d,%d -- the hunting spot waits until the character is back "
                        "near %d,%d, where it left from",
                        static_cast<unsigned>(f.map), static_cast<unsigned>(f.area), ex, ey,
                        static_cast<int>(std::lround(spot.left_x)), static_cast<int>(std::lround(spot.left_y)));
            } else if (spot.set && spot.suspended && !spot.map_aside) {
                // A later entry: back where it left from -- the door there
                // and back -- or somewhere else. Set aside by a log in, an
                // entry is a look from after it either way. Set aside by the
                // map tab, only the tab's own set_zone brings it back.
                if (spot.login) login_gap_ = true;
                const double off = std::hypot(static_cast<double>(f.entry_x - spot.left_x),
                                              static_cast<double>(f.entry_y - spot.left_y));
                if (off <= kSpotBackPx && !elsewhere(spot)) {
                    back_in_use(spot);
                    GB_INFO("Hunter: map entry %u/%u at %d,%d, %d px from where the character left -- the "
                            "hunting spot is in use again",
                            static_cast<unsigned>(f.map), static_cast<unsigned>(f.area), ex, ey,
                            static_cast<int>(std::lround(off)));
                } else {
                    // Somewhere else, or nowhere the packets can tell is the
                    // spot's: from now on only a door back where it left from
                    // brings it back -- even one a log in had set aside:
                    // standing near where it was, here, is another map's
                    // pixels. On the spot's own map, known so, a log in's
                    // rule stands: it may walk back.
                    if (!(spot.map_known && f.map_known && spot.map == f.map && spot.area == f.area)) {
                        spot.login = false;
                        spot.away_known = f.map_known;
                        spot.away_map = f.map;
                        spot.away_area = f.area;
                    }
                    GB_INFO("Hunter: map entry %u/%u at %d,%d, %d px from where the character left -- the "
                            "hunting spot stays aside (Đặt chỗ này sets one here)",
                            static_cast<unsigned>(f.map), static_cast<unsigned>(f.area), ex, ey,
                            static_cast<int>(std::lround(off)));
                }
            }
        }
    }
    if (f.state == WorldFix::State::Known) {
        known_x_ = f.x;
        known_y_ = f.y;
    }

    // Set aside by a log in: back in use once the character stands, settled,
    // within kSpotBackPx of where it was or of the spot -- it came back where
    // it was -- with the fix's count of map entries for its own. Only by a
    // look from after the log in (login_gap_): the new game's capture begins
    // knowing nothing of the character, and a fix from before it is the old
    // game's -- but a report newer than the last before it is the same game's
    // own, found again with its capture going on. Never on a place the
    // packets know is another.
    if (spot.set && spot.suspended && spot.login && !spot.map_aside) {
        if (f.state != WorldFix::State::Known || (f.report_ms != 0 && f.report_ms > login_report_ms_))
            login_gap_ = true;
        if (f.state == WorldFix::State::Known && login_gap_ && f.settled() && !elsewhere(spot)) {
            const double off = std::min(
                std::hypot(static_cast<double>(f.x - spot.left_x), static_cast<double>(f.y - spot.left_y)),
                std::hypot(static_cast<double>(f.x - spot.x), static_cast<double>(f.y - spot.y)));
            if (off <= kSpotBackPx) {
                back_in_use(spot);
                GB_INFO("Hunter: logged back in, the character stands at %d,%d, %d px from where it was or from the "
                        "spot -- the hunting spot is in use again",
                        static_cast<int>(std::lround(f.x)), static_cast<int>(std::lround(f.y)),
                        static_cast<int>(std::lround(off)));
            }
        }
    }

    // The map tab's hunting areas: a delete, an aside, and the area waiting
    // -- taken here when it applies, before a spot is taken where the
    // character stands.
    track_zone(spot, std::move(zone_ask), clear_ask, aside_ask);

    // None yet: where the character stands, once it stands there -- not in
    // mid-jump, where y holds at the top, nor mid-walk, worked out. So too
    // while an area of the map tab's waits for the character to come to it.
    if (cfg.spot && !spot.set && f.settled()) {
        spot = Spot{};
        spot.set = true;
        spot.set_half(f.x, f.y, cfg.spot_half_w, cfg.spot_half_h);
        spot.epoch = f.epoch;
        spot.map_known = f.map_known;
        spot.map = f.map;
        spot.area = f.area;
        return_strikes_ = 0;
        return_retry_ms_ = 0;
        said_too_high_ = false;
        GB_INFO("Hunter: hunting spot set where the character stands: %d,%d (+-%d / +-%d)",
                static_cast<int>(std::lround(spot.x)), static_cast<int>(std::lround(spot.y)), spot.half_w,
                spot.half_h);
    }

    // What the pass makes of it. With no report of yours since the capture
    // began the packets cannot place the character, and standing would never
    // send one: hunted as without a spot, the first walk's report places it.
    // Lost, no capture, suspended, or none to be had yet: standing, attacking
    // what comes within reach, as stay_put does.
    if (f.state == WorldFix::State::FirstReport && !spot.suspended) {
        spot_mode_ = SpotMode::Free;
        spot_why_ = SpotWhy::WaitingFirst;
    } else if (f.state == WorldFix::State::NoCapture) {
        spot_mode_ = SpotMode::Stand;
        spot_why_ = SpotWhy::NoFix;
    } else if (f.state == WorldFix::State::Lost) {
        spot_mode_ = SpotMode::Stand;
        spot_why_ = SpotWhy::Paused;
    } else if (spot.suspended) {
        spot_mode_ = SpotMode::Stand;
        spot_why_ = SpotWhy::Suspended;
    } else if (!spot.set) {
        spot_mode_ = SpotMode::Stand;
        spot_why_ = SpotWhy::Settling;
    } else {
        spot_mode_ = SpotMode::Use;
        // In use -- with what the walk back last met still said through a
        // fight, where go_home is not reached to say it: given up on, to be
        // tried again, or too high. A spot set or back in use again has all
        // three cleared, and is None.
        spot_why_ = return_strikes_ >= kSpotMaxTries                         ? SpotWhy::GaveUp
                    : return_retry_ms_ != 0 && clock_now() < return_retry_ms_ ? SpotWhy::Retrying
                    : said_too_high_                                          ? SpotWhy::TooHigh
                                                                              : SpotWhy::None;
    }
    if (spot_mode_ != SpotMode::Use) end_return();

    // The map tab's area against its map's file: where the character stands
    // on it, how far outside the box, and the way back -- at every pass, a
    // fight on or not. go_home alone said "too high", and on 2026-11-04 it
    // was never reached: the card said "in use" in green from 522 px off.
    // Judged from a standing point -- this pass, or carried through a jump:
    // a jump attack in a fight would have the card flicker.
    track_zone_way(cfg, spot);
    if (spot_mode_ == SpotMode::Use && zone_.page && zone_.judged && spot_why_ == SpotWhy::None && !zone_.home &&
        !zone_.way)
        spot_why_ = SpotWhy::NoReturn;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        spot_ = spot;
        // Not to a suspended one: its pixels may be another map's.
        status_.spot_dx = status_.spot_dy = 0.0f;
        if (spot.set && !spot.suspended && f.state == WorldFix::State::Known) {
            status_.spot_dx = spot.x - f.x;
            status_.spot_dy = spot.y - f.y;
        }
        const bool use = spot_mode_ == SpotMode::Use && f.state == WorldFix::State::Known;
        status_.spot_outside_px = use ? zone_.outside_px : -1;
        status_.spot_home = use && (zone_.page ? zone_.home
                                               : std::abs(spot.x - f.x) <= static_cast<float>(cfg.spot_tolerance));
        status_.spot_way = use && zone_.way.has_value();
        status_.spot_way_x = status_.spot_way ? zone_.way->at.x : 0;
        status_.spot_way_y = status_.spot_way ? zone_.way->at.y : 0;
        status_.spot_back_first = use && zone_.back_first;
        // With jumps up able to go: "walking, falling or jumping up", and the
        // jump the way starts with (2026.11.05b).
        status_.spot_way_by_jumps = use && zone_.judged && way_jumps_;
        status_.spot_way_jumps = status_.spot_way ? zone_.way->jumps : 0;
        status_.spot_way_jump_first = status_.spot_way && zone_.way->jump.has_value();
        status_.spot_way_jump_kind = status_.spot_way_jump_first ? zone_.way->jump->kind : JumpKind::Up;
        status_.spot_way_jump_x = status_.spot_way_jump_first ? zone_.way->jump->x : 0;
    }
    publish_spot();
}

void Hunter::track_zone_way(const HunterConfig& cfg, const Spot& spot) {
    const WorldFix& f = fix_;
    ZoneHere z;
    if (spot_mode_ != SpotMode::Use || f.state != WorldFix::State::Known) {
        zone_ = z;
        return;
    }
    z.box.home_x = round_px(spot.x);
    z.box.home_y = round_px(spot.y);
    z.box.left = round_px(spot.left);
    z.box.top = round_px(spot.top);
    z.box.right = round_px(spot.right);
    z.box.bottom = round_px(spot.bottom);
    z.page = zone_page(spot);
    if (z.page) {
        z.stand = stand_on(*z.page, f, true);
        z.report = stand_on(*z.page, f, false);
    }
    // How far outside: by where it stands on its floor, or by the fix -- this
    // run's spot's box too, for the Positions line.
    const MapPoint at = z.stand ? *z.stand : MapPoint{ round_px(f.x), round_px(f.y) };
    z.outside_px = zone_outside_px(z.box, at.x, at.y);
    if (z.page) {
        z.home = zone_at_home(z.page.get(), z.box, at, cfg.spot_tolerance);
        if (z.stand) {
            // Asked of the map only when where it stands, or the box, has
            // changed: standing, once. With jumps up able to go (2026.11.05b)
            // the way may jump straight up, kZoneReturnJumpsMax times -- and
            // is asked again when they come or go, or a point or a floor is
            // learned.
            const bool jumps = jump_ready(cfg, z.page.get()) && cfg.jump_up && jump_latch_ == JumpUpOff::None;
            if (!way_asked_ || way_from_.x != z.stand->x || way_from_.y != z.stand->y || !same_place(way_box_, z.box) ||
                way_jumps_ != jumps || (jumps && way_epoch_ != jump_epoch_)) {
                if (jumps) {
                    const JumpRules up = jump_rules(cfg, z.page);
                    way_ = return_point(*z.page, z.box, *z.stand, &up);
                } else {
                    way_ = return_point(*z.page, z.box, *z.stand);
                }
                way_asked_ = true;
                way_from_ = *z.stand;
                way_box_ = z.box;
                way_jumps_ = jumps;
                way_epoch_ = jump_epoch_;
                jump_dry_asked_ = false;
            }
            z.way = way_;
            z.judged = true;
        } else if (zone_.page == z.page && same_place(zone_.box, z.box) &&
                   (f.airborne || f.moving || in_air(clock_now()))) {
            // In the air, or walked past its floor's end before the landing's
            // report: the way last worked out stands until it lands -- and what
            // it made of the way back, NoReturn too, but for a report on none
            // of the file's floors. Standing on none of them: no way -- go_home
            // goes as it always did.
            z.way = zone_.way;
            z.judged = zone_.judged && (f.airborne || z.report.has_value());
            // In the air: how far outside, as judged where it took off. The
            // arc may pass above the box's top (the owner's Q3 of 05b) -- a
            // Ctrl + ↑ off the ramp's foot peaks 60 px and more over it -- and
            // the walk back first would come on mid-flight, the seek's goal
            // let go of. Not on the ground past a floor's end the file lacks:
            // that may really leave the box.
            if (f.airborne || in_air(clock_now())) z.outside_px = zone_.outside_px;
        }
        // The owner's Q2 (zone_walks_back_first): outside, with a way back
        // the map shows, the walk back first -- but not while a walk back
        // given up on waits to be tried again, nor once kSpotMaxTries have
        // got nowhere: what is in reach outside is fought then, as in 04c, or
        // the character stands out there and never attacks.
        z.back_first = zone_walks_back_first(z.outside_px, z.way) && return_strikes_ < kSpotMaxTries &&
                       (return_retry_ms_ == 0 || clock_now() >= return_retry_ms_);
    }
    zone_ = std::move(z);
}

void Hunter::track_zone(Spot& spot, std::optional<Zone> asked, const std::string& clear, const std::string& aside) {
    const WorldFix& f = fix_;
    // Deleted on the tab: a spot from it is dropped -- taken again where the
    // character stands, by the caller -- and one waiting goes with it.
    if (!clear.empty()) {
        if (zone_pending_ && zone_pending_->stem == clear) zone_pending_.reset();
        if (spot.set && spot.from_map && spot.stem == clear) {
            spot = Spot{};
            return_strikes_ = 0;
            return_retry_ms_ = 0;
            said_too_high_ = false;
            end_return();
            GB_INFO("Hunter: the map tab's hunting area for %s was deleted -- the spot is taken where the character "
                    "stands",
                    clear.c_str());
        }
    }
    // The tab no longer places the character on that map -- a capture
    // started again, its name let go of or another -- so the area's pixels
    // may be another map's: a spot from it stands aside as at a map entry,
    // where the character was last known kept, until the tab sends the area
    // again. One waiting goes.
    if (!aside.empty()) {
        if (zone_pending_ && zone_pending_->stem == aside) zone_pending_.reset();
        if (spot.set && spot.from_map && spot.stem == aside && !spot.suspended) {
            spot.suspended = true;
            spot.login = false;
            spot.map_aside = true;
            spot.left_x = known_x_;
            spot.left_y = known_y_;
            end_return();
            GB_INFO("Hunter: the map tab no longer places the character on %s -- its hunting area stands aside "
                    "until the map is known again",
                    aside.c_str());
        }
    }
    if (asked) {
        zone_pending_ = std::move(asked);
        said_zone_wait_ = false;
    }

    zone_wait_ = ZoneWait::None;
    zone_wait_px_ = 0;
    if (!zone_pending_) return;
    const Zone& z = *zone_pending_;
    // For a map entry still to come -- the tab saw it first -- or nothing to
    // count them by: kept. For one gone by: the character may be anywhere.
    if (f.state == WorldFix::State::NoCapture || z.epoch > f.epoch) {
        zone_wait_ = ZoneWait::Ahead;
        return;
    }
    if (z.epoch < f.epoch) {
        GB_DEBUG("Hunter: hunting area for %s not taken: made for map entry %u, now %u", z.stem.c_str(),
                 static_cast<unsigned>(z.epoch), static_cast<unsigned>(f.epoch));
        zone_pending_.reset();
        return;
    }
    if (!f.settled()) {
        zone_wait_ = ZoneWait::NotSettled;
        return;
    }
    // The owner's Q1 (zone_take): the one in use edited, at once; otherwise
    // only with the character inside it or near it, or back near where a
    // spot of it was left -- a door there and back, a log in, an aside.
    const HuntZone box = zone_box(z);
    const bool ours = spot.set && spot.from_map && spot.stem == z.stem;
    const bool edit = ours && !spot.suspended;
    const bool back = ours && spot.suspended &&
                      std::hypot(static_cast<double>(f.x - spot.left_x), static_cast<double>(f.y - spot.left_y)) <=
                          static_cast<double>(kZoneTakePx);
    const ZoneTake take = zone_take(box, f.x, f.y, edit, back);
    if (take == ZoneTake::Wait) {
        zone_wait_ = ZoneWait::Outside;
        zone_wait_px_ = zone_outside_px(box, round_px(f.x), round_px(f.y));
        if (!said_zone_wait_) {
            said_zone_wait_ = true;
            GB_INFO("Hunter: the map tab's hunting area for %s waits -- the character stands %d px outside it (map "
                    "entry %u)",
                    z.stem.c_str(), zone_wait_px_, static_cast<unsigned>(f.epoch));
        }
        return;
    }
    // The one in use sent again as it is: nothing to change -- what its
    // walks back met stays said.
    if (edit && spot.x == z.home_x && spot.y == z.home_y && spot.left == z.left && spot.top == z.top &&
        spot.right == z.right && spot.bottom == z.bottom) {
        zone_pending_.reset();
        return;
    }
    Spot s;
    s.set = true;
    s.x = z.home_x;
    s.y = z.home_y;
    s.left = z.left;
    s.top = z.top;
    s.right = z.right;
    s.bottom = z.bottom;
    s.half_w = round_px((z.right - z.left) / 2.0f);
    s.half_h = round_px((z.bottom - z.top) / 2.0f);
    s.epoch = f.epoch;
    s.map_known = f.map_known;
    s.map = f.map;
    s.area = f.area;
    s.from_map = true;
    s.stem = z.stem;
    spot = std::move(s);
    return_strikes_ = 0;
    return_retry_ms_ = 0;
    said_too_high_ = false;
    end_return();
    GB_INFO("Hunter: hunting area from the map tab: %s, %s (map entry %u; %s)", spot.stem.c_str(),
            zone_text(box).c_str(), static_cast<unsigned>(f.epoch), zone_take_name(take));
    zone_pending_.reset();
}

void Hunter::mark_out_of_area(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                              int target_at, std::vector<Listed>& listed) const {
    Spot spot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        spot = spot_;
    }
    // A plate's place in the packets' world: the fix, and the plate's
    // distance from yours laid back over the world's pixels -- what
    // PacketEyes made of it, undone (packet_scale both ways, packet_dy on
    // the height); the screen's plates are taken as the packets' are.
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    for (std::size_t i = 0; i < mobs.size() && i < listed.size(); ++i) {
        const Nameplate& n = mobs[i];
        // Within reach it is fought where it stands: the area bounds walking.
        // But for the owner's Q2 of 04d: the character outside the box with a
        // way back the map shows walks back first -- what is in reach outside
        // the box is let be, or it fights out there for good (2026-11-04,
        // 1402,802: 108 px below the box until DỪNG).
        if (in_reach(cfg, n.x() - me.x(), n.y() - me.y()) && !zone_.back_first) continue;
        // The current target too when its name did not read this look: with
        // only "Đánh trước" to be attacked, that makes it NotFirst, and it is
        // kept as the one it was (pass()) -- walked after past the edge for as
        // long as its name did not read. Marked, it is let go of as outside.
        const bool nameless_target =
            static_cast<int>(i) == target_at && listed[i] == Listed::NotFirst && n.name.empty();
        if (!nameless_target && listed[i] != Listed::Free && listed[i] != Listed::First &&
            listed[i] != Listed::GaveUp)
            continue;
        const double wx = fix_.x + (n.x() - me.x()) * scale;
        const double wy = fix_.y + (n.y() - me.y() - cfg.packet_dy) * scale;
        const double keep = static_cast<int>(i) == target_at ? kSpotKeepPx : 0;
        if (!spot.inside(wx, wy, keep)) listed[i] = Listed::OutOfArea;
    }
}

void Hunter::spot_busy(std::uint64_t now) {
    spot_busy_ms_ = now;
    end_return();
}

void Hunter::end_return() {
    return_on_ = false;
    return_quiet_ms_ = 0;
}

bool Hunter::go_home(const HunterConfig& cfg, std::uint64_t now) {
    if (spot_mode_ != SpotMode::Use) return false;
    Spot spot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        spot = spot_;
    }
    // The map tab's area with its map's file: the way back the map shows --
    // but for a report on the ground on none of its floors, which no walk
    // gives: the file does not fit there. This run's spot, or a map not gone
    // by, as it always went. A walk worked out past its floor's end is not
    // that: its report is on a floor still.
    const WorldFix& f = fix_;
    if (zone_.page) {
        const bool unfit = !zone_.report && !zone_.way && !f.airborne && !in_air(now);
        if (!unfit) return go_home_zone(cfg, spot, now);
    }
    const float sdx = spot.x - f.x, sdy = spot.y - f.y;
    const float reported = std::abs(spot.x - f.rx);
    const float tol = static_cast<float>(cfg.spot_tolerance);
    const bool held = coord_.critical_active();

    // The give-up clock of a walk back under way: the time walking -- or
    // standing a moment between walks, for the report of where a walk that
    // took the character there by the working-out really ended -- that the
    // reports put it no kSpotProgressPx nearer. Passes in a row only: a hold,
    // the game out of focus or the character unseen in between are not
    // counted, as they are not for a target.
    if (return_on_ && !held) {
        if (reported + kSpotProgressPx <= return_mark_px_) {
            return_mark_px_ = reported;
            return_quiet_ms_ = 0;
        } else if (return_pass_ + 1 == passes_ && now > return_seen_ms_) {
            return_quiet_ms_ += std::min<std::uint64_t>(now - return_seen_ms_, 250);
        }
        return_pass_ = passes_;
        return_seen_ms_ = now;
    }

    // Home: let go of the walk -- the working-out says so. Over only once a
    // report says so too: a walk into a wall works out to anywhere, and its
    // stop's report puts the character back where it is.
    if (std::abs(sdx) <= tol) {
        if (return_on_ && reported <= tol) {
            GB_DEBUG("Hunter: back at the hunting spot, %d px off",
                     static_cast<int>(std::lround(std::abs(sdx))));
            end_return();
        }
        // Back by itself -- pushed there, or walked there by a fight: tries
        // given up on are tried again.
        if (reported <= tol) {
            return_strikes_ = 0;
            return_retry_ms_ = 0;
        }
        said_too_high_ = false;
        spot_why_ = SpotWhy::None;
        publish_spot();
        return false;
    }

    if (return_strikes_ >= kSpotMaxTries) {
        spot_why_ = SpotWhy::GaveUp;
        publish_spot();
        return false;
    }
    if (return_retry_ms_ != 0 && now < return_retry_ms_) {
        spot_why_ = SpotWhy::Retrying;
        publish_spot();
        return false;
    }
    // More than one jump above: no walk gets there. Said once.
    if (sdy < -static_cast<float>(kOneJumpPx)) {
        if (!said_too_high_) {
            said_too_high_ = true;
            GB_INFO("Hunter: the hunting spot is %d px above -- higher than one jump; not walking back to it",
                    static_cast<int>(std::lround(-sdy)));
        }
        end_return();
        spot_why_ = SpotWhy::TooHigh;
        publish_spot();
        return false;
    }
    said_too_high_ = false;
    spot_why_ = SpotWhy::None;
    publish_spot();

    // Not under a hold, which wants the character standing; not with a
    // kill's pick-ups owed; not before the delay -- a monster on its way
    // would have it walk off and straight back.
    if (held || (cfg.loots() && loot_left_ > 0)) return false;
    if (now - spot_busy_ms_ < cfg.spot_return_delay_ms) return false;
    // In the air -- by the packets, or by the hunter's own last hop: a walk
    // back already held goes on as it is, with no key pressed or let go of;
    // none starts.
    if (f.airborne || in_air(now)) {
        if (returned_last_ && walk_vk_ != 0) {
            // Walking that way still, through the hop: a walk back that hops
            // all the way counts the steps back the other way afresh as one
            // along the ground does (note_walk), not once a jump.
            note_walk(walk_vk_ == cfg.left_key ? 0 : 1, true, now);
            returning_ = true;
            return true;
        }
        return false;
    }

    if (!return_on_) {
        return_on_ = true;
        return_mark_px_ = reported;
        return_quiet_ms_ = 0;
        return_pass_ = passes_;
        return_seen_ms_ = now;
        ++minute_returns_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++status_.returns;
        }
        GB_INFO("Hunter: walking back to the hunting spot: %d px %s, %d px %s",
                static_cast<int>(std::lround(std::abs(sdx))), sdx < 0 ? "left" : "right",
                static_cast<int>(std::lround(std::abs(sdy))), sdy < 0 ? "up" : "down");
    } else if (return_quiet_ms_ >= cfg.spot_give_up_ms) {
        // Nowhere, by the reports, in spot_give_up_ms: a wall, a ledge it
        // cannot climb, a gap. Tried again a while later; kSpotMaxTries in a
        // row, and not again until the spot is set again or the character is
        // back at it.
        end_return();
        ++return_strikes_;
        ++minute_return_give_ups_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++status_.return_give_ups;
        }
        if (return_strikes_ >= kSpotMaxTries) {
            spot_why_ = SpotWhy::GaveUp;
            GB_INFO("Hunter: the walk back to the hunting spot got nowhere in %u s, %d times in a row -- no more "
                    "tries; reported %d px off",
                    cfg.spot_give_up_ms / 1000, return_strikes_, static_cast<int>(std::lround(reported)));
        } else {
            return_retry_ms_ = now + cfg.spot_retry_ms;
            spot_why_ = SpotWhy::Retrying;
            GB_INFO("Hunter: the walk back to the hunting spot got nowhere in %u s -- reported %d px off; trying "
                    "again in %u s",
                    cfg.spot_give_up_ms / 1000, static_cast<int>(std::lround(reported)), cfg.spot_retry_ms / 1000);
        }
        publish_spot();
        return false;
    }

    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    walk(cfg, static_cast<int>(std::lround(sdx / scale)), static_cast<int>(std::lround(sdy / scale)), now,
         WalkFor::Spot);
    returning_ = true;
    return true;
}

// ------------------------------------------- the area against its map's file

std::shared_ptr<const GameMapBook::Page> Hunter::zone_page(const Spot& s) const {
    if (!s.set || !s.from_map || s.suspended || book_ == nullptr || s.stem.empty()) return nullptr;
    // Found wrong this session: a drop landed where it did not say, or two
    // standing points of yours fell off its floors (the hunter's own trust,
    // when drops have it looking).
    if (book_->distrusted(s.stem)) return nullptr;
    if (trust_.stem() == s.stem && !trust_.trusted() && trust_.report().why == MapTrust::Why::Mismatch) return nullptr;
    return book_->get(s.stem);
}

std::optional<MapPoint> Hunter::stand_on(const GameMapBook::Page& p, const WorldFix& f, bool worked_out) {
    if (f.state != WorldFix::State::Known || f.airborne) return std::nullopt;
    const int rx = round_px(f.rx), ry = round_px(f.ry);
    const std::optional<MapFloor> on = p.floor_near(rx, ry);
    if (!on || on->wall()) return std::nullopt;
    if (!worked_out) return MapPoint{ rx, on->y - 1 };
    // Its floor followed to where the walk has been worked out to: past its
    // end, the character is falling, or landed with no report yet -- none.
    const int x = round_px(f.x);
    const std::optional<int> top = p.follow_to(rx, ry, x);
    if (!top) return std::nullopt;
    return MapPoint{ x, *top - 1 };
}

bool Hunter::go_home_zone(const HunterConfig& cfg, const Spot& spot, std::uint64_t now) {
    const WorldFix& f = fix_;
    const ZoneHere& z = zone_;
    const GameMapBook::Page& page = *z.page;
    const int tol = cfg.spot_tolerance;
    const bool held = coord_.critical_active();
    const std::optional<ZoneReturn>& way = z.way;
    const MapPoint at = z.stand ? *z.stand : MapPoint{ round_px(f.x), round_px(f.y) };
    const MapPoint rep = z.report ? *z.report : MapPoint{ round_px(f.rx), round_px(f.ry) };

    // Where the walk goes: the home, or its floor's point in the box nearest
    // it across -- or, the way going off the floor's end, past that end; with
    // nothing the map shows, the home as every walk back went (as_before).
    // A way that starts with a jump up (2026.11.05b): its take-off.
    MapPoint goal{ z.box.home_x, z.box.home_y };
    if (way) goal = way->as_before || way->falls == 0 ? way->at : MapPoint{ way->walk_x, way->at.y };
    if (way && way->jump) goal = way->jump->take();
    // How far off it the last report put the character, across: what the
    // give-up clock goes by, measured afresh toward a new goal -- a fall
    // landed, a floor's point moved, a jump landed.
    const float reported = static_cast<float>(std::abs(goal.x - rep.x));
    if (return_on_ && (!return_goal_on_ || return_goal_.x != goal.x || return_goal_.y != goal.y)) {
        return_mark_px_ = reported;
        return_quiet_ms_ = 0;
    }
    return_goal_on_ = true;
    return_goal_ = goal;
    // Settled at a jump's take-off, waiting there for a moment it may go, or
    // in its flight: no time lost -- the give-up clock stands (2026.11.05b).
    const bool jumping = jump_.purpose == JumpUpFor::Home &&
                         (jump_.phase == JumpUpPhase::Settle || jump_.phase == JumpUpPhase::Air);
    const bool clock_stands = jumping || jump_wait_home_;
    jump_wait_home_ = false;
    if (return_on_ && !held && !clock_stands) {
        if (reported + kSpotProgressPx <= return_mark_px_) {
            return_mark_px_ = reported;
            return_quiet_ms_ = 0;
        } else if (return_pass_ + 1 == passes_ && now > return_seen_ms_) {
            return_quiet_ms_ += std::min<std::uint64_t>(now - return_seen_ms_, 250);
        }
        return_pass_ = passes_;
        return_seen_ms_ = now;
    }
    // In the flight of a jump of the way back: held still -- no key -- until
    // it is judged (jump_follow), the walk back going on (2026.11.05b).
    if (jumping && jump_.phase == JumpUpPhase::Air) {
        returning_ = true;
        return true;
    }
    // With the tick off, or jumps off for the run: where the way back would
    // have jumped up, said (jump_would) -- worked out once a standing point.
    if (!z.home && z.stand && jump_ready(cfg, z.page.get()) && !(cfg.jump_up && jump_latch_ == JumpUpOff::None)) {
        if (!jump_dry_asked_ || jump_dry_from_.x != z.stand->x || jump_dry_from_.y != z.stand->y) {
            const JumpRules up = jump_rules(cfg, z.page);
            jump_dry_way_ = return_point(page, z.box, *z.stand, &up);
            jump_dry_asked_ = true;
            jump_dry_from_ = *z.stand;
        }
        if (jump_dry_way_ && jump_dry_way_->jump) jump_would(*jump_dry_way_->jump, JumpUpFor::Home, -1, 0, {}, now);
    }

    // Home -- across, and on its floor: never by x alone (882,616 under the
    // ledge's home of 2026-11-04 was "3 px off"). Or at the way back's point,
    // the home out of reach of it: stood at, never walked past. As before,
    // the worked-out place lets go of the walk, and a report there ends it.
    const bool own = way && !way->home && way->own_floor();
    const bool at_way = own && z.stand && zone_stands_at(&page, way->at, *z.stand, tol);
    const bool rep_at_way = own && zone_stands_at(&page, way->at, rep, tol);
    const bool before = way && way->as_before && std::abs(goal.x - at.x) <= tol;
    if (z.home || at_way || before) {
        const bool reported_there = z.home   ? zone_at_home(&page, z.box, rep, tol)
                                    : at_way ? rep_at_way
                                             : std::abs(goal.x - rep.x) <= tol;
        if (return_on_ && reported_there) {
            GB_DEBUG("Hunter: back in the hunting area at %d,%d -- %s", at.x, at.y,
                     z.home ? "the home" : "its floor's point nearest the home, which it cannot reach from there");
            end_return();
        }
        if (reported_there) {
            return_strikes_ = 0;
            return_retry_ms_ = 0;
        }
        said_no_way_ = false;
        said_too_high_ = false;
        spot_why_ = SpotWhy::None;
        publish_spot();
        return false;
    }
    // A walk back already held, gone on with as it is: in the air -- or on
    // the ground past its floor's end with no landing's report, only as far
    // as the goal. walk() is not called on it, so nothing renews it, finds it
    // stuck or gives it up: a floor the game has and the file lacks is not
    // walked blind until the packets lose the character. Past the goal the
    // caller lets go, and the stop's report places it.
    const auto hold_on = [&] {
        if (!returned_last_ || walk_vk_ == 0) return false;
        const bool short_of_goal = walk_vk_ == cfg.left_key ? f.x > static_cast<float>(goal.x)
                                                            : f.x < static_cast<float>(goal.x);
        if (!f.airborne && !in_air(now) && !short_of_goal) return false;
        note_walk(walk_vk_ == cfg.left_key ? 0 : 1, true, now);
        returning_ = true;
        return true;
    };
    // Nowhere on the map's floors to work a way back out from -- in the air,
    // past its floor's end -- and none worked out before: as it is.
    if (!z.stand && !way) return !held && hold_on();

    if (return_strikes_ >= kSpotMaxTries) {
        spot_why_ = SpotWhy::GaveUp;
        publish_spot();
        return false;
    }
    if (return_retry_ms_ != 0 && now < return_retry_ms_) {
        spot_why_ = SpotWhy::Retrying;
        publish_spot();
        return false;
    }
    // No way back the map shows, and the home more than one jump above: in
    // place of TooHigh, said once -- it fights where it stands (the owner's
    // Q2 of 04d).
    if (!way) {
        if (!said_no_way_) {
            said_no_way_ = true;
            const int dy = z.box.home_y - at.y;
            GB_INFO("Hunter: no way back into the hunting area from %d,%d (%d px outside it) -- no walk along its "
                    "floor or off an end gets there, and the home is %d px %s; fighting where it stands",
                    at.x, at.y, z.outside_px, std::abs(dy), dy < 0 ? "up" : "down");
        }
        end_return();
        said_too_high_ = false;
        spot_why_ = SpotWhy::NoReturn;
        publish_spot();
        return false;
    }
    said_no_way_ = false;
    said_too_high_ = false;
    spot_why_ = SpotWhy::None;
    publish_spot();

    // A way with jumps up worked out before this session learned something
    // new of the map -- a landing judged this pass: asked again at the next
    // (track_zone_way), not walked meanwhile (2026.11.05b).
    if (way_jumps_ && way_epoch_ != jump_epoch_) return false;
    // As go_home: not under a hold, nor with a kill's pick-ups owed, nor
    // before the delay; in the air, or past the floor's end before the
    // landing's report, a walk back already held goes on as it is (hold_on).
    if (held || (cfg.loots() && loot_left_ > 0)) return false;
    if (now - spot_busy_ms_ < cfg.spot_return_delay_ms) return false;
    if (f.airborne || in_air(now) || !z.stand) return hold_on();

    if (!return_on_) {
        return_on_ = true;
        return_mark_px_ = reported;
        return_quiet_ms_ = 0;
        return_pass_ = passes_;
        return_seen_ms_ = now;
        ++minute_returns_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++status_.returns;
        }
        const int gdx = way->at.x - at.x, gdy = way->at.y - at.y;
        char from[64];
        if (z.outside_px > 0)
            std::snprintf(from, sizeof from, "into the hunting area from %d,%d (%d px outside it)", at.x, at.y,
                          z.outside_px);
        else
            std::snprintf(from, sizeof from, "in the hunting area from %d,%d", at.x, at.y);
        // With jumps up on the way (2026.11.05b): the first, and how many.
        char how[128] = "";
        if (way->jump)
            std::snprintf(how, sizeof how,
                          ", along the floor it stands on to x %d, then a jump up (%s) onto floor %d%s", way->jump->x,
                          jump_kind_name(way->jump->kind), way->jump->land_top,
                          way->jumps > 1 ? " and another later" : "");
        else if (way->jumps > 0)
            std::snprintf(how, sizeof how, ", off its floor's end first, then %d jump%s up", way->jumps,
                          way->jumps == 1 ? "" : "s");
        GB_INFO("Hunter: walking back %s: to %d,%d, %s -- %d px %s, %d px %s%s", from, way->at.x, way->at.y,
                way->home        ? "the home"
                : way->as_before ? "the home, as every walk back went (no way the map shows)"
                                 : "its floor's point nearest the home",
                std::abs(gdx), gdx < 0 ? "left" : "right", std::abs(gdy), gdy < 0 ? "up" : "down",
                how[0] != '\0'    ? how
                : way->as_before ? ""
                : way->falls == 0 ? ", along the floor it stands on, no hop"
                : (gdx < 0) == (goal.x < at.x) ? ", off its floor's end first, no hop"
                                               : ", off its floor's end the other way first, no hop");
    } else if (return_quiet_ms_ >= cfg.spot_give_up_ms) {
        end_return();
        ++return_strikes_;
        ++minute_return_give_ups_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++status_.return_give_ups;
        }
        if (return_strikes_ >= kSpotMaxTries) {
            spot_why_ = SpotWhy::GaveUp;
            GB_INFO("Hunter: the walk back into the hunting area got nowhere in %u s, %d times in a row -- no more "
                    "tries; reported %d px off %d,%d",
                    cfg.spot_give_up_ms / 1000, return_strikes_, static_cast<int>(std::lround(reported)), goal.x,
                    goal.y);
        } else {
            return_retry_ms_ = now + cfg.spot_retry_ms;
            spot_why_ = SpotWhy::Retrying;
            GB_INFO("Hunter: the walk back into the hunting area got nowhere in %u s -- reported %d px off %d,%d; "
                    "trying again in %u s",
                    cfg.spot_give_up_ms / 1000, static_cast<int>(std::lround(reported)), goal.x, goal.y,
                    cfg.spot_retry_ms / 1000);
        }
        publish_spot();
        return false;
    }

    // A way that starts with a jump up, at its take-off -- within
    // kJumpUpLeadPx of it, or settling there: the walk let go of, the stop
    // waited for and the key pressed (jump_at). Refused there for now, it
    // stands, the give-up clock running (2026.11.05b).
    const bool at_take_off = way->jump && std::abs(at.x - way->jump->x) <= kJumpUpLeadPx;
    if (way->jump && ((jumping && jump_.phase == JumpUpPhase::Settle) || at_take_off)) {
        if (!jump_at(cfg, JumpUpFor::Home, *way->jump, -1, 0, {}, now)) return false;
        returning_ = true;
        return true;
    }

    // Along the floor, or off its end: no hop (WalkFor::Floor). As before,
    // with nothing the map shows: the home's height hopped toward.
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    const int dx = static_cast<int>(std::lround((goal.x - f.x) / scale));
    if (way->as_before)
        walk(cfg, dx, static_cast<int>(std::lround((spot.y - f.y) / scale)), now, WalkFor::Spot);
    else
        walk(cfg, dx, 0, now, WalkFor::Floor);
    returning_ = true;
    return true;
}

bool Hunter::walk_kept_in(const HunterConfig& cfg, const Nameplate& target, int dx, int dy, std::uint64_t now,
                          int goal_px) {
    const ZoneHere& z = zone_;
    // This run's spot, no map to go by, or nowhere on it to look from: walked
    // as before.
    if (spot_mode_ != SpotMode::Use || !z.page || !z.stand) return false;
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    // As far as the walk goes before it stops for this target -- to reach
    // across when it is on the level, under it otherwise; the seek's, to its
    // attack point -- and a look's walk more: past the target's floor, the
    // box's edge a walk would never come to is no reason to refuse it.
    const int adx = static_cast<int>(std::lround(std::abs(dx) * scale));
    const int reach = static_cast<int>(std::lround(cfg.reach * scale));
    const int to_go = goal_px >= 0 ? goal_px : level_with(cfg, dy) ? std::max(0, adx - reach) : adx;
    const int ahead = std::clamp(to_go + kZoneWalkSlackPx, 1, kZoneWalkAheadPx);
    const ZoneWalk w = walk_stays_in(*z.page, z.box, *z.stand, dx < 0 ? -1 : 1, ahead);
    if (w.ok) return false;

    // Refused: let go of, the target passed over a while -- picked straight
    // back it would be walked to again -- and its fight over; by the seek as
    // well, by its slot (2026.11.05b).
    let_go_of_moves(cfg);
    if (gave_up_.size() < 16)
        gave_up_.push_back(GaveUpSpot{ target.x(), target.y(), now + 2ull * cfg.give_up_ms, true });
    if (target.slot >= 0) seek_skip(target.slot, target.gen, now + 2ull * cfg.give_up_ms, SeekNo::Passed);
    ++minute_walks_kept_;
    char land[48] = "";
    if (w.why == ZoneWalk::Why::WalkOff && w.land)
        std::snprintf(land, sizeof land, ", landing at %d,%d", w.land->x, w.land->y);
    else if (w.why == ZoneWalk::Why::WalkOff)
        std::snprintf(land, sizeof land, ", landing on nothing");
    // Said at INFO once a kWalkKeptSayMs, with where; every one counted for
    // the minute line.
    const LogLevel level =
        walk_kept_said_ms_ == 0 || now >= walk_kept_said_ms_ + kWalkKeptSayMs ? LogLevel::Info : LogLevel::Debug;
    if (level == LogLevel::Info) walk_kept_said_ms_ = now;
    Logger::instance().logf(level,
                            "Hunter: not walking %s toward %s (%d px across, %d px %s) -- from %d,%d it %s at %d,%d%s "
                            "(%d px outside the hunting area); passed over for %u s",
                            dx < 0 ? "left" : "right", target.name.empty() ? "a monster" : target.name.c_str(),
                            std::abs(dx), std::abs(dy), dy < 0 ? "up" : "down", z.stand->x, z.stand->y,
                            zone_walk_name(w.why), w.at.x, w.at.y, land, w.out_px, 2 * cfg.give_up_ms / 1000);
    end_fight("the walk would leave the hunting area");
    watch_ = DamageWatch{};
    have_target_ = false;
    return true;
}

std::vector<char> Hunter::own_floor_marks(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                                          const Nameplate& me, const GameMapBook::Page& p, MapPoint stand,
                                          const WorldFix& f) const {
    std::vector<char> out(mobs.size(), 0);
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    // Each one above's place in the world, as mark_out_of_area lays it; the
    // character's floor followed once each way as far as the furthest.
    std::vector<MapPoint> where(mobs.size());
    int far_left = 0, far_right = 0;
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.y() - me.y() >= -cfg.up()) continue;
        where[i] = { static_cast<int>(std::lround(f.x + (n.x() - me.x()) * scale)),
                     static_cast<int>(std::lround(f.y + (n.y() - me.y() - cfg.packet_dy) * scale)) };
        const int d = where[i].x - stand.x;
        if (d < 0) far_left = std::max(far_left, -d);
        else       far_right = std::max(far_right, d);
    }
    const MapTrail left = p.follow(stand.x, stand.y, -1, far_left + 1);
    const MapTrail right = p.follow(stand.x, stand.y, 1, far_right + 1);
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.y() - me.y() >= -cfg.up()) continue;
        const MapPoint w = where[i];
        // The character's floor at its column -- the column before a hole it
        // crosses -- and the monster's floor there: the same, it walks to it.
        const MapTrail& t = w.x < stand.x ? left : right;
        const MapPoint* top = nullptr;
        bool reached = false;
        for (const MapPoint& q : t.pts) {
            const bool past = w.x < stand.x ? q.x < w.x : q.x > w.x;
            if (q.x == w.x || past) reached = true;
            if (!past) top = &q;
        }
        if (!reached || top == nullptr) continue;
        const std::optional<int> theirs = monster_floor(p, w.x, w.y);
        if (theirs && std::abs(*theirs - top->y) <= GameMapBook::kFollowLinkPx) out[i] = 1;
    }
    return out;
}

void Hunter::publish_spot() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.spot_set = spot_.set;
    status_.spot_x = spot_.x;
    status_.spot_y = spot_.y;
    status_.spot_half_w = spot_.half_w;
    status_.spot_half_h = spot_.half_h;
    status_.spot_suspended = spot_.suspended;
    status_.spot_login = spot_.suspended && spot_.login;
    status_.spot_map_aside = spot_.suspended && spot_.map_aside;
    status_.spot_why = spot_why_;
    status_.spot_left = spot_.left;
    status_.spot_top = spot_.top;
    status_.spot_right = spot_.right;
    status_.spot_bottom = spot_.bottom;
    status_.spot_from_map = spot_.set && spot_.from_map;
    status_.spot_stem = spot_.stem;
    status_.spot_epoch = spot_.epoch;
    status_.zone_wait = zone_wait_;
    status_.zone_wait_stem = zone_wait_ != ZoneWait::None && zone_pending_ ? zone_pending_->stem : std::string();
    status_.zone_wait_px = zone_wait_px_;
}

// ---------------------------------------------------------------- the loop

void Hunter::loop() {
    name_this_thread("hunter");
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
    // The time since the last pass, to what that pass was spent on -- the
    // seek's minute line (2026.11.05b); and nothing of the seek's is on until
    // this pass says so.
    seek_time(clock_now());
    seek_on_ = false;

    // The plates in another frame of reference than the last pass's -- the
    // packets taken over from the screen, or the screen back from them: what
    // was held by where it stood is let go of before anything is looked at.
    // The target was followed by its spot, and its spot in the other frame is
    // somewhere else, so it would read as gone -- a kill, and three loot
    // presses -- at every switch.
    if (eyes_.view) {
        const std::uint32_t view = eyes_.view();
        if (view != view_) {
            view_ = view;
            forget_positions();
        }
    }

    // The hunting spot: where the packets put the character, the map entries
    // since the last pass, and what the spot makes of this one -- every pass,
    // a hold on or not. Switched off, nothing of it is asked, and nothing
    // below changes.
    returned_last_ = returning_;
    returning_ = false;
    if (cfg.spot) {
        track_spot(cfg);
    } else if (spot_mode_ != SpotMode::Off) {
        // Switched off mid-run: the walk back stops this pass -- nothing below
        // goes home now -- and the spot is dropped with what its walks back
        // met. Switched on again, it is taken afresh where the character then
        // stands: the player who unticked it to walk somewhere else wants the
        // new place, not a walk back to the old one.
        spot_mode_ = SpotMode::Off;
        spot_why_ = SpotWhy::None;
        end_return();
        return_strikes_ = 0;
        return_retry_ms_ = 0;
        said_too_high_ = false;
        login_gap_ = false;
        // An area of the map tab's too, waiting or asked: ticked again, the
        // tab sends it again -- and what the map made of the one in use.
        zone_pending_.reset();
        zone_wait_ = ZoneWait::None;
        zone_wait_px_ = 0;
        zone_ = ZoneHere{};
        bool had = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            had = spot_.set;
            spot_ = Spot{};
            retake_asked_ = suspend_asked_ = false;
            zone_asked_.reset();
            zone_clear_asked_.clear();
            zone_aside_asked_.clear();
            status_.spot_dx = status_.spot_dy = 0.0f;
        }
        if (had)
            GB_INFO("Hunter: Chỗ đánh quái switched off -- the hunting spot is dropped; switched on again, it is "
                    "taken where the character stands");
        publish_spot();
    }
    // No spot to go by: standing, as stay_put does -- attacking only what is
    // within reach, never walking, never stepping back.
    const bool still = cfg.stay_put || spot_mode_ == SpotMode::Stand;

    const auto set_state = [this](State s, bool only_skipped, bool passed_over = false, bool outside = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = s;
        status_.only_skipped = only_skipped;
        status_.passed_over = passed_over;
        status_.outside_area = outside;
        // Walking back to the hunting spot is for want of a monster too.
        if ((s == State::NoTargets || s == State::Returning) && status_.no_target_since_ms == 0)
            status_.no_target_since_ms = clock_now();
    };

    // "Nhảy xuống bậc dưới": the keys a drop owes, where the packets put the
    // character for it, the map it goes by, and a map change after a press
    // -- every pass, a hold on or not, the game in front or not. With
    // drop_down off and no press of the last kPressWatchMs, nothing. Halted
    // -- the map changed after a press, or a key-up stays owed -- the
    // hunter moves nothing until GoBot's window has stopped it.
    drop_watch(cfg, clock_now());
    if (drop_halt_) {
        let_go_of_moves(cfg);
        disengage();
        set_state(State::Idle, false);
        return;
    }

    // A hold -- a revive, a trash pass, a chat line, a popup being closed --
    // refuses every key while it is on, so there is no fight meanwhile; and
    // whatever it did may have turned the character, so which way it faces
    // is not known once it is over.
    const bool held = coord_.critical_active();
    if (held_before_ && !held) facing_ = Facing::Unknown;
    held_before_ = held;
    // And a walk held from the last look is let go of: whatever holds the
    // keys wants the character standing, and the game free of a held arrow.
    // Time it cannot walk is no time spent failing to reach the target: not
    // counted toward passing it over (reach_ms_).
    if (held) {
        disengage();
        let_go_of_moves(cfg);
        reach_ms_ = clock_now();
        drop_hold("a hold");
        jump_hold("a hold");
    }

    // A held walk with the game no longer in front is let go of whatever
    // require_focus says: the arrow would be down in whatever is.
    if (walk_vk_ != 0 && !focused()) let_go_of_moves(cfg);

    if (cfg.require_focus && !focused()) {
        release_movement();
        facing_ = Facing::Unknown;
        disengage();
        reach_ms_ = clock_now();
        drop_hold("the game not in front");
        jump_hold("the game not in front");
        set_state(State::Unfocused, false);
        return;
    }
    minute_log(cfg, clock_now());
    // A drop's fall judged, by this pass's fix; a settle not renewed at the
    // last pass let go of. Nothing is pressed here. A jump up's flight the
    // same way (2026.11.05b).
    drop_follow(cfg, clock_now());
    jump_follow(cfg, clock_now());

    // What the last kill dropped. A few presses, spaced so the game sees
    // each one, before anything else is chased -- walking off to the next
    // monster first leaves the drop behind.
    if (cfg.loots() && loot_left_ > 0) {
        const std::uint64_t t = clock_now();
        if (!cfg.hold_walk) {
            if (t - last_loot_ms_ >= 220) {
                if (press_loot(cfg)) --loot_left_;
                return;
            }
        } else {
            // Standing until they are done: a walk taken up between the
            // presses carried the character off the drop and back, a stop
            // and a start each time, after every kill. For kLootStandMs at
            // most, so a pick-up key the input will not take cannot hold the
            // hunter still.
            if (loot_since_ms_ == 0) loot_since_ms_ = t;
            if (t - loot_since_ms_ <= kLootStandMs) {
                let_go_of_moves(cfg);
                if (t - last_loot_ms_ >= 220 && press_loot(cfg)) --loot_left_;
                if (loot_left_ == 0) loot_since_ms_ = 0;
                return;
            }
            loot_left_ = 0;
            loot_since_ms_ = 0;
        }
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
    // "Tự tìm quái trong vùng" (2026.11.05b): the whole map's monsters, and
    // whether the seek is on this pass -- the map tab's area in use, the
    // packets steering, no walk back first. Off, nothing below is its.
    seek_look(cfg, clock_now());
    if (mobs.empty()) {
        // The last one in view has just gone, and the likeliest reason is
        // that it died. Or its plate dropped out for a look, which they do
        // all the time: the fight is over once it has not come back where it
        // was for a moment, not at once.
        if (have_target_) loot_left_ = 3;
        have_target_ = false;
        // With the seek on: one out of view to go to, or nothing -- stood for
        // inside the box; outside it, the walk back as ever.
        if (seek_on_) {
            const std::uint64_t t = clock_now();
            const SeekIdle idle = seek_idle(cfg, seek_choose(cfg, mobs, me, {}, t, -1, nullptr), t);
            if (idle != SeekIdle::NotMine) {
                if (idle == SeekIdle::Going) spot_busy(t);
                disengage();
                set_state(idle == SeekIdle::Going ? State::Approaching : State::NoTargets, false);
                fight_unseen(t, "gone");
                return;
            }
        }
        // Nothing to fight: back to the hunting spot, if there is one and
        // the character is away from it -- or the keys let go of, as ever.
        const bool home = go_home(cfg, clock_now());
        if (!home) let_go_of_moves(cfg);
        disengage();
        set_state(home ? State::Returning : State::NoTargets, false);
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
    // Those given up on a moment ago: for taking no damage (with a style), or
    // for staying out of reach.
    if (!gave_up_.empty()) mark_gave_up(mobs, now, listed);
    // Outside the hunting spot's area and out of reach: not walked to. The
    // current target -- found as below (find_target) -- only once
    // kSpotKeepPx past the edge.
    if (spot_mode_ == SpotMode::Use) {
        const int target_at = have_target_ ? find_target(mobs, slack) : -1;
        mark_out_of_area(cfg, mobs, me, target_at, listed);
    }
    // With the seek on: one not in reach that no standing point inside the
    // area gets in reach of is never walked toward (2026.11.05b).
    if (seek_on_) mark_no_way(cfg, mobs, me, listed, now);

    // Stay on the current target while it is still there. Re-picking the
    // nearest every frame makes the bot oscillate between two monsters a
    // similar distance away and never reach either. A plate from the packets
    // is found by its slot and generation (find_target): another monster
    // walking past where it was is not taken for it.
    const Nameplate* target = nullptr;
    if (have_target_) {
        bool respawned = false;
        int nearest = -1;
        const int found = find_target(mobs, slack, &respawned, &nearest);
        std::size_t at = 0;
        if (found >= 0) {
            at = static_cast<std::size_t>(found);
            target = &mobs[at];
            if (target_slot_ >= 0 && target->slot >= 0) {
                ++minute_slot_kept_;
                if (nearest != found) ++minute_swaps_refused_;
            }
        } else {
            // Too far from where it was -- or its slot empty, or holding the
            // next monster: it died and this is a different one.
            if (respawned) ++minute_new_gen_;
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
        // name merely did not read this time is still the one it was. So
        // too one gone kSpotKeepPx outside the hunting spot's area, out of
        // reach: not walked after, and no kill.
        if (target && refused(listed[at]) &&
            !(listed[at] == Listed::NotFirst && target->name.empty())) {
            const bool outside = listed[at] == Listed::OutOfArea;
            const bool no_way = listed[at] == Listed::NoWay;
            if (outside)
                GB_INFO("Hunter: letting go of %s -- it is outside the hunting area",
                        target->name.empty() ? "a monster" : target->name.c_str());
            // The seek's (2026.11.05b): a kept target is checked again at
            // every look, not only at its pick.
            if (no_way)
                GB_INFO("Hunter: letting go of slot %d -- no place inside the area to hit it from any more",
                        target->slot);
            target = nullptr;
            end_fight(outside ? "outside the area" : no_way ? "no way to it" : "the lists");
            watch_ = DamageWatch{};
            if (no_way) seek_let_go(nullptr);
        }

        // With a style, as GoC: the give-up clock runs only while the target
        // is out of reach, and one within reach is fought until it dies.
        // Without one it runs from the pick, in reach or not, and every fight
        // is picked afresh this often -- as it always was. When it was last in
        // reach is kept either way.
        const bool reach_now = target && in_reach(cfg, target->x() - me.x(), target->y() - me.y());
        if (reach_now) reach_ms_ = now;
        if (reach_now && cfg.goc_rules()) target_since_ms_ = now;
        // The seek getting nearer its attack point (2026.11.05b): no give-up
        // meanwhile -- a walk of 3-5 s would pass the monster over at 4 s. One
        // that gets nowhere is passed over by the seek itself (Stuck).
        if (target && !reach_now && seek_on_ && seek_holds_clock(*target, now)) target_since_ms_ = now;
        if (target && now - target_since_ms_ > cfg.give_up_ms) {
            // In reach at a look of give_up_ms, it is picked straight back:
            // counted apart too (targets_repicked).
            const bool repicked = now - reach_ms_ <= cfg.give_up_ms;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.targets_dropped;
                if (repicked) ++status_.targets_repicked;
            }
            // Out of reach for the whole of give_up_ms: passed over for twice
            // as long, on "Đánh trước" or not -- picked again at once, one
            // behind a wall or up on a ledge held the bot under it for good.
            // In reach at any look in that time -- every fight is given up on
            // every give_up_ms without a style, one that has just stepped out
            // of reach among them -- it is picked straight back, as it always
            // was. Said, with where it was, for what a jump could reach.
            const int gdx = target->x() - me.x(), gdy = target->y() - me.y();
            if (!repicked) {
                GB_INFO("Hunter: gave up on %s out of reach after %u s: %d px %s, %d px %s -- passed "
                        "over for %u s",
                        target->name.empty() ? "a monster" : target->name.c_str(), cfg.give_up_ms / 1000,
                        std::abs(gdx), gdx < 0 ? "left" : "right", std::abs(gdy), gdy < 0 ? "up" : "down",
                        2 * cfg.give_up_ms / 1000);
                if (gave_up_.size() < 16)
                    gave_up_.push_back(
                        GaveUpSpot{ target->x(), target->y(), now + 2ull * cfg.give_up_ms, true });
                if (target->slot >= 0)
                    seek_skip(target->slot, target->gen, now + 2ull * cfg.give_up_ms, SeekNo::Passed);
                listed[at] = Listed::GaveUp;
            }
            target = nullptr;
            // Without a style the same monster is usually picked straight
            // back, and that is the same fight still: its log goes on. With
            // one, out of reach this long is a fight
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
                    reach_ms_ = now;
                    last_switch_ms_ = now;
                    switched_ = true;
                    break;
                }
            }
        }
    }
    // A fresh pick, counted for the minute line once it is taken up (note_pick).
    bool fresh_pick = false;
    // Nothing to pick in view: back to the spot, as with no monster at all.
    const auto nothing_to_pick = [&] {
        // Ruled out by the lists, that is: one given up on a moment ago is
        // not the lists' doing -- it is passed over, and said apart; nor is
        // one outside the hunting spot's area, nor one the seek has no way to.
        bool ruled_out = false, passed_over = false, outside = false;
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            if (mobs[i].hp <= cfg.dead_below) continue;
            if (listed[i] == Listed::GaveUp)         passed_over = true;
            else if (listed[i] == Listed::OutOfArea) outside = true;
            else if (refused(listed[i]) && listed[i] != Listed::NoWay) ruled_out = true;
        }
        note_wait(cfg, mobs, me, listed, now);
        const bool home = go_home(cfg, now);
        if (!home) let_go_of_moves(cfg);
        disengage();
        set_state(home ? State::Returning : State::NoTargets, ruled_out, passed_over, outside);
        have_target_ = false;
        // A target whose plate dropped out for this look, with only monsters
        // that may not be attacked left in view: as with no plate at all, its
        // fight goes on until it has been gone a moment.
        fight_unseen(now, "nothing to attack");
    };
    if (seek_on_) {
        // "Tự tìm quái trong vùng" (2026.11.05b): one in reach first; then the
        // monsters inside the area the character gets to -- in view or not --
        // by tier and cost, with the drop's target below between its own floor
        // and the falls; the one gone for kept unless another is clearly
        // better. No Q3 marks: no way to one is NoWay (mark_no_way).
        const int kept = target ? static_cast<int>(target - mobs.data()) : -1;
        bool wounded = false;
        const SeekChoice c = seek_choose(cfg, mobs, me, listed, now, kept, &wounded);
        if (c.index >= 0 && c.index != kept) {
            if (target) {
                end_fight("another target (sought)");
                watch_ = DamageWatch{};
            }
            target = &mobs[static_cast<std::size_t>(c.index)];
            target_first_ = false;
            target_since_ms_ = now;
            reach_ms_ = now;
            if (wounded) ++minute_wounded_;
            fresh_pick = true;
        } else if (c.index < 0) {
            if (target) {
                end_fight("another target (sought, out of view)");
                watch_ = DamageWatch{};
            }
            target = nullptr;
            have_target_ = false;
            // To the one out of view; or nothing -- stood for inside the box;
            // outside it, the walk back as ever.
            const SeekIdle idle = seek_idle(cfg, c, now);
            if (idle == SeekIdle::NotMine) {
                nothing_to_pick();
                return;
            }
            if (idle == SeekIdle::Going) spot_busy(now);
            disengage();
            set_state(idle == SeekIdle::Going ? State::Approaching : State::NoTargets, false);
            fight_unseen(now, "nothing to attack");
            return;
        }
    } else if (!target) {
        Nameplate me_now;
        const bool have_me = see_player(me_now);
        bool wounded = false;
        // With nothing on the level, one below that a drop could go to now
        // before any other (choose_target): never with drops off, off for the
        // run, the map not trusted, or the spot on but not in use.
        const bool below_first = drops_may_go(cfg);
        // The owner's Q3 of 04d: no hop to follow, and a map to say which of
        // those above stand on the character's own floor -- the area's file,
        // or with no spot the one drops trust -- and the last resort passes
        // over the rest above. Neither: any height, as ever. This run's spot,
        // no saved area, goes on as before 04d whatever drops trust.
        std::vector<char> own;
        const std::vector<char>* own_floor = nullptr;
        if (kSkipAboveUnreached && have_me && !hops_to_targets(cfg)) {
            std::shared_ptr<const GameMapBook::Page> page = zone_.page;
            std::optional<MapPoint> stand = zone_.stand;
            WorldFix f = fix_;
            if (!page && !cfg.spot) {
                page = trust_.page();
                f = drop_fix_on_ ? drop_fix_ : WorldFix{};
                stand = page ? stand_on(*page, f, true) : std::nullopt;
            }
            if (page && stand) {
                own = own_floor_marks(cfg, mobs, me_now, *page, *stand, f);
                // 2026.11.05b: the nearest above a jump up gets onto, in the
                // map tab's area -- 2, picked last; with the tick off, said.
                if (page == zone_.page) jump_marks(cfg, mobs, me_now, listed, *page, *stand, f, own);
                own_floor = &own;
            }
        }
        const int pick =
            choose_target(cfg, mobs, have_me ? &me_now : nullptr, listed, &wounded, below_first, own_floor);

        // Everything in view reads as finished, or the lists rule all of it
        // out. Standing still beats swinging at a corpse, which holds the
        // hunter in place while something living walks past.
        if (pick < 0) {
            nothing_to_pick();
            return;
        }
        target = &mobs[static_cast<std::size_t>(pick)];
        target_first_ = listed[static_cast<std::size_t>(pick)] == Listed::First;
        target_since_ms_ = now;
        reach_ms_ = now;
        if (wounded) ++minute_wounded_;
        fresh_pick = true;
    }

    // Where it is, and which it is: the next look finds it by these.
    aim_at(*target);
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
            retreat_[back_.way].blocked = true;
            GB_INFO("Hunter: no more steps back to the %s -- the last one met %s; none that way until "
                    "the character has walked the other way",
                    back_.way == 0 ? "left" : "right", wrong);
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

    // Taken up: one in reach, or one walked to. Standing still, one out of
    // reach is not -- the next pass picks afresh, at every pass it waits.
    if (fresh_pick && (!still || in_reach(cfg, dx, dy))) note_pick(cfg, mobs, me, listed);

    // In reach -- but for the seek's walk away from it to its attack point,
    // at the edge of its band: the turn to it would walk the character back
    // out of it, so the walk goes on to its aim first (2026.11.05b).
    if (in_reach(cfg, dx, dy) && !(seek_on_ && seek_walks_away(*target))) {
        let_go_of_moves(cfg);
        spot_busy(now);
        // The seek's walk ends here (2026.11.05b): its arrival, its way and
        // its progress mark count within one walk to the point only. Kept over
        // a fight, a monster that stepped a few px out of reach after it read
        // as not found at once (kSeekArriveMs long past), or as stuck.
        if (seek_on_) {
            seek_.arrived_ms = 0;
            seek_.dir = 0;
            seek_.quiet_pass = 0;
        }
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
            if (target->slot >= 0) seek_skip(target->slot, target->gen, now + cfg.no_damage_ms, SeekNo::Passed);
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
        // game. The next pass turns to the target and attacks -- a tap of
        // kTurnTapMs, which walks the character back in about 21 px, so the
        // step is held as much longer than GoC's sum: held only that, the
        // turn left it a pixel inside keep_away, and it stepped back and
        // turned again until keep_distance_steps ran out. Never standing
        // still (GoC's lazy mode does not either) and never from a target off
        // the character's own level: GoBot cannot see where a platform ends,
        // and stepping back off one is the worst this could do. So too never
        // more than keep_distance_steps steps one way, from whichever
        // monsters, until the character has walked back the other way (an
        // attack from far enough wins no ground back); never before the last
        // step has been seen on the screen, nor that way again after a step
        // that met a wall or went down a ledge.
        const int away = keep_away_px(cfg);
        const int back_way = dx < 0 ? 1 : 0;        // away from the target
        Retreat& retreat = retreat_[back_way];
        const int most = std::clamp(cfg.keep_distance_steps, 1, 10);
        // With the hunting spot, never past the edge of its area: the step is
        // cut to the room left that way, and with too little for one -- a
        // step is held 50 ms at the least, and the turn after it walks
        // kTurnTapMs of that back (kSpotStepRoomPx) -- none is taken. With
        // the map tab's area and its file, nor one its floor takes out of the
        // box, down a slope or off an end (walk_stays_in, the owner's Q1 of
        // 04d): the x room alone let a step right from 987,683 on t52_s2 end
        // 12 px under the box. Refused, it fights where it stands.
        const bool in_area = spot_mode_ == SpotMode::Use;
        int room_ms = 0;
        if (in_area) {
            const Spot area = spot();
            const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
            const double room = back_way == 1 ? static_cast<double>(area.right) - fix_.x
                                              : fix_.x - static_cast<double>(area.left);
            const int ms = room < kSpotStepRoomPx ? 0 : static_cast<int>(std::lround(room / scale * 1000.0 / kWalkPxPerS));
            room_ms = ms < 50 + kTurnTapMs ? 0 : ms;
        }
        if (cfg.keep_distance && !still && !just_stepped_ && adx < away && !held && !drop_in_air(now) &&
            !jump_in_air(now) &&
            !retreat.blocked && !back_.judge && retreat.in_row < most &&
            std::abs(dy) <= std::max(cfg.up_nj(), cfg.down_nj()) && (!in_area || room_ms > 0) &&
            (!zone_.page || !zone_.stand ||
             walk_stays_in(*zone_.page, zone_.box, *zone_.stand, back_way == 1 ? 1 : -1).ok)) {
            // Off before the key goes out: no skill is thrown as the step
            // starts.
            disengage();
            int hold = backstep_ms(away, adx) + kTurnTapMs;
            if (in_area) hold = std::min(hold, room_ms);
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
                facing_key_ms_ = clock_now();
                facing_key_way_ = facing_;
                if (++retreat.in_row == most)
                    GB_INFO("Hunter: %d steps back to the %s -- no more that way until the character "
                            "has walked the other way; it fights where it stands",
                            most, back_way == 0 ? "left" : "right");
                back_.way = back_way;
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

        // Which way the character faces, by the packets where they steer and
        // know, over what the hunter took it to be: a turn its key missed
        // (kFacingMissedMs), one something else made (kFacingTrustMs), or the
        // Unknown a hold, the game out of focus or the character unseen left.
        face_from_packets(cfg);

        // Face it. A tap turns the character, and a skill thrown the wrong
        // way hits nothing at all -- but only when it is not facing that way
        // already, as GoC turns, and then a moment for the game to turn it.
        // Every setup, a style or none: a tap on every pass, as the hunter
        // turned without a style, keep_distance or stay_put, walked the
        // character in on the target a few pixels at a time -- 40 of the 62
        // fights in the player's log, a ranged character with a reach of 150,
        // were fought from under 50 px, and from the 450 px it is to shoot
        // from it would have walked in all the way. Standing still, those taps
        // took it off its spot too. The tap is kTurnTapMs: one the game
        // misses is not tried again at the next pass -- only once the packets
        // show it missed.
        // Never in a jump up's flight: an arrow there steers it (2026.11.05b).
        bool tapped = false;
        if (adx > cfg.facing_tolerance && facing_ != toward && !jump_in_air(now)) {
            if (auto token = coord_.acquire("hunt-face", Priority::Combat))
                tapped = press(dx < 0 ? cfg.left_key : cfg.right_key, kTurnTapMs);
            if (tapped) {
                facing_ = toward;
                facing_key_ms_ = clock_now();
                facing_key_way_ = toward;
                ++minute_turns_;
                pause(kSettleMs);
            }
        }

        // Fighting, as far as the skills are concerned: facing it, with the
        // game in front and no hold on.
        const bool facing = tapped || facing_ == toward || adx <= cfg.facing_tolerance;
        if (facing && !held && focused()) engage(adx, now);
        else                              disengage();

        if (now - last_attack_ms_ >= cfg.attack_interval_ms) {
            if (auto token = coord_.acquire("hunt-attack", Priority::Combat)) {
                // As its key went down, for the log's casts (set_on_attack).
                const std::uint64_t down_ms = clock_now();
                if (press(cfg.attack_key, 60) && on_attack_) on_attack_(cfg.attack_key, down_ms);
                last_attack_ms_ = now;
                note_attack(dx);
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
    // for something to come -- or, pushed off the hunting spot, walk back to
    // it. The lists hold here as everywhere.
    if (still) {
        const int in = choose_in_reach(cfg, mobs, me, listed);
        bool home = false;
        if (in >= 0) {
            const std::size_t i = static_cast<std::size_t>(in);
            // Its slot with its place: kept by the slot of the one out of
            // reach, the next look would go back to that one, and never
            // attack this one.
            aim_at(mobs[i]);
            target_first_ = listed[i] == Listed::First;
            target_since_ms_ = now;
            reach_ms_ = now;
            spot_busy(now);
            if (fresh_pick) note_pick(cfg, mobs, me, listed);
        } else {
            have_target_ = false;
            note_wait(cfg, mobs, me, listed, now);
            home = go_home(cfg, now);
        }
        if (!home) let_go_of_moves(cfg);
        set_state(home ? State::Returning : State::Waiting, false);
        return;
    }

    // Walk towards it -- but not with a kill's pick-ups still owed, which the
    // next pass stands for (hold_walk): walked off now, it would be let go of
    // at once.
    spot_busy(now);
    set_state(State::Approaching, false);
    if (cfg.hold_walk && cfg.loots() && loot_left_ > 0) {
        let_go_of_moves(cfg);
        return;
    }
    // A target below: dropped to through the platform underfoot when the
    // rules allow (drop_rule) -- settled first, then Ctrl + ↓ -- and held
    // still while it falls. Otherwise walked to, as ever.
    if (drop_toward(cfg, *target, me, mobs, listed, dx, dy, now)) return;
    // A target above, off the character's own floor, with "Nhảy lên bục
    // trên" in the map tab's area: walked to the take-off along the floor,
    // settled, and jumped -- held still in the air (2026.11.05b). With the
    // seek on, its own way's jump legs instead (seek_go): one planner for
    // where it jumps.
    if (!seek_on_ && jump_toward(cfg, *target, me, mobs, listed, dx, dy, now)) return;
    // The owner's Q6 of 04d: above it, a drop able to go -- stood there, and
    // the drop judged at the next look from where the character stands. Not
    // on the seek's own way to it: that goes where its plan says.
    if ((!seek_on_ || seek_.by_drop || !seek_.on) && drop_stop_above(cfg, *target, dx, dy)) {
        // The seek's drop target, its drop refused from right above it for a
        // reason the next look gives again: its own way from the next look on
        // -- a walk off an end -- for kSeekPassOverMs, the give-up clock from
        // now (2026.11.05b).
        const SeekLook* l = seek_on_ && seek_.on && seek_.by_drop ? seek_look_of(seek_.slot, seek_.gen) : nullptr;
        if (l != nullptr && l->plan && target->slot == seek_.slot && target->gen == seek_.gen &&
            drop_lasting(drop_why_pass_)) {
            bool had = false;
            for (SeekSkip& s : seek_no_drop_)
                if (s.slot == seek_.slot && s.gen == seek_.gen) {
                    s.until_ms = now + kSeekPassOverMs;
                    had = true;
                }
            if (!had) {
                if (seek_no_drop_.size() >= 16) seek_no_drop_.erase(seek_no_drop_.begin());
                seek_no_drop_.push_back(SeekSkip{ seek_.slot, seek_.gen, now + kSeekPassOverMs, SeekNo::Passed });
            }
            target_since_ms_ = now;
            GB_INFO("Hunter: slot %d -- no drop to it from above (%s); its own way instead", seek_.slot,
                    drop_why_name(drop_why_pass_));
        }
        let_go_of_moves(cfg);
        return;
    }
    // "Tự tìm quái trong vùng" (2026.11.05b): to the target's attack point --
    // along the floor, off an end, or by a jump up -- never toward its x.
    if (seek_on_ && seek_go(cfg, target, now)) return;
    // With the map tab's area in use, never a walk out of its box -- down a
    // slope, or off an end to land outside it: the owner's Q1 of 04d. A drop
    // refused above is walked by the same rule.
    if (walk_kept_in(cfg, *target, dx, dy, now)) {
        set_state(State::NoTargets, false, true);
        return;
    }
    walk(cfg, dx, dy, now);
}

} // namespace gobot
