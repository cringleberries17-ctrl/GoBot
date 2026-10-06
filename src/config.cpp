#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <windows.h>

namespace gobot {

/// InputManager::key_name lives in input_manager.cpp. Declared here rather
/// than pulling the whole header into config.hpp just to label default keys.
std::string InputManagerKeyName(std::uint32_t vk);

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_bool(const std::string& v, bool def) {
    const std::string s = lower(trim(v));
    if (s == "true" || s == "1" || s == "yes" || s == "on")  return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    return def;
}

} // namespace

// ------------------------------------------------------------------- IniFile

bool IniFile::set_in_place(const std::string& path, const std::string& section,
                           const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    const std::string want_section = lower(trim(section));
    const std::string want_key     = lower(trim(key));

    bool in_section = false;
    bool found_section = false;
    std::size_t last_in_section = 0;     // last line belonging to that section

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);

        if (!line.empty() && line.front() == '[') {
            const auto close = line.find(']');
            if (close != std::string::npos) {
                in_section = lower(trim(line.substr(1, close - 1))) == want_section;
                if (in_section) { found_section = true; last_in_section = i; }
            }
            continue;
        }
        if (!in_section) continue;
        last_in_section = i;

        // Comments stay comments. A commented-out key is somebody's note about
        // what the value used to be, not the value.
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(line.substr(0, eq))) != want_key) continue;

        lines[i] = trim(key) + "=" + value;
        std::ofstream out(path, std::ios::trunc);
        if (!out) return false;
        for (const std::string& l : lines) out << l << "\n";
        return true;
    }

    // Not there. Add it: at the end of its section, or in a new one.
    const std::string added = trim(key) + "=" + value;
    if (found_section) {
        // Step back over trailing blank lines so the key joins the section
        // rather than floating in the gap before the next one.
        std::size_t at = last_in_section;
        while (at > 0 && trim(lines[at]).empty()) --at;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at) + 1, added);
    } else {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back({});
        lines.push_back("[" + trim(section) + "]");
        lines.push_back(added);
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    for (const std::string& l : lines) out << l << "\n";
    return true;
}

bool IniFile::set_in_place(const std::string& path, const std::string& section,
                           const std::string& key, int value) {
    return set_in_place(path, section, key, std::to_string(value));
}

bool IniFile::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;

    clear();
    std::string line, section;

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close == std::string::npos) continue;
            section = trim(line.substr(1, close - 1));
            if (!data_.count(section)) order_.push_back(section);
            data_[section];                       // materialise empty section
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (!data_.count(section)) order_.push_back(section);
        data_[section][key] = val;
    }
    return true;
}

bool IniFile::save(const std::string& path, const std::string& header) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    if (!header.empty()) {
        std::istringstream hs(header);
        std::string l;
        while (std::getline(hs, l)) out << "# " << l << "\n";
        out << "\n";
    }

    // Written sections first (in insertion order), then anything added later.
    std::vector<std::string> written;
    auto emit = [&](const std::string& sec) {
        auto it = data_.find(sec);
        if (it == data_.end()) return;
        out << "[" << sec << "]\n";
        for (const auto& [k, v] : it->second) out << k << "=" << v << "\n";
        out << "\n";
        written.push_back(sec);
    };

    for (const auto& sec : order_) emit(sec);
    for (const auto& [sec, _] : data_) {
        if (std::find(written.begin(), written.end(), sec) == written.end()) emit(sec);
    }
    return true;
}

std::string IniFile::get(const std::string& section, const std::string& key,
                         const std::string& def) const {
    auto s = data_.find(section);
    if (s == data_.end()) return def;
    auto k = s->second.find(key);
    return k == s->second.end() ? def : k->second;
}

int IniFile::get_int(const std::string& section, const std::string& key, int def) const {
    const std::string v = get(section, key);
    if (v.empty()) return def;
    try {
        return std::stoi(v, nullptr, 0);   // base 0 accepts 0x-prefixed values
    } catch (...) {
        return def;
    }
}

float IniFile::get_float(const std::string& section, const std::string& key, float def) const {
    const std::string v = get(section, key);
    if (v.empty()) return def;
    try {
        return std::stof(v);
    } catch (...) {
        return def;
    }
}

