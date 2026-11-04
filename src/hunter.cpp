#include "gobot/hunter.hpp"
#include "gobot/attack_style.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/load_meter.hpp"
#include "gobot/logger.hpp"
#include "gobot/monster_list.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
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
}

void Hunter::suspend_spot() {
    std::lock_guard<std::mutex> lock(mutex_);
    suspend_asked_ = true;
    // And the map drops go by: where the character now stands may be
    // another map's (drop_watch).
    drop_suspend_asked_ = true;
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
    minute_drops_ = minute_drop_worked_ = minute_drop_nothing_ = minute_drop_jump_ = 0;
    minute_drop_rose_ = minute_drop_off_map_ = minute_drop_gate_ = minute_drop_unknown_ = 0;
    disengage();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.dropping = false;
    status_.drops = status_.drops_worked = 0;
    status_.drop_wait = DropWhy::None;
    status_.drop_wait_px = 0;
    status_.drop_wait_ms = 0;
    status_.drop_off = DropOff::None;
    status_.drop_map_changed = status_.drop_keys_stuck = false;
    spot_ = Spot{};
    retake_asked_ = suspend_asked_ = false;
    status_.only_skipped = false;
    status_.passed_over = false;
    status_.outside_area = false;
    status_.no_target_since_ms = 0;
    status_.spot_set = false;
    status_.spot_suspended = false;
    status_.spot_login = false;
    status_.spot_why = SpotWhy::None;
    status_.spot_dx = status_.spot_dy = 0.0f;
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
    trust_.lose("the plates' view changed");
}

