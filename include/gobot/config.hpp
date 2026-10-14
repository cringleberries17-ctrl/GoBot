#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gobot {

/// Minimal INI reader/writer: `[Section]`, `key=value`, `#` or `;` comments.
/// Preserves nothing about formatting — configs are rewritten wholesale.
/// `set_in_place` is the exception, and exists for the files that cannot
/// afford that.
class IniFile {
public:
    bool load(const std::string& path);
    bool save(const std::string& path, const std::string& header = {}) const;

    /// Change one key in a file on disk, leaving everything else as it is.
    ///
    /// `save` rewrites a file out of the values this object holds and keeps
    /// nothing else, which is right for a file this program generated and
    /// wrong for one a person has written in. configs/login.ini is the second
    /// kind: it carries the measured geometry of every page of the crossing
    /// and the reasoning behind each number, and a bot that threw all of that
    /// away because somebody picked a different channel would be destroying
    /// the only record of how those numbers were arrived at.
    ///
    /// Adds the key if the section has not got one, and the section too if it
    /// is missing. The file keeps its own line endings, LF or CRLF. False if
    /// the file could not be read or written.
    static bool set_in_place(const std::string& path, const std::string& section,
                             const std::string& key, const std::string& value);
    static bool set_in_place(const std::string& path, const std::string& section,
                             const std::string& key, int value);

    std::string   get(const std::string& section, const std::string& key,
                      const std::string& def = {}) const;
    int           get_int(const std::string& section, const std::string& key, int def = 0) const;
    float         get_float(const std::string& section, const std::string& key, float def = 0.f) const;
    bool          get_bool(const std::string& section, const std::string& key, bool def = false) const;

    void set(const std::string& section, const std::string& key, const std::string& value);
    void set_int(const std::string& section, const std::string& key, int value);
    void set_float(const std::string& section, const std::string& key, float value);
    void set_bool(const std::string& section, const std::string& key, bool value);

    bool has_section(const std::string& section) const;
    bool has_key(const std::string& section, const std::string& key) const;
    std::vector<std::string> sections() const;
    std::vector<std::string> keys(const std::string& section) const;

    void clear() { data_.clear(); order_.clear(); }

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
    std::vector<std::string> order_;    ///< section write order
};

// ---------------------------------------------------------------- bot config

struct SkillSlot {
    std::string   key;              ///< display label
    std::uint32_t keycode = 0;      ///< Win32 VK
    std::uint32_t cooldown_ms = 1000;
    bool          enabled = false;
    /// Effective area relative to the player, in game units. A skill only
    /// fires when a target sits inside this box.
    float         aoe_min_x = 0, aoe_max_x = 1000;
    float         aoe_min_y = 0, aoe_max_y = 0;
    int           hits = 1;         ///< expected hits per cast
    int           targets = 1;      ///< minimum targets in range before casting
};

struct BuffSlot {
    std::string   key;
    std::uint32_t keycode = 0;
    std::uint32_t cooldown_sec = 1600;
    bool          enabled = false;
};

struct HealRule {
    bool          enabled = false;
    std::uint32_t keycode = 0;
    int           trigger_pct = 50;
};

enum class TargetMode {
    Nearest = 0,
    LowestHp = 1,
    HighestHp = 2,
    Weakest = 3,
};

struct BotConfig {
    // --- combat ---
    bool  auto_attack = true;
    bool  auto_fury = true;
    int   fury_skill_points = 0;
    bool  double_jump = false;
    bool  backing_away = true;
    bool  lazy_mode = true;              ///< act only when a target is already close
    bool  face_target = true;
    int   facing_tolerance = 12;
    bool  auto_loot = false;
    bool  ignore_fly_monster = true;
    bool  allow_attack_while_jumping = false;
    bool  enable_tile_pathfinding = false;
    bool  auto_buff = true;
    bool  sequential_skill = true;
    bool  priority_boss = false;
    bool  overcome_obstacles = false;

    TargetMode target_mode = TargetMode::Nearest;
    float min_range = 0;
    float attack_range = 350;

    // Only engage inside this box (map coordinates).
    float working_min_x = 0, working_min_y = 0;
    float working_max_x = 1024, working_max_y = 1024;

    // Vertical band a target must fall in to be hittable.
    float highest_hit_y = 150;
    float lowest_hit_y  = 63;
    float higher_threshold = 150;
    float lower_threshold  = 63;

    // --- recovery ---
    bool  return_to_original_location = false;
    bool  return_to_previous_map = true;
    bool  continue_auto_after_death = false;
    int   return_delay_ms = 0;
    float return_tolerance_x = 48;
    float return_tolerance_y = 31;
    bool  fix_location_after_login = false;

    static constexpr std::size_t kSkillSlots = 8;
    static constexpr std::size_t kBuffSlots  = 6;
    SkillSlot skills[kSkillSlots];
    BuffSlot  buffs[kBuffSlots];

    // --- sustain ---
    HealRule hp_heal1, hp_heal2, mp_heal1;
    bool     player_auto_revive = false;
    int      revive_max_attempts = 3;

    // --- movement keys (Win32 VK) ---
    std::uint32_t key_jump = 17;            // VK_CONTROL
    std::uint32_t key_normal_attack = 32;   // VK_SPACE
    std::uint32_t key_fury = 96;            // VK_NUMPAD0
    std::uint32_t key_left = 37;            // VK_LEFT
    std::uint32_t key_right = 39;           // VK_RIGHT
    std::uint32_t key_up = 38;
    std::uint32_t key_down = 40;
    std::uint32_t key_loot = 90;            // 'Z'

    // --- hotkeys ---
    std::uint32_t key_start_stop = 121;     // F10

    // --- patrol ---
    bool patrol_use_waypoints = true;
    bool patrol_loop_cycle = false;
    bool patrol_sweep_mode = false;
    int  patrol_killall_goal_pct = 100;

    // --- timing (ms) ---
    int  command_delay_min = 40;
    int  command_delay_max = 90;
    int  attack_interval_ms = 120;
    int  decision_interval_ms = 80;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void apply_defaults();
};

// --------------------------------------------------------------- app config

struct AppConfig {
    // capture
    std::string   interface_name;
    std::string   server_ip_start;
    std::string   server_ip_end;
    std::uint16_t port_start = 0;
    std::uint16_t port_end = 0;
    std::string   bpf_filter;
    std::uint32_t batch_interval_ms = 50;

    /// Run automatic setup as soon as the GUI opens, instead of waiting for
    /// the button. On by default: it is what almost everyone wants, and the
    /// alternative is a window that sits there doing nothing until you find
    /// the right control.
    bool autostart = true;

    // target window
    std::string window_title = "GhostOnline";
    std::string window_class;
    std::string process_name;

    // paths
    std::string protocol_path = "configs/protocol.ini";
    std::string bot_config_path = "configs/bot_default.ini";
    std::string log_path = "gobot.log";
    std::string log_level = "info";

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

/// Resolve a path relative to the executable, so the tools work regardless of
/// the shell's working directory.
std::string exe_relative(const std::string& relative);

} // namespace gobot