bool IniFile::get_bool(const std::string& section, const std::string& key, bool def) const {
    const std::string v = get(section, key);
    return v.empty() ? def : parse_bool(v, def);
}

void IniFile::set(const std::string& section, const std::string& key, const std::string& value) {
    if (!data_.count(section)) order_.push_back(section);
    data_[section][key] = value;
}

void IniFile::set_int(const std::string& s, const std::string& k, int v) {
    set(s, k, std::to_string(v));
}

void IniFile::set_float(const std::string& s, const std::string& k, float v) {
    std::ostringstream os;
    os << v;
    set(s, k, os.str());
}

void IniFile::set_bool(const std::string& s, const std::string& k, bool v) {
    set(s, k, v ? "true" : "false");
}

bool IniFile::has_section(const std::string& s) const { return data_.count(s) != 0; }

bool IniFile::has_key(const std::string& s, const std::string& k) const {
    auto it = data_.find(s);
    return it != data_.end() && it->second.count(k) != 0;
}

std::vector<std::string> IniFile::sections() const {
    std::vector<std::string> out;
    out.reserve(data_.size());
    for (const auto& [s, _] : data_) out.push_back(s);
    return out;
}

std::vector<std::string> IniFile::keys(const std::string& section) const {
    std::vector<std::string> out;
    auto it = data_.find(section);
    if (it == data_.end()) return out;
    out.reserve(it->second.size());
    for (const auto& [k, _] : it->second) out.push_back(k);
    return out;
}

// ----------------------------------------------------------------- BotConfig

void BotConfig::apply_defaults() {
    // Mirrors a typical melee setup: four skills on ZXCV, buffs on 3/4/6,
    // potions on 1/2. Everything is overridable from the INI.
    const std::uint32_t skill_keys[] = { 'Z', 'X', 'C', 'V', VK_SPACE, 'N', 0, 0 };
    for (std::size_t i = 0; i < kSkillSlots; ++i) {
        skills[i].keycode     = skill_keys[i];
        skills[i].key         = InputManagerKeyName(skill_keys[i]);
        skills[i].enabled     = skill_keys[i] != 0 && i < 4;
        skills[i].cooldown_ms = 1000;
        skills[i].aoe_min_x   = 0;
        skills[i].aoe_max_x   = 1000;
        skills[i].hits        = 1;
        skills[i].targets     = 1;
    }

    const std::uint32_t buff_keys[] = { '3', '4', '6', 0, 0, 0 };
    const std::uint32_t buff_cds[]  = { 1600, 1600, 60, 0, 0, 0 };
    for (std::size_t i = 0; i < kBuffSlots; ++i) {
        buffs[i].keycode      = buff_keys[i];
        buffs[i].cooldown_sec = buff_cds[i];
        buffs[i].enabled      = buff_keys[i] != 0;
    }

    hp_heal1 = { true, '1', 50 };
    mp_heal1 = { true, '2', 50 };
}