void Hunter::stop() {
    if (!running_.exchange(false)) return;
    stepped_ = false;
    if (thread_.joinable()) thread_.join();
    disengage();
    end_fight("stopped");
    drop_hold("stopped");
    release_movement();
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
    // spot. Toward the spot, when it is more than a floor's slope above.
    if (!cfg.jump_to_reach || cfg.jump_key == 0) return;
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
    if (press(cfg.jump_key, 50)) last_hop_ms_ = now;
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
    const bool steered = cfg.packet_positions == PacketPositions::On || what == WalkFor::Spot;
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
                          bool below_first) {
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
    char levels[160] = "";
    int used = 0;
    if (minute_nearest_below_ > 0 || minute_nearest_above_ > 0)
        used = std::snprintf(levels, sizeof levels, "; nearest off level: below %d, above %d of %d picks",
                             minute_nearest_below_, minute_nearest_above_, minute_picks_);
    if (minute_below_wait_ms_ > 0 && used >= 0 && static_cast<std::size_t>(used) < sizeof levels)
        std::snprintf(levels + used, sizeof levels - static_cast<std::size_t>(used),
                      "; waiting while a monster below was in reach: %.1f s",
                      static_cast<double>(minute_below_wait_ms_) / 1000.0);
    // And the drops this minute, only when one was pressed: what came of
    // each. Apart from the given up: a target a drop passed over is not one.
    const std::string drops = drop_minute_text();
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
    minute_drops_ = minute_drop_worked_ = minute_drop_nothing_ = minute_drop_jump_ = 0;
    minute_drop_rose_ = minute_drop_off_map_ = minute_drop_gate_ = minute_drop_unknown_ = 0;
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        spot = spot_;
        retake = retake_asked_;
        suspend = suspend_asked_;
        retake_asked_ = suspend_asked_ = false;
    }

    // "Đặt chỗ này": taken again below, at the first settled look -- this one
    // if it is. Whatever stood in the way of the old one goes with it.
    if (retake) {
        spot = Spot{};
        return_strikes_ = 0;
        return_retry_ms_ = 0;
        said_too_high_ = false;
        end_return();
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
        } else if (!spot.login) {
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
            } else if (spot.set && spot.suspended) {
                // A later entry: back where it left from -- the door there
                // and back -- or somewhere else. Set aside by a log in, an
                // entry is a look from after it either way.
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
    if (spot.set && spot.suspended && spot.login) {
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

    // None yet: where the character stands, once it stands there -- not in
    // mid-jump, where y holds at the top, nor mid-walk, worked out.
    if (cfg.spot && !spot.set && f.settled()) {
        spot = Spot{};
        spot.set = true;
        spot.x = f.x;
        spot.y = f.y;
        spot.half_w = cfg.spot_half_w;
        spot.half_h = cfg.spot_half_h;
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        spot_ = spot;
        // Not to a suspended one: its pixels may be another map's.
        status_.spot_dx = status_.spot_dy = 0.0f;
        if (spot.set && !spot.suspended && f.state == WorldFix::State::Known) {
            status_.spot_dx = spot.x - f.x;
            status_.spot_dy = spot.y - f.y;
        }
    }
    publish_spot();
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
        if (in_reach(cfg, n.x() - me.x(), n.y() - me.y())) continue;
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
        if (std::abs(wx - spot.x) > spot.half_w + keep || std::abs(wy - spot.y) > spot.half_h + keep)
            listed[i] = Listed::OutOfArea;
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
    const WorldFix& f = fix_;
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

void Hunter::publish_spot() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.spot_set = spot_.set;
    status_.spot_x = spot_.x;
    status_.spot_y = spot_.y;
    status_.spot_half_w = spot_.half_w;
    status_.spot_half_h = spot_.half_h;
    status_.spot_suspended = spot_.suspended;
    status_.spot_login = spot_.suspended && spot_.login;
    status_.spot_why = spot_why_;
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
        bool had = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            had = spot_.set;
            spot_ = Spot{};
            retake_asked_ = suspend_asked_ = false;
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
        set_state(State::Unfocused, false);
        return;
    }
    minute_log(cfg, clock_now());
    // A drop's fall judged, by this pass's fix; a settle not renewed at the
    // last pass let go of. Nothing is pressed here.
    drop_follow(cfg, clock_now());

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
    if (mobs.empty()) {
        // The last one in view has just gone, and the likeliest reason is
        // that it died. Or its plate dropped out for a look, which they do
        // all the time: the fight is over once it has not come back where it
        // was for a moment, not at once.
        if (have_target_) loot_left_ = 3;
        have_target_ = false;
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
            if (outside)
                GB_INFO("Hunter: letting go of %s -- it is outside the hunting area",
                        target->name.empty() ? "a monster" : target->name.c_str());
            target = nullptr;
            end_fight(outside ? "outside the area" : "the lists");
            watch_ = DamageWatch{};
        }

        // With a style, as GoC: the give-up clock runs only while the target
        // is out of reach, and one within reach is fought until it dies.
        // Without one it runs from the pick, in reach or not, and every fight
        // is picked afresh this often -- as it always was. When it was last in
        // reach is kept either way.
        const bool reach_now = target && in_reach(cfg, target->x() - me.x(), target->y() - me.y());
        if (reach_now) reach_ms_ = now;
        if (reach_now && cfg.goc_rules()) target_since_ms_ = now;
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
    if (!target) {
        Nameplate me_now;
        const bool have_me = see_player(me_now);
        bool wounded = false;
        // With nothing on the level, one below that a drop could go to now
        // before any other (choose_target): never with drops off, off for the
        // run, the map not trusted, or the spot on but not in use.
        const bool below_first = drop_wanted(cfg) && drop_latch_ == DropOff::None && trust_.trusted() &&
                                 (!cfg.spot || spot_mode_ == SpotMode::Use);
        const int pick = choose_target(cfg, mobs, have_me ? &me_now : nullptr, listed, &wounded, below_first);

        // Everything in view reads as finished, or the lists rule all of it
        // out. Standing still beats swinging at a corpse, which holds the
        // hunter in place while something living walks past.
        if (pick < 0) {
            // Ruled out by the lists, that is: one given up on a moment ago
            // is not the lists' doing -- it is passed over, and said apart;
            // nor is one outside the hunting spot's area.
            bool ruled_out = false, passed_over = false, outside = false;
            for (std::size_t i = 0; i < mobs.size(); ++i) {
                if (mobs[i].hp <= cfg.dead_below) continue;
                if (listed[i] == Listed::GaveUp)         passed_over = true;
                else if (listed[i] == Listed::OutOfArea) outside = true;
                else if (refused(listed[i]))             ruled_out = true;
            }
            // Nothing to fight: back to the spot, as with no monster at all.
            note_wait(cfg, mobs, me, listed, now);
            const bool home = go_home(cfg, now);
            if (!home) let_go_of_moves(cfg);
            disengage();
            set_state(home ? State::Returning : State::NoTargets, ruled_out, passed_over, outside);
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

    if (in_reach(cfg, dx, dy)) {
        let_go_of_moves(cfg);
        spot_busy(now);
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
        // kTurnTapMs of that back (kSpotStepRoomPx) -- none is taken.
        const bool in_area = spot_mode_ == SpotMode::Use;
        int room_ms = 0;
        if (in_area) {
            const Spot area = spot();
            const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
            const double room = back_way == 1 ? area.x + area.half_w - fix_.x : fix_.x - (area.x - area.half_w);
            const int ms = room < kSpotStepRoomPx ? 0 : static_cast<int>(std::lround(room / scale * 1000.0 / kWalkPxPerS));
            room_ms = ms < 50 + kTurnTapMs ? 0 : ms;
        }
        if (cfg.keep_distance && !still && !just_stepped_ && adx < away && !held && !drop_in_air(now) &&
            !retreat.blocked && !back_.judge && retreat.in_row < most &&
            std::abs(dy) <= std::max(cfg.up_nj(), cfg.down_nj()) && (!in_area || room_ms > 0)) {
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
        bool tapped = false;
        if (adx > cfg.facing_tolerance && facing_ != toward) {
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
                press(cfg.attack_key, 60);
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
    walk(cfg, dx, dy, now);
}

} // namespace gobot