bool BotConfig::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) {
        GB_WARN("BotConfig: cannot read %s -- using defaults", path.c_str());
        apply_defaults();
        return false;
    }

    apply_defaults();

    auto B = [&](const char* k, bool d)  { return ini.get_bool("Bot", k, d); };
    auto F = [&](const char* k, float d) { return ini.get_float("Bot", k, d); };
    auto I = [&](const char* k, int d)   { return ini.get_int("Bot", k, d); };

    auto_attack                = B("auto_attack", auto_attack);
    auto_fury                  = B("auto_fury", auto_fury);
    fury_skill_points          = I("fury_skill_points", fury_skill_points);
    double_jump                = B("double_jump", double_jump);
    backing_away               = B("backing_away", backing_away);
    lazy_mode                  = B("lazy_mode", lazy_mode);
    face_target                = B("face_target", face_target);
    facing_tolerance           = I("facing_tolerance", facing_tolerance);
    auto_loot                  = B("auto_loot", auto_loot);
    ignore_fly_monster         = B("ignore_fly_monster", ignore_fly_monster);
    allow_attack_while_jumping = B("allow_attack_while_jumping", allow_attack_while_jumping);
    enable_tile_pathfinding    = B("enable_tile_pathfinding", enable_tile_pathfinding);
    auto_buff                  = B("auto_buff", auto_buff);
    sequential_skill           = B("sequential_skill", sequential_skill);
    priority_boss              = B("priority_boss", priority_boss);
    overcome_obstacles         = B("overcome_obstacles", overcome_obstacles);

    target_mode  = static_cast<TargetMode>(I("target_mode", 0));
    min_range    = F("min_range", min_range);
    attack_range = F("attack_range", attack_range);

    working_min_x = F("working_min_x", working_min_x);
    working_min_y = F("working_min_y", working_min_y);
    working_max_x = F("working_max_x", working_max_x);
    working_max_y = F("working_max_y", working_max_y);

    highest_hit_y    = F("highest_hit_y", highest_hit_y);
    lowest_hit_y     = F("lowest_hit_y", lowest_hit_y);
    higher_threshold = F("higher_threshold", higher_threshold);
    lower_threshold  = F("lower_threshold", lower_threshold);

    return_to_original_location = B("return_to_original_location", return_to_original_location);
    return_to_previous_map      = B("return_to_previous_map", return_to_previous_map);
    continue_auto_after_death   = B("continue_auto_after_death", continue_auto_after_death);
    fix_location_after_login    = B("fix_location_after_login", fix_location_after_login);
    return_delay_ms             = I("return_delay", return_delay_ms);
    return_tolerance_x          = F("return_tolerance_x", return_tolerance_x);
    return_tolerance_y          = F("return_tolerance_y", return_tolerance_y);

    for (std::size_t i = 0; i < kSkillSlots; ++i) {
        const std::string p = "skill_" + std::to_string(i) + "_";
        skills[i].keycode     = static_cast<std::uint32_t>(ini.get_int("Bot", p + "keycode", skills[i].keycode));
        skills[i].key         = ini.get("Bot", p + "key", skills[i].key);
        skills[i].cooldown_ms = static_cast<std::uint32_t>(ini.get_int("Bot", p + "cooldown", static_cast<int>(skills[i].cooldown_ms)));
        skills[i].enabled     = ini.get_bool("Bot", p + "enabled", skills[i].enabled);
        skills[i].aoe_min_x   = ini.get_float("Bot", p + "aoe_min_x", skills[i].aoe_min_x);
        skills[i].aoe_max_x   = ini.get_float("Bot", p + "aoe_max_x", skills[i].aoe_max_x);
        skills[i].aoe_min_y   = ini.get_float("Bot", p + "aoe_min_y", skills[i].aoe_min_y);
        skills[i].aoe_max_y   = ini.get_float("Bot", p + "aoe_max_y", skills[i].aoe_max_y);
        skills[i].hits        = ini.get_int("Bot", p + "hits", skills[i].hits);
        skills[i].targets     = ini.get_int("Bot", p + "targets", skills[i].targets);

        // The reference config stores cooldowns in seconds for values that are
        // implausibly small as milliseconds. Normalise to ms.
        if (skills[i].cooldown_ms > 0 && skills[i].cooldown_ms < 50)
            skills[i].cooldown_ms *= 1000;
    }

    for (std::size_t i = 0; i < kBuffSlots; ++i) {
        const std::string p = "buff_" + std::to_string(i) + "_";
        buffs[i].keycode      = static_cast<std::uint32_t>(ini.get_int("Bot", p + "keycode", buffs[i].keycode));
        buffs[i].key          = ini.get("Bot", p + "key", buffs[i].key);
        buffs[i].cooldown_sec = static_cast<std::uint32_t>(ini.get_int("Bot", p + "cooldown", static_cast<int>(buffs[i].cooldown_sec)));
        buffs[i].enabled      = ini.get_bool("Bot", p + "enabled", buffs[i].enabled);
    }

    hp_heal1.enabled     = ini.get_bool("Heal", "hp_heal1_enabled", hp_heal1.enabled);
    hp_heal1.keycode     = static_cast<std::uint32_t>(ini.get_int("Heal", "hp_heal1_keycode", static_cast<int>(hp_heal1.keycode)));
    hp_heal1.trigger_pct = ini.get_int("Heal", "hp_heal1_trigger", hp_heal1.trigger_pct);

    hp_heal2.enabled     = ini.get_bool("Heal", "hp_heal2_enabled", hp_heal2.enabled);
    hp_heal2.keycode     = static_cast<std::uint32_t>(ini.get_int("Heal", "hp_heal2_keycode", static_cast<int>(hp_heal2.keycode)));
    hp_heal2.trigger_pct = ini.get_int("Heal", "hp_heal2_trigger", hp_heal2.trigger_pct);

    mp_heal1.enabled     = ini.get_bool("Heal", "mp_heal1_enabled", mp_heal1.enabled);
    mp_heal1.keycode     = static_cast<std::uint32_t>(ini.get_int("Heal", "mp_heal1_keycode", static_cast<int>(mp_heal1.keycode)));
    mp_heal1.trigger_pct = ini.get_int("Heal", "mp_heal1_trigger", mp_heal1.trigger_pct);

    player_auto_revive  = ini.get_bool("Heal", "player_auto_revive_enabled", player_auto_revive);
    revive_max_attempts = ini.get_int("Heal", "player_revive_max_attempts", revive_max_attempts);

    key_jump          = static_cast<std::uint32_t>(ini.get_int("KeyConfig", "key_jump", static_cast<int>(key_jump)));
    key_normal_attack = static_cast<std::uint32_t>(ini.get_int("KeyConfig", "key_normal_attack", static_cast<int>(key_normal_attack)));
    key_fury          = static_cast<std::uint32_t>(ini.get_int("KeyConfig", "key_fury", static_cast<int>(key_fury)));
    key_left          = static_cast<std::uint32_t>(ini.get_int("KeyConfig", "key_left", static_cast<int>(key_left)));
    key_right         = static_cast<std::uint32_t>(ini.get_int("KeyConfig", "key_right", static_cast<int>(key_right)));
    key_up            = static_cast<std::uint32_t>(ini.get_int("KeyConfig", "key_up", static_cast<int>(key_up)));
    key_down          = static_cast<std::uint32_t>(ini.get_int("KeyConfig", "key_down", static_cast<int>(key_down)));
    key_loot          = static_cast<std::uint32_t>(ini.get_int("KeyConfig", "key_loot", static_cast<int>(key_loot)));
    key_start_stop    = static_cast<std::uint32_t>(ini.get_int("KeyConfig", "key_start_stop", static_cast<int>(key_start_stop)));

    patrol_use_waypoints    = ini.get_bool("Patrol", "patrol_use_waypoints", patrol_use_waypoints);
    patrol_loop_cycle       = ini.get_bool("Patrol", "patrol_loop_cycle", patrol_loop_cycle);
    patrol_sweep_mode       = ini.get_bool("Patrol", "patrol_sweep_mode", patrol_sweep_mode);
    patrol_killall_goal_pct = ini.get_int("Patrol", "patrol_killall_goal_pct", patrol_killall_goal_pct);

    command_delay_min    = ini.get_int("Timing", "command_delay_min", command_delay_min);
    command_delay_max    = ini.get_int("Timing", "command_delay_max", command_delay_max);
    attack_interval_ms   = ini.get_int("Timing", "attack_interval_ms", attack_interval_ms);
    decision_interval_ms = ini.get_int("Timing", "decision_interval_ms", decision_interval_ms);

    if (command_delay_max < command_delay_min) std::swap(command_delay_min, command_delay_max);

    GB_INFO("BotConfig loaded from %s", path.c_str());
    return true;
}

bool BotConfig::save(const std::string& path) const {
    IniFile ini;

    ini.set_bool("Bot", "auto_attack", auto_attack);
    ini.set_bool("Bot", "auto_fury", auto_fury);
    ini.set_int ("Bot", "fury_skill_points", fury_skill_points);
    ini.set_bool("Bot", "double_jump", double_jump);
    ini.set_bool("Bot", "backing_away", backing_away);
    ini.set_bool("Bot", "lazy_mode", lazy_mode);
    ini.set_bool("Bot", "face_target", face_target);
    ini.set_int ("Bot", "facing_tolerance", facing_tolerance);
    ini.set_bool("Bot", "auto_loot", auto_loot);
    ini.set_bool("Bot", "ignore_fly_monster", ignore_fly_monster);
    ini.set_bool("Bot", "allow_attack_while_jumping", allow_attack_while_jumping);
    ini.set_bool("Bot", "enable_tile_pathfinding", enable_tile_pathfinding);
    ini.set_bool("Bot", "auto_buff", auto_buff);
    ini.set_bool("Bot", "sequential_skill", sequential_skill);
    ini.set_bool("Bot", "priority_boss", priority_boss);
    ini.set_bool("Bot", "overcome_obstacles", overcome_obstacles);
    ini.set_int ("Bot", "target_mode", static_cast<int>(target_mode));
    ini.set_float("Bot", "min_range", min_range);
    ini.set_float("Bot", "attack_range", attack_range);
    ini.set_float("Bot", "working_min_x", working_min_x);
    ini.set_float("Bot", "working_min_y", working_min_y);
    ini.set_float("Bot", "working_max_x", working_max_x);
    ini.set_float("Bot", "working_max_y", working_max_y);
    ini.set_float("Bot", "highest_hit_y", highest_hit_y);
    ini.set_float("Bot", "lowest_hit_y", lowest_hit_y);
    ini.set_float("Bot", "higher_threshold", higher_threshold);
    ini.set_float("Bot", "lower_threshold", lower_threshold);
    ini.set_bool("Bot", "return_to_original_location", return_to_original_location);
    ini.set_bool("Bot", "return_to_previous_map", return_to_previous_map);
    ini.set_bool("Bot", "continue_auto_after_death", continue_auto_after_death);
    ini.set_bool("Bot", "fix_location_after_login", fix_location_after_login);
    ini.set_int ("Bot", "return_delay", return_delay_ms);
    ini.set_float("Bot", "return_tolerance_x", return_tolerance_x);
    ini.set_float("Bot", "return_tolerance_y", return_tolerance_y);

    for (std::size_t i = 0; i < kSkillSlots; ++i) {
        const std::string p = "skill_" + std::to_string(i) + "_";
        ini.set      ("Bot", p + "key", skills[i].key);
        ini.set_int  ("Bot", p + "keycode", static_cast<int>(skills[i].keycode));
        ini.set_int  ("Bot", p + "cooldown", static_cast<int>(skills[i].cooldown_ms));
        ini.set_bool ("Bot", p + "enabled", skills[i].enabled);
        ini.set_float("Bot", p + "aoe_min_x", skills[i].aoe_min_x);
        ini.set_float("Bot", p + "aoe_max_x", skills[i].aoe_max_x);
        ini.set_float("Bot", p + "aoe_min_y", skills[i].aoe_min_y);
        ini.set_float("Bot", p + "aoe_max_y", skills[i].aoe_max_y);
        ini.set_int  ("Bot", p + "hits", skills[i].hits);
        ini.set_int  ("Bot", p + "targets", skills[i].targets);
    }

    for (std::size_t i = 0; i < kBuffSlots; ++i) {
        const std::string p = "buff_" + std::to_string(i) + "_";
        ini.set     ("Bot", p + "key", buffs[i].key);
        ini.set_int ("Bot", p + "keycode", static_cast<int>(buffs[i].keycode));
        ini.set_int ("Bot", p + "cooldown", static_cast<int>(buffs[i].cooldown_sec));
        ini.set_bool("Bot", p + "enabled", buffs[i].enabled);
    }

    ini.set_bool("Heal", "hp_heal1_enabled", hp_heal1.enabled);
    ini.set_int ("Heal", "hp_heal1_keycode", static_cast<int>(hp_heal1.keycode));
    ini.set_int ("Heal", "hp_heal1_trigger", hp_heal1.trigger_pct);
    ini.set_bool("Heal", "hp_heal2_enabled", hp_heal2.enabled);
    ini.set_int ("Heal", "hp_heal2_keycode", static_cast<int>(hp_heal2.keycode));
    ini.set_int ("Heal", "hp_heal2_trigger", hp_heal2.trigger_pct);
    ini.set_bool("Heal", "mp_heal1_enabled", mp_heal1.enabled);
    ini.set_int ("Heal", "mp_heal1_keycode", static_cast<int>(mp_heal1.keycode));
    ini.set_int ("Heal", "mp_heal1_trigger", mp_heal1.trigger_pct);
    ini.set_bool("Heal", "player_auto_revive_enabled", player_auto_revive);
    ini.set_int ("Heal", "player_revive_max_attempts", revive_max_attempts);

    ini.set_int("KeyConfig", "key_jump", static_cast<int>(key_jump));
    ini.set_int("KeyConfig", "key_normal_attack", static_cast<int>(key_normal_attack));
    ini.set_int("KeyConfig", "key_fury", static_cast<int>(key_fury));
    ini.set_int("KeyConfig", "key_left", static_cast<int>(key_left));
    ini.set_int("KeyConfig", "key_right", static_cast<int>(key_right));
    ini.set_int("KeyConfig", "key_up", static_cast<int>(key_up));
    ini.set_int("KeyConfig", "key_down", static_cast<int>(key_down));
    ini.set_int("KeyConfig", "key_loot", static_cast<int>(key_loot));
    ini.set_int("KeyConfig", "key_start_stop", static_cast<int>(key_start_stop));

    ini.set_bool("Patrol", "patrol_use_waypoints", patrol_use_waypoints);
    ini.set_bool("Patrol", "patrol_loop_cycle", patrol_loop_cycle);
    ini.set_bool("Patrol", "patrol_sweep_mode", patrol_sweep_mode);
    ini.set_int ("Patrol", "patrol_killall_goal_pct", patrol_killall_goal_pct);

    ini.set_int("Timing", "command_delay_min", command_delay_min);
    ini.set_int("Timing", "command_delay_max", command_delay_max);
    ini.set_int("Timing", "attack_interval_ms", attack_interval_ms);
    ini.set_int("Timing", "decision_interval_ms", decision_interval_ms);

    return ini.save(path, "GoBot bot configuration");
}

// ----------------------------------------------------------------- AppConfig

bool AppConfig::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) {
        GB_WARN("AppConfig: cannot read %s -- using defaults", path.c_str());
        return false;
    }

    interface_name    = ini.get("Capture", "interface", interface_name);
    server_ip_start   = ini.get("Capture", "server_ip_start", server_ip_start);
    server_ip_end     = ini.get("Capture", "server_ip_end", server_ip_end);
    port_start        = static_cast<std::uint16_t>(ini.get_int("Capture", "port_start", port_start));
    port_end          = static_cast<std::uint16_t>(ini.get_int("Capture", "port_end", port_end));
    bpf_filter        = ini.get("Capture", "bpf_filter", bpf_filter);
    batch_interval_ms = static_cast<std::uint32_t>(ini.get_int("Capture", "batch_interval_ms", static_cast<int>(batch_interval_ms)));
    autostart         = ini.get_bool("Capture", "autostart", autostart);

    window_title  = ini.get("Window", "title", window_title);
    window_class  = ini.get("Window", "class", window_class);
    process_name  = ini.get("Window", "process", process_name);

    protocol_path   = ini.get("Paths", "protocol", protocol_path);
    bot_config_path = ini.get("Paths", "bot_config", bot_config_path);
    log_path        = ini.get("Paths", "log", log_path);
    log_level       = ini.get("Paths", "log_level", log_level);

    return true;
}

bool AppConfig::save(const std::string& path) const {
    IniFile ini;
    ini.set    ("Capture", "interface", interface_name);
    ini.set    ("Capture", "server_ip_start", server_ip_start);
    ini.set    ("Capture", "server_ip_end", server_ip_end);
    ini.set_int("Capture", "port_start", port_start);
    ini.set_int("Capture", "port_end", port_end);
    ini.set    ("Capture", "bpf_filter", bpf_filter);
    ini.set_int ("Capture", "batch_interval_ms", static_cast<int>(batch_interval_ms));
    ini.set_bool("Capture", "autostart", autostart);

    ini.set("Window", "title", window_title);
    ini.set("Window", "class", window_class);
    ini.set("Window", "process", process_name);

    ini.set("Paths", "protocol", protocol_path);
    ini.set("Paths", "bot_config", bot_config_path);
    ini.set("Paths", "log", log_path);
    ini.set("Paths", "log_level", log_level);

    return ini.save(path, "GoBot application configuration");
}

std::string exe_relative(const std::string& relative) {
    char buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return relative;

    std::string dir(buf, n);
    const auto slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) return relative;
    dir.resize(slash + 1);
    return dir + relative;
}

} // namespace gobot
