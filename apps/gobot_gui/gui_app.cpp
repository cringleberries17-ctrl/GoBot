#include "gui_app.hpp"
#include "gobot/alerts.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/jump_check.hpp"
#include "gobot/reviver.hpp"
#include "gobot/routine.hpp"
#include "gobot/screen_layout.hpp"
#include "gobot/session_manager.hpp"

#include "gobot/byte_reader.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/no_real_input.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>

#include <windows.h>    // GetAsyncKeyState for the global bot toggle
#include <shellapi.h>   // ShellExecuteEx, to restart ourselves elevated

namespace gobot {
namespace {

// ---- palette ------------------------------------------------------------
const ImVec4 kGreen (0.35f, 0.82f, 0.42f, 1.0f);
const ImVec4 kRed   (0.92f, 0.35f, 0.35f, 1.0f);
const ImVec4 kAmber (0.95f, 0.72f, 0.25f, 1.0f);
const ImVec4 kDim   (0.60f, 0.62f, 0.66f, 1.0f);
const ImVec4 kBlue  (0.40f, 0.66f, 0.95f, 1.0f);

void text_dim(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void help_marker(const char* desc) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// Status pill: a coloured dot plus a label.
///
/// The dot is drawn rather than written: ImGui's default font carries only
/// Latin-1, so a U+25CF bullet renders as a question mark.
void status_dot(bool good, const char* label, const char* bad_label = nullptr) {
    const float r = ImGui::GetFontSize() * 0.28f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cy = p.y + ImGui::GetTextLineHeight() * 0.5f;

    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + r + 1.0f, cy), r, ImGui::GetColorU32(good ? kGreen : kRed));

    ImGui::Dummy(ImVec2(r * 2.0f + 4.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextUnformatted(good ? label : (bad_label ? bad_label : label));
}

const char* const kSemanticNames[] = {
    "unknown", "self_state", "self_hpmp", "self_move", "self_dead", "self_revive",
    "self_exp", "monster_spawn", "monster_update", "monster_remove",
    "monster_selected", "player_spawn", "player_move", "player_remove",
    "item_drop", "item_remove", "map_warp", "map_data", "party_update",
    "party_invite", "party_leave", "party_hp", "chat", "skill_confirm",
    "attack_confirm", "buff_applied", "disconnect",
};
constexpr int kSemanticCount = static_cast<int>(sizeof(kSemanticNames) / sizeof(kSemanticNames[0]));

int semantic_to_index(Semantic s) {
    const char* name = to_string(s);
    for (int i = 0; i < kSemanticCount; ++i) {
        if (std::strcmp(kSemanticNames[i], name) == 0) return i;
    }
    return 0;
}

void copy_to(char* dst, std::size_t cap, const std::string& src) {
    std::snprintf(dst, cap, "%s", src.c_str());
}

/// The hold a chat line is typed under, and kept under while the chat line
/// may be open, by name.
const char* const kChatHold = "auto chat";

/// What a trash pass says while it waits for the chat line to be seen shut.
const char* const kTrashWaitsForChat = "Chờ: ô chat trong game có thể đang mở, dọn rác sau.";

} // namespace

// =========================================================================

GuiApp::GuiApp() = default;

GuiApp::~GuiApp() {
    // A jump key's "Thử" under way presses nothing more: GoBot is closing.
    simple_style_stop();
    if (calibrator_) calibrator_->stop();
    if (mapper_) mapper_->stop();
    if (navigator_) navigator_->stop();
    stop_hunt();
    stop_detector();
    // These own a thread that clicks the game or types into it. Joined before
    // anything else goes away: a worker still running while the things it
    // reads are torn down is a worker clicking at a target that is no longer
    // there. The chat first: a line half typed is rubbed out and closed as it
    // stops, and it holds everything else off until then.
    stop_chat();
    stop_party();
    stop_keeper();
    stop_revive();
    stop_trash();
    stop_quest();
    stop_popups();
    // A look at the game for the bin, or for the chat line, may still be under
    // way; it reads the window handle and its own pictures, which go away
    // below. So may the login tab's look at VTCZone, with pictures of its own.
    if (left_held_look_.valid()) left_held_look_.wait();
    if (chat_doubt_look_.valid()) chat_doubt_look_.wait();
    if (login_look_.valid()) login_look_.wait();
    stop_vitals();
    stop_macro();
    stop_bot();
    stop_capture();
    // Last, once nothing is left to raise an alert: it ends a request under
    // way and forgets the webhook's address.
    alerts_shutdown();
}

void GuiApp::initialise() {
    Logger::instance().set_ring_capacity(2000);

    // A windowed app has no console, so stderr goes nowhere. Without a file
    // sink a startup failure is completely silent.
    //
    // Started afresh each time when app.ini says so, as the package's does:
    // a tester's log, sent with a problem, then holds this session and the
    // one before (gobot_gui.old.log) -- not every character name, chat line
    // and folder path since the day they unzipped it. Without the key, as in
    // a configs folder older than it, every session is added to the one file.
    IniFile app_ini;
    const bool fresh_log = app_ini.load(exe_relative("configs/app.ini")) &&
                           app_ini.get_bool("Paths", "new_log_each_start", false);
    Logger::instance().set_file(exe_relative("gobot_gui.log"), fresh_log);
    // The version first, so that every log a tester sends says which build
    // it came from.
    std::string build = package_version();
    if (build.empty()) build = "(not packaged: no VERSION.txt)";
    if (!package_commit().empty()) build += " (" + package_commit() + ")";
    GB_INFO("GoBot GUI %s starting (elevated: %s)", build.c_str(), elevated_ ? "yes" : "no");

    // Which matcher is live. Linked statically, so there is no DLL in the
    // build folder to see it by, and "no DLLs" looks exactly like "not used".
    GB_INFO("Template matching: %s", TemplateMatcher::backend().c_str());

    app_config_.load(exe_relative("configs/app.ini"));
    bot_config_.load(exe_relative(app_config_.bot_config_path));

    const std::string proto = exe_relative(app_config_.protocol_path);
    registry_.load(proto);

    IniFile ini;
    if (ini.load(proto) && ini.has_section("Framing")) {
        framing_.header_size   = static_cast<std::uint32_t>(ini.get_int("Framing", "header_size", 4));
        framing_.length_offset = static_cast<std::uint32_t>(ini.get_int("Framing", "length_offset", 0));
        framing_.length_size   = static_cast<std::uint32_t>(ini.get_int("Framing", "length_size", 2));
        framing_.length_le     = ini.get("Framing", "length_endian", "little") != "big";
        framing_.length_includes_header = ini.get_bool("Framing", "length_includes_header", true);
        framing_.length_adjust = ini.get_int("Framing", "length_adjust", 0);
        framing_.opcode_offset = static_cast<std::uint32_t>(ini.get_int("Framing", "opcode_offset", 2));
        framing_.opcode_size   = static_cast<std::uint32_t>(ini.get_int("Framing", "opcode_size", 2));
        framing_.opcode_le     = ini.get("Framing", "opcode_endian", "little") != "big";
        framing_.min_frame     = static_cast<std::uint32_t>(ini.get_int("Framing", "min_frame", 4));
        framing_.max_frame     = static_cast<std::uint32_t>(ini.get_int("Framing", "max_frame", 32768));
        framing_.checksum_offset = static_cast<std::uint32_t>(
            ini.get_int("Framing", "checksum_offset", static_cast<int>(FramingSpec::kNoChecksum)));
        framing_.trailer_opcode = static_cast<std::uint16_t>(
            ini.get_int("Framing", "trailer_opcode", 0));
        framing_.trailer_bytes = static_cast<std::uint32_t>(
            ini.get_int("Framing", "trailer_bytes", 0));
        // 13 when missing, as in gobot::load_framing: a player's protocol.ini
        // is never replaced by a build and predates the key.
        framing_.inner_offset = static_cast<std::uint32_t>(
            ini.get_int("Framing", "inner_frame_offset", 13));
        framing_verified_      = ini.get_bool("Framing", "verified", false);
    }

    // The client's half of the conversation is framed differently and needs its
    // own spec, or it is discarded as noise.
    if (ini.has_section("FramingClient")) {
        const std::string C = "FramingClient";
        client_framing_.header_size   = static_cast<std::uint32_t>(ini.get_int(C, "header_size", 8));
        client_framing_.length_offset = static_cast<std::uint32_t>(ini.get_int(C, "length_offset", 4));
        client_framing_.length_size   = static_cast<std::uint32_t>(ini.get_int(C, "length_size", 2));
        client_framing_.length_le     = ini.get(C, "length_endian", "little") != "big";
        client_framing_.length_includes_header = ini.get_bool(C, "length_includes_header", false);
        client_framing_.length_adjust = ini.get_int(C, "length_adjust", 0);
        client_framing_.opcode_offset = static_cast<std::uint32_t>(ini.get_int(C, "opcode_offset", 6));
        client_framing_.opcode_size   = static_cast<std::uint32_t>(ini.get_int(C, "opcode_size", 2));
        client_framing_.opcode_le     = ini.get(C, "opcode_endian", "little") != "big";
        client_framing_.min_frame     = static_cast<std::uint32_t>(ini.get_int(C, "min_frame", 8));
        client_framing_.max_frame     = static_cast<std::uint32_t>(ini.get_int(C, "max_frame", 32768));
        client_framing_.checksum_offset = static_cast<std::uint32_t>(
            ini.get_int(C, "checksum_offset", static_cast<int>(FramingSpec::kNoChecksum)));
        client_framing_.trailer_opcode = static_cast<std::uint16_t>(ini.get_int(C, "trailer_opcode", 0));
        client_framing_.trailer_bytes  = static_cast<std::uint32_t>(ini.get_int(C, "trailer_bytes", 0));
        has_client_framing_ = client_framing_.plausible();
    }

    protocol_ = std::make_unique<ProtocolHandler>(registry_, state_);
    executor_ = std::make_unique<CommandExecutor>(bot_config_, state_, input_, coord_);
    brain_    = std::make_unique<BotBrain>(bot_config_, state_, *executor_);

    executor_->set_pathfinder(&pathfinder_);
    brain_->set_pathfinder(&pathfinder_);

    protocol_->set_map_data_callback([this](const MapData& map) {
        pathfinder_.set_double_jump(bot_config_.double_jump);
        pathfinder_.init(map);
    });
    protocol_->set_death_callback([this] {
        if (!bot_config_.continue_auto_after_death) stop_bot();
    });

    capture_.set_packet_callback([this](const std::vector<GamePacket>& p) { on_packets(p); });
    capture_.set_raw_tap([this](const FlowKey& key, Direction dir,
                                const std::uint8_t* d, std::size_t n) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) return;

        if (dir == Direction::ToClient) bytes_to_client_ += n;
        else                            bytes_to_server_ += n;

        if (dir != Direction::ToClient) return;

        // Keep streams apart rather than merging them. Framing cannot be
        // detected in a mixture of connections, and holding them separately
        // means we can simply detect on the busiest one instead of asking
        // the user which connection is the game.
        const std::uint64_t id = FlowKeyHash{}(key);
        auto& buf = flow_samples_[id];
        if (flow_labels_.find(id) == flow_labels_.end()) flow_labels_[id] = key.to_string();

        if (buf.size() < kMaxSample) {
            const std::size_t take = std::min(n, kMaxSample - buf.size());
            buf.insert(buf.end(), d, d + take);
        }

        // Legacy single-buffer view, for the manual Record button.
        if (raw_sample_.size() < kMaxSample) {
            const std::size_t take = std::min(n, kMaxSample - raw_sample_.size());
            raw_sample_.insert(raw_sample_.end(), d, d + take);
        }
    });
    capture_.set_disconnect_callback([this] {
        GB_WARN("Traffic stopped -- disabling bot");
        stop_bot();
    });

    copy_to(server_ip_,    sizeof(server_ip_),    app_config_.server_ip_start);
    copy_to(bpf_filter_,   sizeof(bpf_filter_),   app_config_.bpf_filter);
    copy_to(window_title_, sizeof(window_title_), app_config_.window_title);
    if (app_config_.port_start != 0)
        std::snprintf(server_port_, sizeof(server_port_), "%u", app_config_.port_start);

    refresh_interfaces();

    // Only auto-select a window when the config names something specific.
    // A bare title substring is too loose to act on unprompted — it will pick
    // an editor or browser tab named after the game — so otherwise we just
    // load the list and let the user choose on the Capture tab.
    windows_ = InputManager::enumerate_windows();
    window_list_loaded_ = true;
    if (!app_config_.process_name.empty() || !app_config_.window_class.empty()) {
        input_.find_game_window(app_config_.window_title, app_config_.window_class,
                                app_config_.process_name);
    }

    macro_config_.load(exe_relative("configs/macro.ini"));
    macro_ = std::make_unique<MacroEngine>(input_, coord_);
    macro_->set_config(macro_config_);

    apply_vitals_config();
    if (macro_config_.vitals_enabled && macro_config_.hp_bar.valid() &&
        input_.window_valid()) {
        start_vitals();
    }

    input_injection_ok_ = InputManager::injection_works();
    if (!input_injection_ok_)
        GB_WARN("Synthetic keystrokes are being filtered -- nothing GoBot presses "
                "will reach the game");

    // Following the client's size unless edited by hand on the Radar tab and
    // saved, which is kept in [Detect] of hunt.ini.
    detect_tuning_ = DetectorTuning::defaults();
    {
        IniFile hunt_ini;
        if (hunt_ini.load(exe_relative("configs/hunt.ini"))) detect_tuning_.load(hunt_ini);
    }
    detector_.set_tuning(detect_tuning_);
    map_book_.load(exe_relative("configs/maps.ini"));
    hunt_config_.load(exe_relative("configs/hunt.ini"));
    hunter_ = std::make_unique<Hunter>(input_, coord_, detector_);
    hunter_->set_config(hunt_config_);
    // Skills that follow the fight ask the hunter, from the macro's own
    // threads, whether it is fighting: only its atomics are read there. No
    // hunter running is no hunter, and every key keeps its timer.
    macro_->set_fight([this] {
        MacroEngine::Fight f;
        if (!hunter_ || !hunter_->running()) return f;
        f.mode = hunter_->engaged() ? MacroEngine::Fight::Mode::Fighting
                                    : MacroEngine::Fight::Mode::Idle;
        f.dx = hunter_->engaged_dx();
        f.attack_key = hunter_->attack_key();
        return f;
    });
    load_simple_options();
    alerts_init();
    load_hotkeys();

    nav_config_.load(exe_relative("configs/hunt.ini"));
    places_.load(exe_relative("configs/waypoints.ini"));
    navigator_ = std::make_unique<Navigator>(input_, coord_, detector_, camera_);
    navigator_->set_config(nav_config_);

    mapper_ = std::make_unique<MapMapper>();
    mapper_->set_minimap(minimap_spec_);

    calibrator_ = std::make_unique<ProtocolCalibrator>(input_);

    executor_->update_config(bot_config_);
    executor_->start();

    if (app_config_.autostart) {
        GB_INFO("Autostart enabled -- running automatic setup");
        auto_start();
    }
}

void GuiApp::tick() {
    // GoBot's own keys, read whether or not the GUI has focus, so the bot can
    // be toggled from inside the game. GetAsyncKeyState rather than
    // RegisterHotKey: a registered hotkey is swallowed and never reaches the
    // game, which matters when the toggle shares a key with something. So a
    // key goes to whatever window is in front as well, and acts only with the
    // game or GoBot there -- the panic key alone from anywhere -- and never
    // while a key box is waiting for a key: binding something to it would
    // otherwise start the bot as a side effect. See simple_hotkeys.cpp.
    //
    // On the simple screen the key is that screen's one button, which starts
    // the fighting and the potions along with the timers.
    const HotkeyActions keys = hotkeys_tick();
    if (keys.toggle) {
        if (simple_view_) simple_toggle();
        else              toggle_macro();
    }

    // Panic key: stop everything and let go of every held key, unconditionally.
    // Worth a dedicated binding — when a macro misbehaves the useful response
    // is always "stop now", and hunting for the window to click is exactly
    // what you cannot do while keys are being pressed into the game.
    if (keys.panic) {
        if (navigator_) navigator_->stop();
        stop_hunt();
        stop_macro();
        stop_bot();
        // These click the game too, and the keeper can start it. The chat
        // rubs out a line half typed and closes it, under its hold, on its own
        // thread: not waited for, as that can take a few seconds. A chat line
        // it still cannot show shut is handed over below.
        stop_chat(true, false);
        stop_party();
        stop_revive();
        stop_trash();
        stop_quest();
        stop_popups();
        stop_keeper();
        simple_style_stop();    // a jump key's "Thử" under way presses nothing
        input_.release_all();
        set_status("Panic stop -- all input released", 5000);
        GB_WARN("Panic key pressed: everything stopped");
    }

    // Hunting gets its own toggle. It presses movement keys, which the macro
    // never does, so being able to stop just that without stopping the skill
    // timers is worth a key of its own.
    if (keys.hunt) toggle_hunt();

    if (hunting_ && hunter_ && !hunter_->running()) hunting_ = false;
    simple_track_run(simple_running());

    // A chat line waits while HP is under what the HP potion fires at, or 50:
    // the potion is held off while a line is typed. The macro's settings are
    // this thread's, so the number is handed over from here.
    {
        int floor = 50;
        for (const MacroKey& k : macro_config_.keys)
            if (k.enabled && k.keycode != 0 && macro_key_kind(k) == MacroKeyKind::HpPotion)
                floor = std::max(floor, k.threshold_pct);
        chat_hp_floor_ = floor;
    }

    // A stop that left the bin's dialog in doubt paused the executor's buffs
    // and potions; with no trash worker to look, they come back once the game
    // shows nothing waiting.
    watch_left_held();

    // The revive ran out of tries. The player's rule: stop the bot, never
    // revive in the village. Here rather than on the worker, which stopping
    // joins. First: the stop hands a chat line in doubt over to the block
    // below, which takes it on the next tick.
    if (revive_gave_up_.exchange(false)) stop_after_revive();

    // Tự chat was stopped by the player with the chat line maybe open, and has
    // handed it over: its hold is still on, so nothing has typed into the line
    // since. Whatever presses keys stops -- DỪNG and F12 have stopped most of
    // it already -- the executor stays paused, and the hold goes; nothing of
    // that starts again until a look shows the line shut (keys_may_start).
    if (chat_handover_.exchange(false)) {
        // Unless a look has shown it shut since (keys_may_start, for a BẮT
        // ĐẦU pressed in between): then what that started is left running.
        if (chat_doubt_) {
            if (navigator_ && navigator_->running()) navigator_->stop();
            stop_hunt();
            stop_macro();
            stop_bot();
            const std::string note = "Ô chat trong game có thể vẫn đang mở, nên GoBot dừng cả đánh "
                                     "quái, bình và buff để không gõ phím vào đó. Đóng ô chat trong game "
                                     "(xoá chữ trong đó rồi bấm Enter), rồi bấm BẮT ĐẦU.";
            simple_note(note, 15000);
            if (!simple_view_) set_status(note, 15000);
            GB_WARN("Chat: stopped while the chat line may be open; the keys are stopped too until "
                    "it is seen shut");
        }
        end_hold(kChatHold);
    }
    // The same doubt, with no worker to watch it: the executor comes back by
    // itself once the game shows the line shut.
    watch_chat_doubt();
    // A worker that has finished is joined here, rather than at the next start.
    if (!chat_running_ && chat_thread_.joinable()) chat_thread_.join();

    if (keeper_reattach_.exchange(false)) {
        const bool found = reattach_game();
        // Logged back in: every buff came off with the old session. Nothing
        // for a cold start's first login, whose jobs start below and cast
        // every buff as they start.
        if (found && macro_ && macro_->running()) macro_->recast_buffs("logging back in");
        // BẮT ĐẦU was pressed with the game closed, and the keeper has now
        // brought it up -- or found it up already, opened by hand or by
        // another tool: start what else was ticked, on that window. Only
        // while the keeper is still on -- one stopped by the panic key, or by
        // hand, in the moment between getting in and this tick means nothing
        // is to start, and starting the macro then would press keys straight
        // after somebody asked for everything to stop. A window not found
        // yet leaves the start waiting; the keeper asks again on its next
        // look.
        if (simple_cold_start_ && keeper_running_ && !keeper_stop_ && found) {
            simple_cold_start_ = false;
            keeper_cold_start_ = false;
            simple_start_jobs();
        }
    }
    // A keeper that ended without getting in leaves nothing to start.
    if (simple_cold_start_ && !keeper_running_) {
        simple_cold_start_ = false;
        keeper_cold_start_ = false;
    }
    refocus_if_idle();
    alerts_tick();
    potions_tick();
    simple_lists_tick();    // "Quái đang thấy", and the end of a look (simple_lists.cpp)
    simple_style_tick();    // a jump key's "Thử" that has finished (simple_style.cpp)

    if (detector_running_) {
        // A new map invalidates everything about where we were: the old level's
        // scenery has nothing to do with the new one's, and carrying a position
        // across would put every saved place a map's width out.
        if (minimap_.take_changed()) {
            camera_.reset();
            anchor_.reset();
            if (navigator_) navigator_->stop();
        }

        // Watch for the map running out. The character crossing the screen
        // while the view stays put is the one moment the level itself gives a
        // coordinate away, and catching it is what makes a saved position mean
        // the same thing tomorrow.
        Nameplate me;
        if (detector_.player(me)) {
            const auto fx = camera_.fix();
            const bool walking = input_.moving() ||
                                 (navigator_ && navigator_->running()) ||
                                 (hunter_ && hunter_->running());
            anchor_.update(fx.tracking ? fx.dx : 1, me.x(), walking);
        }
    }

    // Hand the current bar readings to the macro. Pushed rather than pulled so
    // the macro has no dependency on the reader, and timestamped on arrival so
    // a stalled GUI makes conditional keys hold instead of acting on a figure
    // from ten seconds ago.
    // Hold the macro whenever the character is not in the world.
    //
    // The reader is already looking at the HUD several times a second, and the
    // HUD being there is exactly what "in the world" means -- so this costs
    // nothing beyond asking. Without it a disconnect at two in the morning is
    // six hours of potion keys pressed at a dialog.
    //
    // Only while the reader is running: with no reader there is nothing to
    // judge by, and a macro suspended on no evidence is worse than one that
    // carries on.
    //
    // A suspension of ten seconds or more re-casts every buff as it ends: the
    // engine sees to that itself (see MacroEngine::set_suspended).
    if (vitals_running_ && macro_ && macro_->running()) {
        const bool in_world = vitals_.hp_percent() >= 0.0f;
        if (in_world == macro_->suspended())
            macro_->set_suspended(!in_world,
                                  in_world ? std::string{}
                                           : "the character is not in the world");
    }

    // A death costs every buff. Cast again after a revive -- the worker's,
    // or one by hand that the HUD showed: HP at 0 for a couple of seconds,
    // then above it. Asked for twice in a row, the engine casts once.
    {
        const int revived = revive_count_.load();
        if (revived != revive_seen_) {
            if (revived > revive_seen_ && macro_ && macro_->running())
                macro_->recast_buffs("a revive");
            revive_seen_ = revived;
        }
        const float hp = vitals_running_ ? vitals_.hp_percent() : -1.0f;
        if (hp >= 0.0f && hp < 0.5f) {
            if (hp_zero_since_ms_ == 0) hp_zero_since_ms_ = now_ms();
        } else if (hp > 0.0f) {
            if (hp_zero_since_ms_ != 0 && now_ms() - hp_zero_since_ms_ >= 2000 && macro_ &&
                macro_->running())
                macro_->recast_buffs("HP came back from 0");
            hp_zero_since_ms_ = 0;
        }
    }

    if (vitals_running_) {
        macro_->set_vitals(vitals_.hp_percent(), vitals_.mp_percent());

        // The reader works the bars out again by itself when the game's HUD is
        // resized under it. Take the new geometry so the panel shows it and a
        // Save keeps it, rather than silently re-deriving it every session.
        static std::uint64_t seen_relocations = 0;
        const auto vs = vitals_.stats();
        if (vs.relocations != seen_relocations) {
            seen_relocations = vs.relocations;
            macro_config_.hp_bar = vitals_.hp_bar();
            macro_config_.mp_bar = vitals_.mp_bar();
            set_status("The game's HP/MP bars moved -- found them again. "
                       "Save on the HP / MP tab to keep it.", 9000);
        }
    }

    if (bot_running_ && brain_ && !brain_->running()) {
        // The brain stopped itself (death with continue-after-death off).
        bot_running_ = false;
        set_status("Bot stopped itself");
    }

    auto_step();
    check_framing_health();
}

void GuiApp::check_framing_health() {
    if (framing_verified_ || !capture_running_) return;

    // Confirm the framing from live evidence rather than making the user find
    // a checkbox.
    //
    // The original gate conflated two separate questions. Whether the *framing*
    // is right is objectively measurable: frames either tile the stream cleanly
    // or they do not, and a wrong spec resyncs constantly. Whether a *field
    // layout* is right genuinely needs a human to compare decoded values
    // against the game — and that judgement is still required before the bot
    // runs, because it needs mapped packets too.
    //
    // Gating framing on human confirmation only produced a dead end: automatic
    // setup could derive the format perfectly and still leave the dashboard
    // insisting the packet format was not worked out.
    const auto s = capture_.stats();
    if (s.frames_decoded < 150) return;

    // Attaching mid-stream costs a one-time resync of at most a header per
    // stream. Anything beyond a couple of bytes per frame means the spec is
    // wrong, not that we joined late.
    if (s.resync_bytes > s.frames_decoded * 2) return;

    framing_verified_ = true;
    GB_INFO("Framing confirmed from live traffic: %llu frames decoded, "
            "%llu resync bytes (%.2f per frame)",
            static_cast<unsigned long long>(s.frames_decoded),
            static_cast<unsigned long long>(s.resync_bytes),
            static_cast<double>(s.resync_bytes) / static_cast<double>(s.frames_decoded));
    set_status("Packet format confirmed from live traffic", 6000);
}

void GuiApp::set_status(const std::string& msg, std::uint32_t ms) {
    status_message_ = msg;
    status_until_ms_ = now_ms() + ms;
}

// =========================================================================
//                               AUTOPILOT
// =========================================================================

const char* GuiApp::auto_stage_label() const {
    switch (auto_stage_) {
        case Auto::Idle:           return "idle";
        case Auto::FindGame:       return "Locating the game";
        case Auto::FindConnection: return "Finding the game's server connection";
        case Auto::StartCapture:   return "Starting capture";
        case Auto::Recording:      return "Recording traffic";
        case Auto::Detecting:      return "Working out the packet format";
        case Auto::Collecting:     return "Collecting packets";
        case Auto::Analysing:      return "Working out what the packets mean";
        case Auto::Done:           return "Done";
        case Auto::Failed:         return "Stopped";
    }
    return "?";
}

float GuiApp::auto_progress() const {
    const std::uint64_t elapsed = now_ms() - auto_stage_started_ms_;
    switch (auto_stage_) {
        case Auto::Recording:
            return std::min(1.0f, static_cast<float>(elapsed) / static_cast<float>(kRecordMs));
        case Auto::Collecting:
            return std::min(1.0f, static_cast<float>(elapsed) / static_cast<float>(kCollectMs));
        case Auto::Done:   return 1.0f;
        case Auto::Idle:
        case Auto::Failed: return 0.0f;
        default:           return 0.25f;
    }
}

void GuiApp::auto_start() {
    auto_error_.clear();
    auto_detail_.clear();
    suggestions_.clear();
    analysis_ran_ = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        flow_samples_.clear();
        flow_labels_.clear();
        observations_.clear();
        opcode_stats_.clear();
        samples_.clear();
    }

    auto_stage_ = Auto::FindGame;
    auto_stage_started_ms_ = now_ms();
    GB_INFO("Autopilot started");
}

void GuiApp::auto_cancel() {
    if (auto_stage_ == Auto::Idle) return;
    auto_stage_ = Auto::Idle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recording_ = false;
    }
    GB_INFO("Autopilot cancelled");
}

void GuiApp::auto_step() {
    if (auto_stage_ == Auto::Idle || auto_stage_ == Auto::Done ||
        auto_stage_ == Auto::Failed)
        return;

    const std::uint64_t elapsed = now_ms() - auto_stage_started_ms_;

    auto fail = [&](const std::string& why) {
        auto_error_ = why;
        auto_stage_ = Auto::Failed;
        GB_WARN("Autopilot: %s", why.c_str());
    };
    auto advance = [&](Auto next) {
        auto_stage_ = next;
        auto_stage_started_ms_ = now_ms();
    };

    switch (auto_stage_) {
        case Auto::FindGame: {
            windows_ = InputManager::enumerate_windows();
            window_list_loaded_ = true;

            if (!input_.window_valid()) {
                input_.find_game_window(app_config_.window_title, app_config_.window_class,
                                        app_config_.process_name);
            }
            if (!input_.window_valid()) {
                fail("Could not find the game window. Start the game, then pick it "
                     "from the list on the Capture tab and run this again.");
                return;
            }

            game_pid_ = pid_for_window(input_.target_window());
            if (game_pid_ == 0) {
                fail("Found the game window but could not identify its process.");
                return;
            }
            auto_detail_ = "Game process id " + std::to_string(game_pid_);
            advance(Auto::FindConnection);
            break;
        }

        case Auto::FindConnection: {
            game_connections_ = game_connection_candidates(game_pid_);

            if (game_connections_.empty()) {
                // Retry briefly: the socket may not be up yet at a login screen.
                if (elapsed < 5000) return;
                fail("The game has no server connection open. Log in to a character "
                     "first, then run this again.");
                return;
            }

            std::ostringstream d;
            d << game_connections_.size() << " connection"
              << (game_connections_.size() == 1 ? "" : "s") << ": ";
            for (std::size_t i = 0; i < game_connections_.size(); ++i) {
                if (i) d << ", ";
                d << ip_to_string(game_connections_[i].remote.ip) << ":"
                  << game_connections_[i].remote.port;
            }
            auto_detail_ = d.str();
            GB_INFO("Autopilot: %s", auto_detail_.c_str());
            advance(Auto::StartCapture);
            break;
        }

        case Auto::StartCapture: {
            if (capture_running_) stop_capture();

            // Point the adapter at whichever local address the game's own
            // socket is bound to — no guessing, no default-route heuristic.
            const std::string local_ip = ip_to_string(game_connections_.front().local.ip);
            std::string iface = PacketCapture::find_interface_by_ip(local_ip);
            if (iface.empty()) iface = PacketCapture::default_interface();

            for (std::size_t i = 0; i < interfaces_.size(); ++i) {
                if (interfaces_[i].name == iface) { selected_interface_ = static_cast<int>(i); break; }
            }

            const std::string filter = build_filter_for(game_connections_);
            copy_to(bpf_filter_, sizeof(bpf_filter_), filter);
            copy_to(server_ip_, sizeof(server_ip_),
                    ip_to_string(game_connections_.front().remote.ip));
            std::snprintf(server_port_, sizeof(server_port_), "%u",
                          game_connections_.front().remote.port);

            CaptureConfig cfg;
            cfg.interface_name = iface;
            cfg.bpf_filter     = filter;

            // We read these straight out of the game's socket table, so
            // direction can be labelled exactly rather than guessed.
            for (const auto& c : game_connections_) cfg.known_servers.push_back(c.remote);

            push_framings();
            capture_.set_opcode_filter({});
            capture_.set_batch_interval_ms(app_config_.batch_interval_ms);

            if (!capture_.start(cfg)) {
                fail("Capture would not start. Npcap may need Administrator rights — "
                     "close GoBot and run it as administrator.");
                return;
            }
            capture_running_ = true;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                flow_samples_.clear();
                bytes_to_client_ = 0;
                bytes_to_server_ = 0;
                recording_ = true;
            }
            advance(Auto::Recording);
            break;
        }

        case Auto::Recording: {
            std::size_t best = 0;
            std::uint64_t c2s = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [id, buf] : flow_samples_) best = std::max(best, buf.size());
                c2s = bytes_to_server_;
            }

            std::ostringstream d;
            d << best << " bytes recorded, " << c2s << " B sent by the client";
            // Nag while it still matters. Telling someone afterwards that they
            // should have been moving wastes the whole minute they just spent.
            if (elapsed > 6000 && c2s < 1500)
                d << "   <-- MOVE YOUR CHARACTER: almost nothing is being sent";
            auto_detail_ = d.str();

            // Enough data, or long enough waiting.
            if (best >= 200 * 1024 || elapsed >= kRecordMs) {
                if (best < 4096) {
                    fail("Barely any traffic arrived (" + std::to_string(best) + " bytes). "
                         "Either the capture adapter is wrong, or the game was idle. "
                         "Move around in game and try again.");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    recording_ = false;
                }
                advance(Auto::Detecting);
            }
            break;
        }

        case Auto::Detecting: {
            // Detect on the busiest single stream. Framing cannot be found in
            // a mixture of connections or directions, and the busiest stream
            // is server->game state, which is exactly what we want.
            Bytes sample;
            std::string label;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [id, buf] : flow_samples_) {
                    if (buf.size() > sample.size()) {
                        sample = buf;
                        auto it = flow_labels_.find(id);
                        label = (it != flow_labels_.end()) ? it->second : std::string();
                    }
                }
            }

            // Keep the sample. When detection picks something wrong, the bytes
            // are the only way to find out why, and holding them only in
            // memory means every failure is unreproducible.
            if (!sample.empty()) {
                const std::string dump = exe_relative("last_capture.s2c");
                if (std::FILE* f = open_file(dump, "wb")) {
                    std::fwrite(sample.data(), 1, sample.size(), f);
                    std::fclose(f);
                    GB_INFO("Recorded sample written to %s (%zu bytes, stream %s)",
                            dump.c_str(), sample.size(),
                            label.empty() ? "?" : label.c_str());
                }
            }

            detect_results_ = detect_framing(sample.data(), sample.size(), 10);
            detect_ran_ = true;

            for (std::size_t i = 0; i < std::min<std::size_t>(detect_results_.size(), 5); ++i) {
                const auto& r = detect_results_[i];
                GB_INFO("  candidate %zu: %s  cov=%.1f%% frames=%zu opcodes=%zu score=%.0f",
                        i + 1, r.spec.describe().c_str(), r.coverage * 100.0,
                        r.frames_parsed, r.distinct_opcodes, r.score);
            }

            if (detect_results_.empty() || detect_results_.front().coverage < 0.90) {
                fail("Could not work out the packet format from " +
                     std::to_string(sample.size()) + " bytes. The traffic is most "
                     "likely encrypted, in which case packet capture cannot read it.");
                return;
            }

            apply_framing(detect_results_.front().spec);

            std::ostringstream d;
            d << "Framing found: " << detect_results_.front().spec.describe()
              << "  (" << static_cast<int>(detect_results_.front().coverage * 100)
              << "% of the stream, " << detect_results_.front().frames_parsed << " packets)";
            auto_detail_ = d.str();
            GB_INFO("Autopilot: %s", auto_detail_.c_str());

            advance(Auto::Collecting);
            break;
        }

        case Auto::Collecting: {
            std::size_t opcodes = 0, samples = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                opcodes = observations_.size();
                for (const auto& [op, o] : observations_) samples += o.payloads.size();
            }
            auto_detail_ = std::to_string(opcodes) + " opcodes, " +
                           std::to_string(samples) + " packets collected";

            if (elapsed >= kCollectMs) {
                if (opcodes == 0) {
                    fail("The framing was accepted but no packets decoded. "
                         "Try recording again with more movement in game.");
                    return;
                }
                advance(Auto::Analysing);
            }
            break;
        }

        case Auto::Analysing: {
            std::map<std::uint16_t, OpcodeObservations> snapshot;
            std::uint64_t c2s = 0, s2c = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot = observations_;
                c2s = bytes_to_server_;
                s2c = bytes_to_client_;
            }

            suggestions_ = analyze_protocol(snapshot);
            analysis_ran_ = true;

            if (!suggestions_.empty()) {
                auto_detail_ = std::to_string(suggestions_.size()) +
                               " candidate packet meanings found";
                advance(Auto::Done);
                break;
            }

            // Nothing found. Almost always this is a capture of a character
            // standing still, and the traffic ratio says so plainly: an idle
            // client sends a trickle of keepalives, a moving one sends a
            // steady stream of position updates.
            const double ratio = s2c > 0 ? static_cast<double>(c2s) / static_cast<double>(s2c) : 0.0;

            std::ostringstream d;
            d << "No packet meanings identified.  client->server " << c2s
              << " B, server->client " << s2c << " B.";
            auto_detail_ = d.str();

            if (c2s < 4096 || ratio < 0.05) {
                auto_error_ =
                    "The character does not appear to have moved during the capture. "
                    "Meaning is inferred from how values change -- coordinates stepping "
                    "along, HP dropping -- so a recording of a character standing still "
                    "contains nothing to recognise. Run it again and walk around, fight "
                    "something, and take some damage for the whole minute.";
                auto_stage_ = Auto::Failed;
            } else {
                auto_error_ =
                    "There was plenty of traffic, but no field behaved like a coordinate "
                    "or a health bar. The values may be packed in a way this does not "
                    "look for. Use the Opcodes and Fields tabs on the busiest opcode to "
                    "read it by hand.";
                auto_stage_ = Auto::Failed;
            }
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------- actions

void GuiApp::refresh_interfaces() {
    interfaces_ = PacketCapture::list_interfaces();

    const std::string want = app_config_.interface_name.empty()
                           ? PacketCapture::default_interface()
                           : app_config_.interface_name;

    selected_interface_ = -1;
    for (std::size_t i = 0; i < interfaces_.size(); ++i) {
        if (interfaces_[i].name == want) {
            selected_interface_ = static_cast<int>(i);
            break;
        }
    }
}

void GuiApp::find_game_window() {
    windows_ = InputManager::enumerate_windows();
    window_list_loaded_ = true;

    if (input_.find_game_window(window_title_, app_config_.window_class,
                                app_config_.process_name)) {
        set_status("Game window found -- confirm it is the right one on the Capture tab", 6000);
    } else {
        set_status("Game window not found -- pick it manually on the Capture tab", 6000);
    }
}

void GuiApp::start_capture() {
    if (capture_running_) return;

    push_framings();
    capture_.set_batch_interval_ms(app_config_.batch_interval_ms);

    // Forward everything while discovering. A filter here would hide exactly
    // the unmapped opcodes the Protocol panel exists to find.
    capture_.set_opcode_filter({});

    CaptureConfig cfg;
    if (selected_interface_ >= 0 && selected_interface_ < static_cast<int>(interfaces_.size()))
        cfg.interface_name = interfaces_[static_cast<std::size_t>(selected_interface_)].name;
    cfg.server_ip_start = server_ip_;
    cfg.server_ip_end   = server_ip_;
    cfg.bpf_filter      = bpf_filter_;
    if (server_port_[0]) {
        const int p = std::atoi(server_port_);
        if (p > 0 && p < 65536) {
            cfg.port_start = static_cast<std::uint16_t>(p);
            cfg.port_end   = cfg.port_start;
        }
    }

    // Manual start still benefits from whatever the discovery step found.
    for (const auto& c : game_connections_) cfg.known_servers.push_back(c.remote);

    if (capture_.start(cfg)) {
        capture_running_ = true;
        set_status("Capture started");
    } else {
        set_status("Capture failed to start -- run as Administrator?", 8000);
    }
}

void GuiApp::stop_capture() {
    if (!capture_running_) return;
    capture_.stop();
    capture_running_ = false;
    recording_ = false;
    set_status("Capture stopped");
}

void GuiApp::start_bot() {
    if (bot_running_) return;

    if (!framing_verified_) {
        set_status("Framing is not verified -- see the Protocol tab", 6000);
        return;
    }
    if (registry_.size() == 0) {
        set_status("No packet layouts mapped -- see the Protocol tab", 6000);
        return;
    }
    if (!input_.window_valid()) {
        set_status("Game window not found", 6000);
        return;
    }
    if (!capture_running_) {
        set_status("Start capture first", 6000);
        return;
    }
    if (!keys_may_start()) return;

    brain_->update_config(bot_config_);
    executor_->update_config(bot_config_);
    brain_->reanchor_origin();
    brain_->start();
    executor_->set_auto_attack(true);
    bot_running_ = true;
    GB_INFO("=== BOT ENABLED ===");
}

void GuiApp::stop_bot() {
    if (!bot_running_) return;
    if (brain_) brain_->stop();
    if (executor_) {
        executor_->set_auto_attack(false);
        executor_->clear_queue();
    }
    input_.release_all();
    bot_running_ = false;
    GB_INFO("=== BOT DISABLED ===");
}

void GuiApp::toggle_bot() {
    if (bot_running_) stop_bot(); else start_bot();
}

void GuiApp::on_packets(const std::vector<GamePacket>& all) {
    // Frames found inside 0x0081, and frames that came over UDP, are counted
    // and go no further. The tables here key everything by opcode, the same
    // opcode number need not mean the same thing in those places, and the
    // analyzer's suggestions become layouts the bot acts on -- so what the
    // bot sees, and what this screen suggests, stays what it was until plan
    // item P0-3 decides what those frames are. gobot_sniff --watch lists them.
    std::uint64_t held_back = 0;
    for (const auto& p : all)
        if (!p.plain()) ++held_back;
    std::vector<GamePacket> kept;
    if (held_back > 0) {
        kept.reserve(all.size() - held_back);
        for (const auto& p : all)
            if (p.plain()) kept.push_back(p);
    }
    const std::vector<GamePacket>& packets = held_back > 0 ? kept : all;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        held_back_frames_ += held_back;
        for (const auto& p : packets) {
            auto& st = opcode_stats_[p.opcode];
            ++st.count;
            st.last_size = p.payload.size();
            st.min_size = std::min(st.min_size, p.payload.size());
            st.max_size = std::max(st.max_size, p.payload.size());
            if (p.from_server) ++st.to_client; else ++st.to_server;

            auto& q = samples_[p.opcode];
            q.push_back(p.payload);
            while (q.size() > kMaxSamplesPerOpcode) q.pop_front();

            // Observations feed the analyzer. Direction is kept because it is
            // the single most useful signal available: a packet the client
            // sends can only be describing the client.
            auto& obs = observations_[p.opcode];
            if (p.from_server) ++obs.to_client; else ++obs.to_server;
            if (obs.payloads.size() < kMaxSamplesPerOpcode) {
                obs.payloads.push_back(p.payload);
                obs.timestamps_ms.push_back(p.timestamp_ms);
            }
        }
    }
    // Calibration is fed every frame off the TCP stream whatever is mapped,
    // before the protocol layer drops the opcodes it has no layout for -- but
    // not the frames held back above: what it finds is keyed by opcode, as
    // the tables are, and becomes a layout the bot acts on.
    if (calibrator_ && calibrator_->running()) calibrator_->on_packets(packets);

    if (protocol_) protocol_->on_packets(packets);
}

void GuiApp::run_detection() {
    std::vector<std::uint8_t> sample;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sample = raw_sample_;
    }

    if (sample.size() < 2048) {
        set_status("Need more data -- record for longer while playing", 6000);
        return;
    }

    detect_results_ = detect_framing(sample.data(), sample.size(), 10);
    detect_ran_ = true;

    if (detect_results_.empty())
        set_status("No framing found -- the stream may be encrypted", 8000);
    else
        set_status("Detection complete -- review the candidates below");
}

void GuiApp::push_framings() {
    capture_.set_framing(framing_);
    if (has_client_framing_) capture_.set_client_framing(client_framing_);
}

void GuiApp::apply_framing(const FramingSpec& spec) {
    framing_ = spec;
    push_framings();

    // A framing change invalidates everything decoded under the old one.
    std::lock_guard<std::mutex> lock(mutex_);
    opcode_stats_.clear();
    samples_.clear();
    selected_opcode_ = -1;
    set_status("Framing applied -- opcode statistics reset");
}

void GuiApp::save_protocol() {
    const std::string path = exe_relative(app_config_.protocol_path);

    // Preserve unrelated sections; only rewrite Framing and the layouts.
    IniFile ini;
    ini.load(path);

    ini.set_bool("Framing", "verified", framing_verified_);
    ini.set_int ("Framing", "header_size", static_cast<int>(framing_.header_size));
    ini.set_int ("Framing", "length_offset", static_cast<int>(framing_.length_offset));
    ini.set_int ("Framing", "length_size", static_cast<int>(framing_.length_size));
    ini.set     ("Framing", "length_endian", framing_.length_le ? "little" : "big");
    ini.set_bool("Framing", "length_includes_header", framing_.length_includes_header);
    ini.set_int ("Framing", "length_adjust", framing_.length_adjust);
    ini.set_int ("Framing", "opcode_offset", static_cast<int>(framing_.opcode_offset));
    ini.set_int ("Framing", "opcode_size", static_cast<int>(framing_.opcode_size));
    ini.set     ("Framing", "opcode_endian", framing_.opcode_le ? "little" : "big");
    ini.set_int ("Framing", "min_frame", static_cast<int>(framing_.min_frame));
    ini.set_int ("Framing", "max_frame", static_cast<int>(framing_.max_frame));
    if (framing_.has_checksum())
        ini.set_int("Framing", "checksum_offset", static_cast<int>(framing_.checksum_offset));
    if (framing_.has_trailer()) {
        ini.set_int("Framing", "trailer_opcode", static_cast<int>(framing_.trailer_opcode));
        ini.set_int("Framing", "trailer_bytes", static_cast<int>(framing_.trailer_bytes));
        ini.set_int("Framing", "inner_frame_offset", static_cast<int>(framing_.inner_offset));
    }

    for (std::uint16_t op : registry_.mapped_opcodes()) {
        const PacketSpec* s = registry_.find(op);
        if (!s) continue;
        char sec[40];
        std::snprintf(sec, sizeof(sec), "Packet.0x%04X", op);
        ini.set(sec, "name", s->name);
        ini.set(sec, "semantic", to_string(s->semantic));
        ini.set(sec, "fields", OpcodeRegistry::field_list_to_string(s->fields));
        if (!s->repeat_count_field.empty())
            ini.set(sec, "repeat_count_field", s->repeat_count_field);
    }

    if (ini.save(path, "GoBot protocol map"))
        set_status("Saved " + path);
    else
        set_status("FAILED to save " + path, 8000);
}

void GuiApp::save_bot_config() {
    const std::string path = exe_relative(app_config_.bot_config_path);
    if (bot_config_.save(path)) {
        if (executor_) executor_->update_config(bot_config_);
        if (brain_) brain_->update_config(bot_config_);
        set_status("Saved " + path);
    } else {
        set_status("FAILED to save " + path, 8000);
    }
}

void GuiApp::reload_configs() {
    bot_config_.load(exe_relative(app_config_.bot_config_path));
    registry_.load(exe_relative(app_config_.protocol_path));
    if (executor_) executor_->update_config(bot_config_);
    if (brain_) brain_->update_config(bot_config_);

    macro_config_.load(exe_relative("configs/macro.ini"));
    if (macro_) macro_->set_config(macro_config_);
    apply_vitals_config();

    set_status("Configs reloaded");
}

// =========================================================================
//                                  DRAW
// =========================================================================

void GuiApp::draw() {
    // The simple screen is the default, and draws the whole window itself.
    if (simple_view_) { draw_simple_view(); return; }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar;

    ImGui::Begin("GoBot", nullptr, flags);

    draw_menu_bar();

    // The run bar and the character block stay put above the tabs.
    //
    // Which tab is open changes constantly; whether the character is alive
    // does not, and it is the thing being watched. The commercial client puts
    // both above its tab bar for that reason and it is the right call: having
    // to go and find the health reading is having to go and find it at exactly
    // the moment there is no time to.
    draw_run_bar();
    ImGui::Separator();
    draw_player_block();
    ImGui::Separator();

    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Features")) { draw_feature_list();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Map"))      { draw_mapfile_panel(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log"))      { draw_log_panel();     ImGui::EndTabItem(); }

        // Everything that exists to take the game apart rather than to play
        // it, behind one tab. None of it is of any use to somebody running the
        // bot, and all of it used to be on the same row as the controls that
        // are -- nine tabs of protocol archaeology ahead of "Log".
        if (advanced_)
            if (ImGui::BeginTabItem("Tools")) { draw_tools_panel(); ImGui::EndTabItem(); }

        ImGui::EndTabBar();
    }

    if (show_not_built_) draw_not_built_list();

    draw_status_bar();
    ImGui::End();
}

// =========================================================================
//                            THE FEATURE LIST
// =========================================================================

void GuiApp::apply_bot_config() {
    if (brain_)    brain_->update_config(bot_config_);
    if (executor_) executor_->update_config(bot_config_);
}

void GuiApp::apply_macro_config() {
    if (macro_) macro_->set_config(macro_config_);
}

bool GuiApp::draw_feature(const char* name, bool* on, const char* what,
                          const std::function<void()>& settings) {
    ImGui::PushID(name);
    bool changed = false;

    // The switch first, then the name, then what it does -- so a glance down
    // the left edge answers "what is running?" without reading anything.
    //
    // The description sits at a fixed column rather than straight after the
    // name, so the second column lines up down the whole list. Left to follow
    // the name it butted against it, and "Attack" next to "picks a target"
    // read as one word.
    constexpr float kSwitchColumn = 32.0f;
    constexpr float kWhatColumn   = 240.0f;

    const float line = ImGui::GetCursorPosY();
    if (on) changed = ImGui::Checkbox("##on", on);
    else    ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

    ImGui::SameLine(kSwitchColumn);
    ImGui::SetCursorPosY(line);
    const bool open = ImGui::CollapsingHeader(name);

    // Drawn over the header's own bar, which is what makes the two read as one
    // row rather than as a row with a caption under it.
    ImGui::SameLine(kWhatColumn);
    ImGui::SetCursorPosY(line + ImGui::GetStyle().FramePadding.y);
    text_dim("%s", what);

    if (open) {
        ImGui::Indent(14);
        ImGui::Dummy(ImVec2(0, 4));
        if (settings) settings();
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::Unindent(14);
    }
    ImGui::Separator();
    ImGui::PopID();
    return changed;
}

void GuiApp::draw_feature_list() {
    ImGui::Dummy(ImVec2(0, 4));

    // --- what it fights, and how ---
    // Attack drives whichever engine can actually fight.
    //
    // It used to start the packet bot unconditionally. That bot needs your
    // own position and nothing in the protocol map supplies one -- the server
    // does not send it -- so ticking the one control everybody reaches for
    // first either did nothing (no capture running: the tick quietly undid
    // itself) or started a bot ranking monsters by their distance from the
    // map's origin. The engine that works, the name-plate hunter, was three
    // menus down under a heading saying none of it was needed.
    //
    // Now: the packet bot when the protocol map can place the character, the
    // hunter otherwise. When a self-position layout is mapped this switches
    // over by itself, with nothing else to change.
    const bool via_packets = packet_bot_can_see_self();
    const bool engaged = via_packets ? bot_running_ : hunting_;
    bool attacking = engaged;
    draw_feature("Attack", &attacking,
                 via_packets ? "picks a target and fights it -- from packets"
                             : "picks a target and fights it -- by name plates",
                 [this] { draw_attack_panel(); });
    if (attacking != engaged) {
        if (via_packets) { if (attacking) start_bot();  else stop_bot();  }
        else             { if (attacking) start_hunt(); else stop_hunt(); }
    }

    draw_feature("Weapon ranges", nullptr,
                 "how far to stand, from the weapon",
                 [this] { draw_profile_panel(); });

    if (draw_feature("Buffs", &bot_config_.auto_buff,
                     "re-casts each on its own timer",
                     [this] { draw_buff_panel(); }))
        apply_bot_config();

    // Healing is the potion keys, not a flag of its own.
    //
    // It was bound to `vitals_enabled` to begin with, which only says whether
    // the bars are read -- so switching Healing on read the bars and drank
    // nothing. What makes a potion key fire is the key being enabled, so that
    // is what this switch sets.
    {
        bool healing = false;
        for (const auto& key : macro_config_.keys)
            if (key.conditional() && key.enabled) { healing = true; break; }

        if (draw_feature("Healing", &healing,
                         "drinks when HP or MP drops",
                         [this] { draw_heal_panel(); })) {
            for (auto& key : macro_config_.keys)
                if (key.conditional()) key.enabled = healing;
            macro_config_.vitals_enabled = healing || macro_config_.vitals_enabled;
            apply_macro_config();
            if (healing && !vitals_running_) start_vitals();
        }
    }

    // --- where it goes ---
    if (draw_feature("Waypoints", &bot_config_.patrol_use_waypoints,
                     "walks a route instead of standing still",
                     [this] { draw_waypoints_panel(); }))
        apply_bot_config();

    draw_feature("Working area", nullptr,
                 "the box it stays inside",
                 [this] { draw_area_panel(); });

    draw_feature("Monsters", nullptr,
                 "what it will and will not attack",
                 [this] { draw_monsters_panel(); });

    // --- keys ---
    bool macro_on = macro_running_;
    draw_feature("Timed keys", &macro_on,
                 "presses keys on their own schedules",
                 [this] { draw_macro_panel(); });
    if (macro_on != macro_running_) { if (macro_on) start_macro(); else stop_macro(); }

    draw_feature("Game keys", nullptr,
                 "what the game's own settings are set to",
                 [this] { draw_keys_panel(); });

    // --- reading the screen ---
    bool reading = vitals_running_;
    draw_feature("HP / MP reader", &reading,
                 "reads the figures printed on the bars",
                 [this] { draw_vitals_panel(); });
    if (reading != vitals_running_) { if (reading) start_vitals(); else stop_vitals(); }

    // --- other players ---
    {
        bool party_on = party_running_;
        draw_feature("Auto party", &party_on,
                     "answers invitations, and invites whoever asks in chat",
                     [this] { draw_party_panel(); });
        if (party_on != party_running_) { if (party_on) start_party(); else stop_party(); }
    }

    // --- unattended ---
    {
        bool keeping = keeper_running_;
        draw_feature("Keep the client alive", &keeping,
                     "notices a crash and crosses back in",
                     [this] { draw_keeper_panel(); });
        if (keeping != keeper_running_) { if (keeping) start_keeper(); else stop_keeper(); }
    }

    draw_feature("Watch the session", nullptr,
                 "holds everything when you are not in the world",
                 [this] { draw_session_panel(); });

    draw_feature("Click sequences", nullptr,
                 "buying, mining, reviving, closing dialogs",
                 [this] { draw_routines_panel(); });

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::SmallButton("What is not built yet")) show_not_built_ = true;

    // What is doing the picture matching, said plainly. It is linked into this
    // executable rather than shipped as a DLL beside it, so there is nothing in
    // the build folder to tell you by.
    ImGui::SameLine();
    text_dim("   matching: %s", TemplateMatcher::backend().c_str());
}

/// Answering invitations, and sending them.
void GuiApp::draw_party_panel() {
    ImGui::TextWrapped(
        "Two halves. Somebody invites you and the client puts up a small box at "
        "the bottom of the screen for about ten seconds -- long enough for a "
        "person and not for an unattended run. And somebody types the keyword "
        "in chat, which means opening the list of players on the map, finding "
        "their row, and pressing Req Party.");
    ImGui::Dummy(ImVec2(0, 6));

    draw_window_picker_inline();
    ImGui::Dummy(ImVec2(0, 4));

    const std::string path = exe_relative("configs/party.ini");
    Party::Settings rules;
    if (!rules.load(path)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::Text("No %s yet.", path.c_str());
        ImGui::PopStyleColor();
        return;
    }

    bool changed = false;
    bool accept = rules.auto_accept;
    if (ImGui::Checkbox("Accept invitations", &accept)) {
        rules.auto_accept = accept;
        changed = true;
    }
    bool invite = rules.auto_invite;
    if (ImGui::Checkbox("Invite whoever types the keyword", &invite)) {
        rules.auto_invite = invite;
        changed = true;
    }

    char keyword[64]{};
    std::snprintf(keyword, sizeof(keyword), "%s", rules.magic_word.c_str());
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("Keyword", keyword, sizeof(keyword))) {
        rules.magic_word = keyword;
        changed = true;
    }
    ImGui::SameLine();
    text_dim("matched as a whole word, so \"spotted\" is not \"pt\".");

    bool blacklist = rules.use_blacklist;
    if (ImGui::Checkbox("Use the blacklist", &blacklist)) {
        rules.use_blacklist = blacklist;
        changed = true;
    }
    ImGui::SameLine();
    text_dim("%zu name(s). Edit the lists in %s.", rules.blacklist.size(),
             path.c_str());

    // Written key by key, so the measurements and the reasoning written into
    // that file survive somebody ticking a box here.
    if (changed) {
        const bool wrote =
            IniFile::set_in_place(path, "Party", "party_auto_accept",
                                  rules.auto_accept ? "true" : "false") &&
            IniFile::set_in_place(path, "Party", "party_auto_party",
                                  rules.auto_invite ? "true" : "false") &&
            IniFile::set_in_place(path, "Party", "party_magic_word",
                                  rules.magic_word) &&
            IniFile::set_in_place(path, "Party", "use_blacklist",
                                  rules.use_blacklist ? "true" : "false") &&
            IniFile::set_in_place(path, "Chat", "keyword", rules.magic_word);
        if (!wrote) set_status("Could not write " + path, 6000);
        if (party_running_)
            set_status("Changed. Switch Auto party off and on to pick it up.", 6000);
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (party_running_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::Text("Running -- %d accepted, %d invited",
                    party_accepted_.load(), party_invited_.load());
        ImGui::PopStyleColor();
    } else {
        text_dim("Not running.");
    }

    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        lines = party_log_;
    }
    if (!lines.empty() && ImGui::BeginChild("##partylog", ImVec2(0, 120), true)) {
        for (const std::string& line : lines) ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    if (!lines.empty()) ImGui::EndChild();
}

/// Noticing the client has fallen over, and starting it again.
void GuiApp::draw_keeper_panel() {
    ImGui::TextWrapped(
        "The session watcher asks whether the HUD is there, which is the right "
        "question for an interrupted session and the wrong one for a dead "
        "client -- a crash, a loading screen and a death all look the same to "
        "it. This asks the other question: has the client exited, stopped "
        "answering, or put up a message box?");
    ImGui::Dummy(ImVec2(0, 6));

    IniFile ini;
    const std::string path = exe_relative("configs/app.ini");
    ini.load(path);
    CrashWatch::Settings s;
    s.load(ini);

    ImGui::Text("Watching   %s, and %s behind it",
                s.game_process.c_str(), s.launcher_process.c_str());
    ImGui::Text("Gives up   after %d restart(s)", s.max_restarts);
    text_dim("A client that falls over that often is not failing at random, "
             "and a program that keeps reconnecting to a server which is "
             "refusing it is indistinguishable from an attack on it. Change "
             "these in [Crash] in configs/app.ini.");

    ImGui::Dummy(ImVec2(0, 6));

    // With no file, the table that ships, which the keeper writes out.
    AutoLogin::Settings login;
    login.load(exe_relative("configs/login.ini"));
    const bool can_login = login.unidentified().empty();
    if (!can_login) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped(
            "The crossing is not set up, so a crashed client can be noticed but "
            "not restarted. Fill in configs/login.ini first.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));
    }

    std::string state;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        state = keeper_state_;
        lines = keeper_log_;
    }

    if (keeper_running_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::Text("%s  (%d restart(s))", state.c_str(), keeper_restarts_.load());
        ImGui::PopStyleColor();
    } else {
        text_dim("Not watching.");
    }

    if (!lines.empty() && ImGui::BeginChild("##keeperlog", ImVec2(0, 140), true)) {
        for (const std::string& line : lines) ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    if (!lines.empty()) ImGui::EndChild();
}

/// The honest list, in one place, off the main screen.
void GuiApp::draw_not_built_list() {
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Not built yet", &show_not_built_,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "The other helper has these and GoBot does not. They are listed here "
        "rather than shown as switches on the main screen, because a control "
        "that looks live and does nothing costs you an evening finding out.");
    ImGui::Dummy(ImVec2(0, 8));

    const struct { const char* name; const char* note; } kPending[] = {
        { "Fishing",        "the routines exist; the loop around them does not" },
        { "Mining",         "likewise -- and the pickaxe-breaking test is written" },
        { "Crest boxes",    "every picture it needs is extracted and named" },
        { "Party sharing",  "exp and loot sharing, always leading, held slots" },
        { "Items",          "bag monitor, use, drop, sell, shop" },
        { "Party advert",   "a chat line said only while not in a party" },
        { "Pet and avatar",  "pet healing, looting, avatar reset window" },
        { "TDO exchange",   "sixteen items, traded in a priority order" },
        { "Raid boss run",  "travel on a schedule and fight" },
        { "Profiles",       "named config sets, exportable" },
        { "Zalo",           "two-way control, later" },
        { "Quest picking",  "choosing quests by picture, GoC's English-client way" },
    };
    for (const auto& p : kPending) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::Text("%s", p.name);
        ImGui::SameLine();
        text_dim("-- %s", p.note);
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Close", ImVec2(100, 0))) show_not_built_ = false;
    ImGui::End();
}

/// The click sequences, and the dry run that checks them.
void GuiApp::draw_routines_panel() {
    ImGui::TextWrapped(
        "Sequences of clicks through the game's own dialogs, kept in "
        "configs/routines.ini rather than in code so they can be corrected "
        "with the game open in front of you.");
    ImGui::Dummy(ImVec2(0, 6));

    // This file is meant to be edited while the bot runs, and a panel showing
    // what it said at startup would be telling somebody their edit did not
    // take. So it is re-read -- but on the file's timestamp, not every frame.
    //
    // Re-reading each frame did work, at the cost of parsing the file sixty
    // times a second for as long as the panel was open: 2,837 reads, and 2,837
    // log lines, in a single session. Watching the modification time gives the
    // same liveness for one read per actual edit.
    {
        const std::string path = exe_relative("configs/routines.ini");
        const std::uint64_t now = now_ms();
        if (now - routines_checked_ms_ > 250) {
            routines_checked_ms_ = now;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            std::uint64_t stamp = 0;
            if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) {
                stamp = (static_cast<std::uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32)
                      | fad.ftLastWriteTime.dwLowDateTime;
            }
            if (stamp != routines_mtime_ || routines_cache_.empty()) {
                routines_mtime_ = stamp;
                routines_cache_ = Routine::load(path);
            }
        }
    }
    const auto& routines = routines_cache_;
    if (routines.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("No routines loaded.");
        ImGui::PopStyleColor();
        return;
    }

    if (ImGui::BeginTable("##routines", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Routine");
        ImGui::TableSetupColumn("Steps", ImGuiTableColumnFlags_WidthFixed, 280);
        ImGui::TableHeadersRow();

        for (const Routine& r : routines) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            for (const DialogDriver::Step& s : r.steps)
                text_dim("%s -> %s%s", s.find.c_str(),
                         s.gone ? "(gone)" : s.confirm.c_str(),
                         s.optional ? "  [optional]" : "");
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextWrapped(
        "Running one from here is not wired up yet, and would be the wrong "
        "place to start anyway. The step order and click offsets in that file "
        "were worked out from the pictures' names rather than by watching the "
        "game, so they are expected to be wrong somewhere. Check one first "
        "from a console, which looks without clicking:");
    ImGui::Dummy(ImVec2(0, 4));
    text_dim("    gobot_cli --routine %s --dry",
             routines.empty() ? "<name>" : routines.front().name.c_str());
}

// =========================================================================
//                        THE SHAPE THE ORIGINAL USES
// =========================================================================

/// Run, stop, and what the bot is attached to.
void GuiApp::draw_run_bar() {
    const bool running = bot_running_ || macro_running_;

    ImGui::PushStyleColor(ImGuiCol_Button, running ? kRed : kGreen);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    if (ImGui::Button(running ? "STOP" : "START", ImVec2(110, 32))) {
        if (running) {
            if (macro_running_) stop_macro();
            if (bot_running_)   stop_bot();
        } else {
            // The reader comes up with the macro, always.
            //
            // Everything conditional depends on it -- a potion rule has
            // nothing to test without it -- and so does knowing whether the
            // character is still in the world. Leaving it as a separate switch
            // meant the common mistake was starting the macro alone and
            // getting a bot that pressed its attack key and nothing else.
            if (!vitals_running_) start_vitals();
            start_macro();
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Starts the keyboard macro and the HP/MP reader.\n"
                          "The packet-driven bot is started from Tools; it "
                          "needs a decoded protocol, which this one has only "
                          "in part.");

    ImGui::SameLine();
    ImGui::BeginGroup();
    {
        // What it is attached to, and whether input can reach it. Both are
        // preconditions for anything else on screen meaning anything.
        if (!input_.window_valid()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            ImGui::TextUnformatted("no game window");
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("%s", app_config_.window_title.c_str());
            ImGui::SameLine();
            if (input_.window_focused()) text_dim("(in front)");
            else                         text_dim("(behind -- keys will not land)");
        }

        const auto ms = macro_ ? macro_->stats() : MacroEngine::Stats{};
        text_dim("%llu presses", static_cast<unsigned long long>(ms.presses));
    }
    ImGui::EndGroup();

    // The reader, which everything conditional depends on.
    ImGui::SameLine(ImGui::GetWindowWidth() - 230);
    ImGui::BeginGroup();
    if (!vitals_running_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("not reading HP/MP");
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("start reading")) start_vitals();
    } else {
        const auto v = vitals_.values();
        if (v.have_hp()) ImGui::Text("HP %d / %d", v.hp, v.hp_max);
        else             text_dim("HP --");
        if (v.have_mp()) ImGui::Text("MP %d / %d", v.mp, v.mp_max);
        else             text_dim("MP --");
    }
    ImGui::EndGroup();
}

/// The character, and what the bot currently thinks it is doing.
void GuiApp::draw_player_block() {
    const auto v = vitals_.values();

    ImGui::BeginGroup();
    {
        // Bars first, because they are read at a glance and the numbers are
        // read deliberately.
        const struct { const char* name; int value, max; float pct; ImVec4 col; } kBars[] = {
            { "HP", v.hp, v.hp_max, vitals_.hp_percent(), ImVec4(0.85f, 0.25f, 0.25f, 1.0f) },
            { "MP", v.mp, v.mp_max, vitals_.mp_percent(), ImVec4(0.25f, 0.45f, 0.90f, 1.0f) },
        };
        for (const auto& bar : kBars) {
            ImGui::TextUnformatted(bar.name);
            ImGui::SameLine(32);
            if (bar.pct < 0.0f) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted("-- no reading --");
                ImGui::PopStyleColor();
            } else {
                char label[48];
                if (bar.value >= 0 && bar.max > 0)
                    std::snprintf(label, sizeof label, "%d / %d   %.0f%%",
                                  bar.value, bar.max, bar.pct);
                else
                    std::snprintf(label, sizeof label, "%.0f%%", bar.pct);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar.col);
                ImGui::ProgressBar(bar.pct / 100.0f, ImVec2(300, 18), label);
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndGroup();

    ImGui::SameLine(360);
    ImGui::BeginGroup();
    {
        Nameplate me;
        if (detector_.player(me)) {
            ImGui::Text("Position  %d, %d", me.x(), me.y());
        } else {
            text_dim("Position  --");
        }

        ImGui::Text("Map       %s", map_name_[0] ? map_name_ : "unknown");

        if (macro_ && macro_->suspended()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextUnformatted("HOLDING -- not in the world");
            ImGui::PopStyleColor();
        } else if (bot_running_) {
            ImGui::Text("State     %s", brain_ ? to_string(brain_->phase()) : "running");
        } else if (macro_running_) {
            ImGui::TextUnformatted("State     macro running");
        } else {
            text_dim("State     stopped");
        }
    }
    ImGui::EndGroup();

    ImGui::SameLine(660);
    ImGui::BeginGroup();
    draw_nearby_monsters();
    ImGui::EndGroup();
}

void GuiApp::draw_nearby_monsters() {
    const auto plates = detector_.plates();
    std::size_t monsters = 0;
    for (const auto& p : plates) if (!p.is_player) ++monsters;

    ImGui::Text("Nearby (%zu)", monsters);

    Nameplate me;
    const bool have_me = detector_.player(me);

    if (ImGui::BeginTable("##nearby", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingStretchProp,
                          ImVec2(290, 72))) {
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("dist", ImGuiTableColumnFlags_WidthFixed, 52);
        ImGui::TableSetupScrollFreeze(0, 0);

        int shown = 0;
        for (const auto& plate : plates) {
            if (plate.is_player || ++shown > 40) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(plate.name.empty() ? "?" : plate.name.c_str());
            ImGui::TableNextColumn();
            if (have_me) {
                const int dx = plate.x() - me.x(), dy = plate.y() - me.y();
                ImGui::Text("%d", static_cast<int>(std::sqrt(
                    static_cast<double>(dx) * dx + static_cast<double>(dy) * dy)));
            } else {
                ImGui::TextUnformatted("--");
            }
        }
        ImGui::EndTable();
    }
}

// --------------------------------------------------------------- the tabs

/// What the character is, and the ranges that follow from it.
///
/// The original asks for a class and a weapon and then loads one of four
/// presets, because the numbers that matter -- how far to stand, how high and
/// low to reach -- depend on the weapon and on nothing the user can be
/// expected to work out. The four sets of numbers are the game's own, taken
/// from the preset file that ships beside the original, and are reproduced
/// here rather than guessed at.
void GuiApp::draw_profile_panel() {
    ImGui::TextWrapped(
        "Pick the weapon and the attack ranges follow. These four sets of "
        "numbers are the ones the original uses; which one suits is decided by "
        "how far the weapon reaches and how wide a band it covers.");
    ImGui::Dummy(ImVec2(0, 8));

    struct Preset {
        const char* name;
        const char* suits;
        float min_range, max_range, high_y, low_y, lower_thr, higher_thr;
    };
    static const Preset kPresets[] = {
        { "Near - Wide",   "Warrior Blade, Chakram, Archer Cannon (high level)",
          120.0f, 200.0f, 131.0f, 80.0f, 31.0f, 31.0f },
        { "Near - Narrow", "Warrior Sword (low level), Archer Cannon (low level), Claw, Axe",
          120.0f, 200.0f,  63.0f, 63.0f, 31.0f, 31.0f },
        { "Far - Wide",    "Mage",
          150.0f, 350.0f, 150.0f, 63.0f, 63.0f, 63.0f },
        { "Far - Narrow",  "Warrior Sword (high level), Archer Bow, Shuriken",
          150.0f, 350.0f,  63.0f, 63.0f, 31.0f, 31.0f },
    };

    for (const Preset& preset : kPresets) {
        ImGui::PushID(preset.name);
        const bool current =
            std::fabs(bot_config_.min_range     - preset.min_range)  < 0.5f &&
            std::fabs(bot_config_.attack_range  - preset.max_range)  < 0.5f &&
            std::fabs(bot_config_.highest_hit_y - preset.high_y)     < 0.5f &&
            std::fabs(bot_config_.lowest_hit_y  - preset.low_y)      < 0.5f &&
            std::fabs(bot_config_.lower_threshold  - preset.lower_thr)  < 0.5f &&
            std::fabs(bot_config_.higher_threshold - preset.higher_thr) < 0.5f;

        if (current) ImGui::PushStyleColor(ImGuiCol_Button, kGreen);
        if (ImGui::Button(current ? "IN USE" : "Use", ImVec2(70, 0))) {
            bot_config_.min_range        = preset.min_range;
            bot_config_.attack_range     = preset.max_range;
            bot_config_.highest_hit_y    = preset.high_y;
            bot_config_.lowest_hit_y     = preset.low_y;
            bot_config_.lower_threshold  = preset.lower_thr;
            bot_config_.higher_threshold = preset.higher_thr;
            if (brain_) brain_->update_config(bot_config_);
            set_status(std::string("Loaded the ") + preset.name + " ranges");
        }
        if (current) ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(preset.name);
        text_dim("%s", preset.suits);
        text_dim("range %.0f-%.0f, reach %.0f up / %.0f down",
                 preset.min_range, preset.max_range, preset.high_y, preset.low_y);
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, 6));
    }

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextUnformatted("In use now");
    ImGui::DragFloat("Min range",  &bot_config_.min_range, 1.0f, 0.0f, 2000.0f, "%.0f");
    ImGui::DragFloat("Atk range",  &bot_config_.attack_range, 1.0f, 0.0f, 2000.0f, "%.0f");
    ImGui::DragFloat("Highest Y",  &bot_config_.highest_hit_y, 1.0f, 0.0f, 2000.0f, "%.0f");
    ImGui::DragFloat("Lowest Y",   &bot_config_.lowest_hit_y, 1.0f, 0.0f, 2000.0f, "%.0f");
    ImGui::DragFloat("Lower thr",  &bot_config_.lower_threshold, 1.0f, 0.0f, 2000.0f, "%.0f");
    ImGui::DragFloat("Higher thr", &bot_config_.higher_threshold, 1.0f, 0.0f, 2000.0f, "%.0f");

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Save", ImVec2(110, 0))) save_bot_config();
}

/// What it hits, and how it stands in order to hit it. The old Bot and Skills
/// panels put together, which is where the original keeps them.
bool GuiApp::packet_bot_can_see_self() const {
    return !registry_.opcodes_for(Semantic::SelfState).empty() ||
           !registry_.opcodes_for(Semantic::SelfMove).empty();
}

void GuiApp::draw_attack_panel() {
    if (!packet_bot_can_see_self()) {
        ImGui::TextWrapped(
            "Fighting by name plates: monsters are found on screen by their red "
            "health bars, and your character by its name. The packet bot takes "
            "over by itself once the protocol map can say where you are standing "
            "-- the server does not send that, so for now this is the engine "
            "that can fight.");
        ImGui::Dummy(ImVec2(0, 4));

        const Hunter::Status hs = hunter_->status();
        if (hunting_) {
            const bool good = hs.state == Hunter::State::Attacking ||
                              hs.state == Hunter::State::Approaching;
            ImGui::PushStyleColor(ImGuiCol_Text, good ? kGreen : kAmber);
            ImGui::Text("%s", Hunter::to_string(hs.state));
            ImGui::PopStyleColor();
        } else {
            text_dim("not running");
        }
        // Kills and give-ups apart: one is the bot working, the other is it
        // failing to reach something, and only the split says which.
        text_dim("%llu attacks   %llu finished off   %llu given up on (out of reach)",
                 static_cast<unsigned long long>(hs.attacks),
                 static_cast<unsigned long long>(hs.targets_finished),
                 static_cast<unsigned long long>(hs.targets_dropped));

        draw_hunt_settings();

        ImGui::Dummy(ImVec2(0, 6));
        if (ImGui::Button("Save##hunt", ImVec2(110, 0))) save_hunt_config();
        ImGui::SameLine();
        text_dim("The view of what it can see is under View > Advanced panels > Radar.");
        return;
    }

    draw_bot_panel();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    draw_skills_panel();
}

void GuiApp::draw_buff_panel() {
    ImGui::Checkbox("Auto buff", &bot_config_.auto_buff);
    ImGui::Dummy(ImVec2(0, 6));

    if (ImGui::BeginTable("##buffs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("On",    ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Key",   ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Every", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Label");
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < BotConfig::kBuffSlots; ++i) {
            BuffSlot& slot = bot_config_.buffs[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Checkbox("##on", &slot.enabled);

            ImGui::TableNextColumn();
            int vk = static_cast<int>(slot.keycode);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##vk", &vk, 0))
                slot.keycode = static_cast<std::uint32_t>(std::clamp(vk, 0, 255));
            if (slot.keycode)
                ImGui::SetItemTooltip("%s", InputManager::key_name(slot.keycode).c_str());

            ImGui::TableNextColumn();
            int seconds = static_cast<int>(slot.cooldown_sec);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##cd", &seconds, 10))
                slot.cooldown_sec = static_cast<std::uint32_t>(std::max(1, seconds));
            ImGui::SetItemTooltip("Seconds between casts.");

            ImGui::TableNextColumn();
            char label[64] = {};
            std::snprintf(label, sizeof label, "%s", slot.key.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##label", label, sizeof label)) slot.key = label;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Save", ImVec2(110, 0))) save_bot_config();
}

void GuiApp::draw_waypoints_panel() {
    draw_movement_section();
}

void GuiApp::draw_area_panel() {
    ImGui::TextWrapped(
        "The box the bot works inside, in map coordinates. A monster outside it "
        "is left alone, which is what keeps a character from being walked off "
        "the end of a map chasing something.");
    ImGui::Dummy(ImVec2(0, 8));

    ImGui::DragFloat("Min X", &bot_config_.working_min_x, 1.0f, 0.0f, 100000.0f, "%.0f");
    ImGui::DragFloat("Max X", &bot_config_.working_max_x, 1.0f, 0.0f, 100000.0f, "%.0f");
    ImGui::DragFloat("Min Y", &bot_config_.working_min_y, 1.0f, 0.0f, 100000.0f, "%.0f");
    ImGui::DragFloat("Max Y", &bot_config_.working_max_y, 1.0f, 0.0f, 100000.0f, "%.0f");

    ImGui::Dummy(ImVec2(0, 8));
    Nameplate me;
    if (detector_.player(me)) {
        if (ImGui::Button("Centre it on me", ImVec2(160, 0))) {
            const float half_x = (bot_config_.working_max_x - bot_config_.working_min_x) / 2;
            const float half_y = (bot_config_.working_max_y - bot_config_.working_min_y) / 2;
            bot_config_.working_min_x = static_cast<float>(me.x()) - half_x;
            bot_config_.working_max_x = static_cast<float>(me.x()) + half_x;
            bot_config_.working_min_y = static_cast<float>(me.y()) - half_y;
            bot_config_.working_max_y = static_cast<float>(me.y()) + half_y;
        }
        ImGui::SameLine();
        text_dim("you are at %d, %d", me.x(), me.y());
    } else {
        text_dim("Where you are is not known yet, so there is nothing to centre on.");
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Save", ImVec2(110, 0))) save_bot_config();
}

void GuiApp::draw_monsters_panel() {
    ImGui::Checkbox("Ignore flying monsters", &bot_config_.ignore_fly_monster);
    ImGui::Checkbox("Go for bosses first", &bot_config_.priority_boss);
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));

    // What the detector can actually see. This is the honest version of a
    // monster filter: a list of names to include or exclude is only worth
    // having once the names being read are right, and this shows whether
    // they are.
    ImGui::TextUnformatted("What is being seen right now");
    text_dim("Names are read with the game's own font, so they are exact or "
             "they are blank -- never a guess.");
    ImGui::Dummy(ImVec2(0, 6));

    const auto plates = detector_.plates();
    if (ImGui::BeginTable("##seen", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY, ImVec2(0, 320))) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("At",     ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Health", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("What",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();

        for (const auto& plate : plates) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (plate.name.empty()) text_dim("(unreadable)");
            else                    ImGui::TextUnformatted(plate.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d, %d", plate.x(), plate.y());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f%%", static_cast<double>(plate.hp) * 100.0);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(plate.is_player ? "player" : "monster");
        }
        ImGui::EndTable();
    }
}

/// Everything about running without somebody sitting there.
void GuiApp::draw_session_panel() {
    ImGui::TextWrapped(
        "What happens when nobody is watching. The bot cannot tell on its own "
        "that it is pressing keys at nothing -- so it watches for the HUD, "
        "which is there when the character is in the world and is there in no "
        "other state the client shows.");
    ImGui::Dummy(ImVec2(0, 8));

    if (macro_ && macro_->running()) {
        if (macro_->suspended()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            const std::string why = macro_->suspended_reason();
            ImGui::Text("HOLDING -- %s",
                        why.empty() ? "the character is not in the world" : why.c_str());
            ImGui::PopStyleColor();
            text_dim("%llu presses held back.",
                     static_cast<unsigned long long>(macro_->stats().held_suspended));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            ImGui::TextUnformatted("In the world; the macro is free to act.");
            ImGui::PopStyleColor();
        }
    } else if (!vitals_running_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("The HP/MP reader is off, so nothing is being watched.");
        ImGui::PopStyleColor();
        text_dim("The watcher and the reader look at the same thing, so watching "
                 "costs nothing extra -- but it needs the reader on.");
    } else {
        text_dim("The macro is not running.");
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    ImGui::TextUnformatted("Auto-login");
    ImGui::Dummy(ImVec2(0, 4));

    // Re-read each frame rather than cached: this file is edited by hand and
    // by the command line tool, and a panel showing what it said at startup
    // would be telling somebody their edit did not take.
    AutoLogin::Settings login;
    const std::string login_path = exe_relative("configs/login.ini");
    if (!login.load(login_path)) {
        text_dim("No %s yet.", login_path.c_str());
    } else {
        // No account here, and none anywhere: VTCZone keeps itself signed in
        // and the crossing starts at its list of games, as GoC's does.
        text_dim("Starts from VTCZone signed in -- sign in to it once, with its "
                 "remember option. If it shows its sign-in form the crossing "
                 "stops and says so; nothing is typed into it.");
        if (!login.launcher_found()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextWrapped("VTCZone is not at %s -- it is started from there when "
                               "it is not running. Change `launcher` in %s.",
                               login.launcher_path.c_str(), login_path.c_str());
            ImGui::PopStyleColor();
        }

        // Which channel, and which character.
        //
        // Written straight back to configs/login.ini, because that is the one
        // place the crossing reads them from -- the command line tool and this
        // panel must not be able to disagree about which character is yours.
        {
            const AutoLogin::Page* channel_page = nullptr;
            const AutoLogin::Page* char_page = nullptr;
            for (const auto& page : login.pages) {
                if (page.step == AutoLogin::Step::Channel)   channel_page = &page;
                if (page.step == AutoLogin::Step::Character) char_page = &page;
            }

            ImGui::Dummy(ImVec2(0, 6));
            bool changed = false;

            // Count from one here and from zero in the file. "Kênh 1" is what
            // the client calls it, and a person picking a channel should not
            // have to know that the program stores it as a zero.
            // Eight on this server, confirmed. The config is what says so;
            // this fallback only matters if somebody deletes the key.
            const int channels =
                (channel_page != nullptr && channel_page->row_count > 0)
                    ? channel_page->row_count : 8;
            int channel = std::clamp(login.channel_index, 0, channels - 1) + 1;
            ImGui::SetNextItemWidth(130);
            if (ImGui::InputInt("Channel", &channel)) {
                channel = std::clamp(channel, 1, channels);
                login.channel_index = channel - 1;
                changed = true;
            }
            ImGui::SameLine();
            text_dim("1 to %d.", channels);

            int character = std::max(0, login.character_index) + 1;
            ImGui::SetNextItemWidth(130);
            if (ImGui::InputInt("Character", &character)) {
                login.character_index = std::max(1, character) - 1;
                changed = true;
            }
            ImGui::SameLine();
            if (char_page != nullptr && char_page->row_count > 0)
                text_dim("1 to %d.", char_page->row_count);
            else
                text_dim("counting from the one shown first.");

            if (char_page != nullptr && char_page->pick == AutoLogin::Pick::Slot)
                text_dim("Counting from the left of the stage. The character is "
                         "clicked where it stands, then the game is started.");
            else if (char_page != nullptr && char_page->pick == AutoLogin::Pick::Step)
                text_dim("This screen is wound round with an arrow, so this is how "
                         "many presses -- not a position on screen.");

            // Two keys changed in place, not the whole file rewritten.
            //
            // configs/login.ini is written in by hand: it carries the measured
            // geometry of every page and why each number is what it is. Saving
            // the settings object over it would keep the values and throw away
            // all of that, which is a steep price for picking a channel.
            if (changed) {
                const bool wrote =
                    IniFile::set_in_place(login_path, "Login", "channel_index",
                                          login.channel_index) &&
                    IniFile::set_in_place(login_path, "Login", "character_index",
                                          login.character_index);
                if (!wrote) set_status("Could not write " + login_path, 6000);
            }
        }

        const auto missing = login.unidentified();
        if (!missing.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::Text("%zu of the %zu launcher pages have no picture named for "
                        "them, so the crossing cannot run.",
                        missing.size(), login.pages.size());
            ImGui::PopStyleColor();
            for (const AutoLogin::Page* page : missing)
                text_dim("    %s  (%s)", page->key.c_str(),
                         AutoLogin::step_name(page->step));
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextWrapped(
                "To identify one, put the launcher on that page and run "
                "gobot_cli --window VTCZone --tplsrc <picture set> --survey, "
                "then write the name it prints into configs/login.ini. Those "
                "pages belong to the publisher's launcher rather than to the "
                "game, so there is nothing to go on until one is on screen.");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            ImGui::Text("All %zu pages of the crossing have a picture named.",
                        login.pages.size());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    ImGui::TextUnformatted("Stopping");
    text_dim("Set in [Session] in configs/app.ini. Turning the machine off is "
             "off unless written down there, is announced, and can be called "
             "off with  shutdown /a.");
}

/// The keys the game itself is set to, which the bot has to be told.
void GuiApp::draw_keys_panel() {
    ImGui::TextWrapped(
        "These have to match what the game's own settings are set to. The bot "
        "presses them; it cannot read them.");
    ImGui::Dummy(ImVec2(0, 8));

    const struct { const char* label; std::uint32_t* vk; const char* what; } kKeys[] = {
        { "Normal attack", &bot_config_.key_normal_attack, "Used when no skill is ready." },
        { "Loot",          &bot_config_.key_loot,          "Picking items up." },
        { "Jump",          &bot_config_.key_jump,          "Also used for the double jump." },
        { "Fury",          &bot_config_.key_fury,          "The fury skill." },
        { "Left",          &bot_config_.key_left,          nullptr },
        { "Right",         &bot_config_.key_right,         nullptr },
        { "Up",            &bot_config_.key_up,            nullptr },
        { "Down",          &bot_config_.key_down,          nullptr },
        { "Start / stop",  &bot_config_.key_start_stop,    "Works while the game is in front." },
    };

    for (const auto& key : kKeys) {
        ImGui::PushID(key.label);
        int vk = static_cast<int>(*key.vk);
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt(key.label, &vk, 0))
            *key.vk = static_cast<std::uint32_t>(std::clamp(vk, 0, 255));
        ImGui::SameLine();
        text_dim("%s%s%s", InputManager::key_name(*key.vk).c_str(),
                 key.what ? "  --  " : "", key.what ? key.what : "");
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Save", ImVec2(110, 0))) save_bot_config();
}

/// Everything that exists to take the game apart rather than to play it.
void GuiApp::draw_tools_panel() {
    ImGui::TextWrapped(
        "None of this is needed to run the bot. It is the kit that worked out "
        "the game's protocol, its maps and its font, kept because the next "
        "person to need it will need it badly.");
    ImGui::Dummy(ImVec2(0, 8));

    if (ImGui::BeginTabBar("##tools")) {
        if (ImGui::BeginTabItem("Radar"))     { draw_radar_panel();     ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("HP / MP"))   { draw_vitals_panel();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Dashboard")) { draw_dashboard();       ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Capture"))   { draw_capture_panel();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Protocol"))  { draw_protocol_panel();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Calibrate")) { draw_calibrate_panel(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Mapper"))    { draw_mapper_panel();    ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

void GuiApp::draw_menu_bar() {
    if (!ImGui::BeginMenuBar()) return;

    // The way back to the simple screen: first in the bar, and in that
    // screen's language, so the person who needs it is the one who can read it.
    if (text_font_) ImGui::PushFont(text_font_, 0.0f);
    const bool back = ImGui::MenuItem("◄ Giao diện đơn giản");
    if (text_font_) ImGui::PopFont();
    if (back) {
        simple_view_ = true;
        // The full screen may have changed what the tabs show. The login tab
        // reads its file every frame; it only needs a fresh look at VTCZone.
        simple_party_loaded_ = false;
        login_looked_ms_ = 0;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save macro"))       save_macro_config();
        if (ImGui::MenuItem("Reload configs"))   reload_configs();
        if (advanced_) {
            ImGui::Separator();
            if (ImGui::MenuItem("Save bot config")) save_bot_config();
            if (ImGui::MenuItem("Save protocol"))   save_protocol();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) quit_ = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Advanced panels", nullptr, &advanced_)) {
            set_status(advanced_ ? "Advanced panels shown"
                                 : "Advanced panels hidden", 3000);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Packet capture, protocol decoding and the "
                              "pathfinding bot. None of it is needed for the "
                              "macro, and most of it needs Npcap and a decoded "
                              "packet format before it does anything.");
        ImGui::EndMenu();
    }

    if (advanced_) {
        if (ImGui::BeginMenu("Capture")) {
            if (ImGui::MenuItem("Start", nullptr, false, !capture_running_)) start_capture();
            if (ImGui::MenuItem("Stop",  nullptr, false,  capture_running_)) stop_capture();
            if (ImGui::MenuItem("Refresh interfaces")) refresh_interfaces();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Bot")) {
            if (ImGui::MenuItem("Start", nullptr, false, !bot_running_)) start_bot();
            if (ImGui::MenuItem("Stop",  nullptr, false,  bot_running_)) stop_bot();
            ImGui::Separator();
            if (ImGui::MenuItem("Find game window")) find_game_window();
            ImGui::EndMenu();
        }
    }

    // Live indicators, right-aligned-ish.
    ImGui::Separator();
    status_dot(input_.window_valid(), "window", "no window");
    ImGui::Separator();
    status_dot(macro_running_, "MACRO ON", "macro off");

    if (macro_config_.needs_vitals()) {
        ImGui::Separator();
        float hp, mp;
        macro_->current_vitals(hp, mp);
        if (hp < 0.0f && mp < 0.0f) {
            status_dot(false, "", "HP/MP not reading");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, hp >= 0.0f && hp < 25.0f ? kRed : kGreen);
            if (hp >= 0.0f) ImGui::Text("HP %.0f%%", hp);
            if (mp >= 0.0f) { ImGui::SameLine(); ImGui::Text("MP %.0f%%", mp); }
            ImGui::PopStyleColor();
        }
    }

    if (advanced_) {
        ImGui::Separator();
        status_dot(capture_running_, "capture", "capture off");
        ImGui::Separator();
        status_dot(bot_running_, "BOT ON", "bot off");
    }

    ImGui::EndMenuBar();
}

// ------------------------------------------------------------- dashboard

bool GuiApp::focus_game_to_start(const char* what) {
    if (input_.window_focused()) return true;
    if (input_.focus_window()) return true;

    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "Could not bring the game to the front -- click it, %s is "
                  "already running", what);
    set_status(msg, 7000);
    GB_WARN("%s started but the game window refused focus", what);
    return false;
}

void GuiApp::start_macro() {
    if (macro_running_) return;
    if (!input_.window_valid()) {
        set_status("Pick the game window first -- the list is at the top of this tab", 6000);
        return;
    }
    if (!keys_may_start()) return;
    macro_->set_config(macro_config_);

    // Conditional keys are inert without a reading, so bring the reader up
    // with the macro rather than making it a separate thing to remember.
    if (macro_config_.needs_vitals() && !vitals_running_) start_vitals();

    // Put the game in front, so pressing the button is the whole of switching
    // the macro on.
    if (macro_config_.focus_on_start) focus_game_to_start("the macro");

    macro_->start();
    macro_running_ = true;
}

void GuiApp::stop_macro() {
    if (!macro_running_) return;
    macro_->stop();
    macro_running_ = false;
}

void GuiApp::toggle_macro() {
    if (macro_running_) stop_macro(); else start_macro();
}

void GuiApp::save_macro_config() {
    const std::string path = exe_relative("configs/macro.ini");
    if (macro_config_.save(path)) {
        macro_->set_config(macro_config_);
        apply_vitals_config();
        set_status("Saved " + path);
    } else {
        set_status("FAILED to save " + path, 8000);
    }
}

// ----------------------------------------------------------------- vitals

void GuiApp::apply_vitals_config() {
    vitals_.set_hp_bar(macro_config_.hp_bar);
    vitals_.set_mp_bar(macro_config_.mp_bar);
    vitals_.set_target_window(input_.target_window());
}

void GuiApp::start_vitals() {
    if (vitals_running_) return;
    if (!input_.window_valid()) {
        set_status("Pick the game window first", 6000);
        return;
    }
    apply_vitals_config();
    vitals_.start();
    vitals_running_ = true;
    macro_config_.vitals_enabled = true;      // remembered across restarts
}

void GuiApp::stop_vitals() {
    if (!vitals_running_) return;
    vitals_.stop();
    vitals_running_ = false;
    macro_config_.vitals_enabled = false;

    // Drop the engine's reading immediately. Without this, a conditional key
    // would keep acting on the last value for another second.
    if (macro_) macro_->set_vitals(-1.0f, -1.0f);
}

// ------------------------------------------------------------------ parties

void GuiApp::say_party(const std::string& line) {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    party_log_.push_back(line);
    if (party_log_.size() > 200) party_log_.erase(party_log_.begin());
}

void GuiApp::say_keeper(const std::string& line) {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    keeper_log_.push_back(line);
    if (keeper_log_.size() > 200) keeper_log_.erase(keeper_log_.begin());
}

void GuiApp::say_keeper_vi(const std::string& now, const std::string& problem) {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    keeper_vi_ = now;
    if (!problem.empty()) keeper_problem_vi_ = problem;
}

void GuiApp::start_party() {
    if (party_running_) return;
    if (!input_.window_valid()) {
        set_status("Pick the game window first", 6000);
        return;
    }
    // A loop that ended by itself leaves its thread to be joined. Assigning
    // over a thread that still wants joining ends the program.
    if (party_thread_.joinable()) party_thread_.join();
    party_stop_ = false;
    party_running_ = true;
    party_accepted_ = 0;
    party_invited_ = 0;
    party_thread_ = std::thread([this] { party_loop(); });
}

void GuiApp::stop_party() {
    // Joined even when it is no longer running: a loop that stopped by itself
    // still has a thread, and destroying that unjoined ends the program.
    party_stop_ = true;
    if (party_thread_.joinable()) party_thread_.join();
    party_running_ = false;
}

/// Both halves of auto-party, on one thread.
///
/// Answering invitations and sending them share a capture and a set of rules,
/// and both are slow enough to stutter the window if they ran on the drawing
/// thread: an invitation costs several seconds of clicking and waiting for the
/// client to catch up.
void GuiApp::party_loop() {
    const std::string config = exe_relative("configs/party.ini");

    // Never on the code's own defaults. A file that cannot be read says
    // nothing about what the player ticked, and a worker that ran anyway
    // answered and sent invitations by rules nobody chose. Tried a few times
    // first: the tab writes the file as a box is ticked, and something may
    // hold it for a moment just then -- a virus scanner, say.
    Party::Settings rules;
    bool read = rules.load(config);
    for (int i = 0; !read && i < 5 && !party_stop_; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        read = rules.load(config);
    }
    if (!read) {
        GB_WARN("Party: cannot read %s -- not started, rather than run on rules nobody chose",
                config.c_str());
        say_party("Không đọc được configs/party.ini nên Tổ đội không chạy. Kiểm tra file đó "
                  "rồi bấm BẮT ĐẦU lại.");
        party_running_ = false;
        return;
    }

    IniFile ini;
    ini.load(config);

    PartyInvite::Settings accept_geometry;
    accept_geometry.load(ini);
    PlayerList::Settings list_geometry;
    list_geometry.load(ini);

    PartyChat::Settings chat_rules;
    chat_rules.keyword     = ini.get("Chat", "keyword", rules.magic_word);
    chat_rules.votes       = std::max(1, ini.get_int("Chat", "votes", 3));
    chat_rules.vote_gap_ms = std::max(50, ini.get_int("Chat", "vote_gap_ms", 150));
    chat_rules.cooldown_s  = std::max(1, ini.get_int("Chat", "cooldown_s", 30));
    chat_rules.min_name_match = ini.get_float("Chat", "min_name_match", 0.7f);
    chat_rules.whole_word_only = ini.get_bool("Chat", "whole_word_only", true);

    // Where the chat lines are: placed from the chat bar on every look, so
    // they are found on a 1280x720 client as on a 1024x768 one.
    ChatLines::Settings chat_place;
    chat_place.load(ini);
    ChatLines chat_lines;
    chat_lines.set_settings(chat_place);

    TemplateStore store;
    if (!store.load_best(exe_relative("configs/templates.ini"))) {
        say_party("No UI pictures, so nothing can be recognised.");
        party_running_ = false;
        return;
    }

    GlyphFont font;
    const bool have_font = font.load(GlyphFont::default_dir());
    if (!have_font)
        say_party("No bitmap font: names cannot be read, so only invitations "
                  "that need no name will be answered.");

    ScreenCapture capture;
    PartyInvite answering;
    answering.set_settings(accept_geometry);
    PartyChat chat;
    chat.set_settings(chat_rules);
    PlayerList list;
    list.set_settings(list_geometry);

    say_party("watching");
    std::string answered;
    std::string refused;            // the dialog last left alone, and why
    std::uint64_t rules_read_ms = now_ms();

    while (!party_stop_) {
        // Pick up changes made while it runs. The simple screen writes the
        // file as boxes are ticked and has no "restart it" to ask for; a rule
        // that only took effect next time would look like a rule ignored.
        if (now_ms() - rules_read_ms > 2000) {
            rules_read_ms = now_ms();
            Party::Settings fresh;
            if (fresh.load(config)) rules = fresh;
            IniFile fresh_ini;
            if (fresh_ini.load(config)) {
                const std::string keyword = fresh_ini.get("Chat", "keyword", rules.magic_word);
                if (keyword != chat_rules.keyword) {
                    chat_rules.keyword = keyword;
                    chat.set_settings(chat_rules);
                }
            }
        }

        // Both halves switched off: nothing to look at, so do not look. And
        // nothing clicked while a trash pass has the bag open: an invitation
        // answered in the middle of a drag drops the item wherever it is.
        if ((!rules.auto_accept && !rules.auto_invite) || trash_pass_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // One look this pass, and what was so of it as it was grabbed: whether
        // the game was all in view is asked right before the grab and right
        // after it, not when the look is read -- a second or more later, an
        // invitation answered meanwhile (simple_party.cpp, take_party_look).
        PartyLook look;
        if (!input_.window_valid() ||
            !take_party_look(party_sight(capture, input_.target_window()), look)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        const Frame& frame = look.frame;
        // "Tổ đội: …" on the tab, every few seconds (simple_party.cpp).
        party_hud_tick(look, store);

        // An invitation on screen may be answered by the player's own hand,
        // and the party change a moment later: "alone" waits for the game to
        // draw it, and one gone without GoBot's answer may have been accepted
        // that way -- a party joined, which the game may take a while to draw
        // (PartyEvents::invitation). Looked for on every pass whenever the
        // tick's invite could go from alone, answered or not. The footer
        // picture is the two buttons every dialog has: the trash bin's own
        // dialogs, told apart by bag::bin_asking, are no invitation; any other
        // dialog with them is taken for one, the safe way round.
        PartyInvite::Seen seen;
        bool bin = false;
        if (rules.auto_accept || (rules.auto_invite && rules.always_leader)) {
            seen = answering.look(frame, store, rules.auto_accept && have_font ? &font : nullptr);
            bin = seen.up && bag::bin_asking(frame, store);
            party_events().invitation(look.grabbed_ms, seen.up && !bin);
        }

        // ---- somebody is asking us ----
        if (rules.auto_accept) {
            if (!seen.up) {
                answered.clear();
                refused.clear();
            } else if (seen.who.empty() || seen.who != answered) {
                const bool name_matters =
                    !rules.accept_all ||
                    (rules.use_blacklist && !rules.blacklist.empty());

                // The footer picture is the two buttons every dialog has: it
                // matches the trash bin's quantity dialog at 0.90, and the drop
                // and revive questions at 0.86. So never one of the bin's
                // dialogs -- Xác Nhận there deletes an item, its count unchecked
                // -- and never a dialog whose line does not read as an
                // invitation's, "Yêu cầu của <name>.", whatever accept_all says.
                std::string why_not;
                if (bin)
                    why_not = "it is the trash bin's own dialog";
                else if (seen.who.empty())
                    why_not = have_font ? "its line does not read as an invitation's"
                                        : "there is no font to read its line with";

                // Answered with the mouse to itself, and only if, after the
                // pause, the same invitation is still there and still not the
                // bin's: the screen a second later is not this one.
                const auto answer = [&](bool accept) {
                    std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::try_to_lock);
                    if (!mouse.owns_lock() || trash_pass_) return false;
                    if (!input_.window_focused()) {
                        input_.focus_window();
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    if (accept)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(accept_geometry.settle_ms));
                    Frame now;
                    if (!capture.grab(input_.target_window(), now) || !now.valid()) return false;
                    const auto again = answering.look(now, store, have_font ? &font : nullptr);
                    if (!again.up || again.who.empty() || bag::bin_asking(now, store) ||
                        std::abs(again.at.x - seen.at.x) > 3 || std::abs(again.at.y - seen.at.y) > 3)
                        return false;
                    return accept ? input_.click_at(again.accept_x, again.accept_y)
                                  : input_.click_at(again.decline_x, again.decline_y);
                };

                if (!why_not.empty()) {
                    // Said once for each dialog, not every look.
                    const std::string key = std::to_string(seen.at.x) + "," +
                                            std::to_string(seen.at.y) + why_not;
                    if (key != refused)
                        say_party("a dialog with the invitation's buttons, not answered: " + why_not);
                    refused = key;
                } else if (name_matters && !rules.may_accept(seen.who)) {
                    if (answer(false)) {
                        // This pass's look is from before it: no invite goes
                        // on it (read_party, PartyHud::stale). And the
                        // invitation's going is GoBot's own doing.
                        const std::uint64_t at = now_ms();
                        party_events().answered(at);
                        party_events().changed(at, "an invitation was declined", party_text::kEventDeclined);
                        say_party("declined " + seen.who);
                        answered = seen.who;
                    }
                } else if (answer(true)) {
                    // A member of somebody else's party from now on, though
                    // this pass's look still shows the player alone: no invite
                    // goes on it, and none from "alone" until the party has
                    // been drawn -- a member then -- or kJoinWaitMs have gone
                    // by without it (PartyEvents::joined).
                    const std::uint64_t at = now_ms();
                    party_events().answered(at);
                    party_events().joined(at, "an invitation was accepted", party_text::kEventAccepted);
                    say_party("accepted " + seen.who);
                    answered = seen.who;
                    ++party_accepted_;
                    alert_party(true, seen.who);
                }
            }
        }

        // ---- somebody is asking in chat ----
        if (rules.auto_invite && have_font) {
            std::vector<std::string> lines;
            for (const ScreenRect& box : chat_lines.boxes(frame, store)) {
                const std::string text = font.read_line(frame, box);
                if (!text.empty()) lines.push_back(text);
            }

            // Tự chat's own lines, read back off the chat, are not somebody
            // asking: an advert with the keyword in it would otherwise have
            // the player try to invite himself after every one.
            const std::vector<std::string> ours = recently_said();
            if (!ours.empty())
                lines.erase(std::remove_if(lines.begin(), lines.end(),
                                           [&](const std::string& l) {
                                               for (const std::string& s : ours)
                                                   if (PartyChat::echoes(l, s)) return true;
                                               return false;
                                           }),
                            lines.end());

            const std::uint64_t now = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());

            for (const std::string& who : chat.saw(lines, now)) {
                if (party_stop_) break;
                if (!rules.may_invite(who)) {
                    say_party(who + " asked, and the rules say no");
                    continue;
                }
                // A reading used for an invite comes from a look grabbed after
                // the last change to the party: an invitation answered, or an
                // invite sent, earlier in this pass leaves this pass's look
                // from before it -- so a new one is taken. (A look from before
                // a change is never used anyway: read_party marks it stale.)
                if (party_events().last_change().at_ms >= look.grabbed_ms) {
                    PartyLook fresh;
                    if (!input_.window_valid() ||
                        !take_party_look(party_sight(capture, input_.target_window()), fresh)) {
                        say_party(who + " asked; no new look at the game after the last change to the party, "
                                        "not invited");
                        continue;
                    }
                    look = std::move(fresh);
                }
                // Only when an invite can work, by the party HUD on that
                // look, and nothing else holds the game (simple_party.cpp).
                if (!party_may_invite(who, look, store, rules.always_leader)) continue;

                // The mouse to itself for the whole of it -- opening the list,
                // finding the row, the invite -- so a trash pass that starts
                // meanwhile waits for it rather than having its drags clicked
                // through.
                std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::try_to_lock);
                if (!mouse.owns_lock() || trash_pass_) {
                    say_party(who + " asked while the trash cleaner had the mouse; not invited");
                    continue;
                }

                std::string why;
                if (!list.ensure_open(input_, capture, store, &why)) {
                    say_party(who + " asked, but the list would not open: " + why);
                    continue;
                }
                Frame open_now;
                if (!capture.grab(input_.target_window(), open_now) ||
                    !open_now.valid())
                    continue;

                const auto rows = list.read(open_now, font);
                const int at = list.find(rows, who);
                if (at < 0) {
                    say_party(who + " asked, but is not among the " +
                              std::to_string(rows.size()) + " on this map");
                    continue;
                }
                const PlayerList::Row* row = nullptr;
                for (const auto& r : rows) if (r.index == at) row = &r;
                if (row == nullptr) continue;

                // Whatever it says, the row and perhaps Req Party were
                // clicked: the party may change from here, and the next asker
                // of this pass is not invited on this pass's look.
                const bool sent = list.invite(input_, capture, store, *row, &why);
                party_events().changed(now_ms(), "an invite was sent", party_text::kEventInvited);
                if (!sent) {
                    say_party("invite to " + who + " did not go: " + why);
                    continue;
                }
                say_party("invited " + who);
                ++party_invited_;
                alert_party(false, who);
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(chat_rules.vote_gap_ms));
    }

    say_party("stopped");
    party_running_ = false;
}

// -------------------------------------------------------- keeping it alive

void GuiApp::start_keeper(bool oneshot) {
    // A keeper started for "Vào game ngay" alone becomes the run's own.
    if (keeper_running_) {
        if (!oneshot) keeper_oneshot_ = false;
        return;
    }
    if (keeper_thread_.joinable()) keeper_thread_.join();
    keeper_ui_.reset();                         // the watchdog starts afresh
    keeper_oneshot_ = oneshot;
    if (!oneshot) keeper_login_now_ = false;    // a key's request the last keeper never took
    keeper_stop_ = false;
    keeper_running_ = true;
    keeper_restarts_ = 0;
    {
        // A new start, so whatever stopped the last one is old news.
        std::lock_guard<std::mutex> lock(worker_mutex_);
        keeper_vi_ = "Đang theo dõi game.";
        keeper_problem_vi_.clear();
    }
    keeper_thread_ = std::thread([this] { keeper_loop(); });
}

void GuiApp::stop_keeper() {
    // A cold start waiting on this keeper has nothing left to wait for. Every
    // way of stopping it comes through here -- the panic key, DỪNG LẠI,
    // unticking "Tự vào lại game", the full screen -- and none of them should
    // be followed by the ticked jobs starting once the game is up.
    simple_cold_start_ = false;
    keeper_cold_start_ = false;
    keeper_stop_ = true;
    {
        // A login in progress takes minutes to finish by itself.
        std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
        if (keeper_flow_) keeper_flow_->cancel();
    }
    if (keeper_thread_.joinable()) keeper_thread_.join();
    keeper_running_ = false;
}

/// Notice the client has fallen over, end it, and cross back in.
void GuiApp::keeper_loop() {
    // Its own lines go to gobot.log as well as to the panel. The crossing's
    // steps are logged by the crossing itself, so those only go to the panel.
    const auto note = [this](const std::string& line) {
        GB_INFO("Keeper: %s", line.c_str());
        say_keeper(line);
    };
    // Stopping for a person: in English for the log and the panel, and in
    // Vietnamese for the simple screen, where it stays after the keeper has
    // gone so the reason is still there to read.
    const auto give_up = [this](const std::string& english, const std::string& vi) {
        GB_WARN("Keeper: %s", english.c_str());
        say_keeper(english);
        say_keeper_vi(vi, vi);
    };

    IniFile app_ini;
    app_ini.load(exe_relative("configs/app.ini"));
    CrashWatch::Settings watch_settings;
    watch_settings.load(app_ini);

    // With no file, the table that ships: written out, as the command line
    // does, so there is a file to change. Only when there is truly no file,
    // though. One that is there and will not open -- a virus scan or a sync
    // holding it for a moment -- is the player's, measurements and notes and
    // all: the table is crossed with instead, and the file is left alone.
    AutoLogin::Settings login;
    const std::string login_path = exe_relative("configs/login.ini");
    if (!login.load(login_path)) {
        if (!AutoLogin::Settings::file_missing(login_path))
            note("could not read " + login_path + " -- crossing with the pages that ship, "
                 "and leaving the file as it is");
        else if (login.save(login_path))
            note("wrote " + login_path + " with the pages that ship");
    }
    if (!login.game_process.empty())
        watch_settings.game_process = login.game_process;
    if (!login.client_process.empty())
        watch_settings.launcher_process = login.client_process;

    // The same pictures, from the same place, as the command line's crossing.
    // Reading them only out of the other helper's assembly meant a package --
    // which does not carry that file -- had nothing to match any page against.
    // Only the crossing's own are kept: this runs for hours, and the whole set
    // is some sixty megabytes of which it needs seven pictures. Loaded again
    // only if the file comes to name others; see below.
    const auto load_store = [](const AutoLogin::Settings& for_these) {
        TemplateStore all;
        AutoLogin::load_pictures(all, exe_relative("configs/templates.ini"));
        return AutoLogin::pictures_for(for_these, all);
    };
    TemplateStore store = load_store(login);
    std::vector<std::string> kept = AutoLogin::picture_names(login);

    CrashWatch watch(watch_settings);
    note("watching");
    bool seen_running = false;      // the client has been up since this started
    bool crossed_in = false;        // this keeper has logged in at least once
    bool said_found = false;        // the cold start's game was found up, and said so

    while (!keeper_stop_) {
        CrashWatch::Look seen = watch.look();
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            keeper_state_ = seen.bad() ? CrashWatch::trouble_name(seen.what)
                                       : "the client is running and answering";
        }

        CrashWatch::Move move =
            CrashWatch::next_move(seen, seen_running, keeper_cold_start_.load());
        // "Vào game ngay", and the watchdog for a game that runs with nobody
        // in the world: see simple_keeper.cpp.
        const KeeperWake wake = keeper_wake(move, watch, watch_settings, login, store, seen);
        if (wake == KeeperWake::Quit) break;
        if (wake == KeeperWake::Stalled) move = CrashWatch::Move::Restart;
        if (move == CrashWatch::Move::Watch || move == CrashWatch::Move::Follow) {
            seen_running = true;
            // BẮT ĐẦU was pressed with the game closed, and what was ticked
            // has not started yet. When the game is up without this keeper
            // having crossed in -- opened by hand, or by another tool, while
            // the pictures loaded -- there is nothing to cross: it is
            // followed, and tick() starts what was ticked, as BẮT ĐẦU does
            // with the game open. Asked again on every look until that has
            // happened, in case its window is not found the first time.
            if (move == CrashWatch::Move::Follow) {
                if (!crossed_in && !said_found) {
                    note("the game is already running -- following it and starting the "
                         "ticked jobs");
                    say_keeper_vi("Game đã mở sẵn. Đang theo dõi game.");
                    said_found = true;
                }
                keeper_reattach_ = true;
            }
            for (int i = 0; i < 6 && !keeper_stop_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        note(seen.detail);
        if (!seen.code.empty()) note("error code " + seen.code);

        // Never up since this started -- BẮT ĐẦU with the game closed. That is
        // a start, not a restart, the way GoC treats a missing window: there
        // is no client to end and no session for the server to see close, so
        // neither the shut-down nor the cool-down, and a game launcher already
        // open is left for the crossing to carry on from. It is one crossing
        // either way, and one that fails stops this.
        const bool cold = move == CrashWatch::Move::Start;
        if (!cold)
            alert("disconnect", alert_text::disconnect(seen.what, seen.code,
                                                       keeper_restarts_ < watch_settings.max_restarts &&
                                                       elevated_ && login.unidentified().empty(),
                                                       seen.minutes));

        if (!cold && keeper_restarts_ >= watch_settings.max_restarts) {
            const std::string n = std::to_string(keeper_restarts_.load());
            give_up("that is " + n + " restarts and it has gone again -- stopping, a person "
                    "is needed",
                    "Game văng " + n + " lần liền — GoBot ngừng tự vào lại. Cần người kiểm tra.");
            alert("keeper_gave_up", alert_text::gave_up_restarts(keeper_restarts_));
            break;
        }

        if (!cold) {
            say_keeper_vi(CrashWatch::trouble_vi(seen) + " — GoBot đóng game rồi vào lại sau " +
                          std::to_string(watch_settings.cooldown_s) + " giây.");
            watch.shut_down(seen);
            for (int i = 0; i < watch_settings.cooldown_s * 2 && !keeper_stop_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (keeper_stop_) break;
        }

        // Read again for every crossing. A channel or a character picked on
        // the tab while this runs is meant for the next one -- the tab says
        // GoBot goes to the one shown there -- and a file that will not read
        // just now leaves the last copy that did.
        {
            AutoLogin::Settings fresh;
            if (fresh.load(login_path)) login = fresh;
        }
        // A page given another picture since then is a picture not kept.
        if (AutoLogin::picture_names(login) != kept) {
            note("configs/login.ini names other pictures now -- loading them");
            store = load_store(login);
            kept = AutoLogin::picture_names(login);
        }

        if (!login.unidentified().empty()) {
            std::string pages;
            for (const AutoLogin::Page* page : login.unidentified())
                pages += (pages.empty() ? "" : ", ") + page->key;
            give_up("the client is down and the crossing is not set up, so it cannot be "
                    "started again -- see configs/login.ini",
                    AutoLogin::advice(AutoLogin::Why::NotSetUp, pages));
            alert("keeper_gave_up", alert_text::gave_up_not_set_up());
            break;
        }

        // The game and VTCZone run as administrator, and Windows drops clicks
        // sent to them from a program that does not.
        if (!elevated_) {
            give_up("GoBot is not running as administrator, so its clicks cannot reach "
                    "VTCZone or the game",
                    "GoBot không chạy bằng quyền Administrator nên không bấm được vào "
                    "VTCZone/game.");
            alert("keeper_gave_up", alert_text::gave_up_not_elevated());
            break;
        }

        if (cold) {
            note("starting the game -- it was not running when this began");
        } else {
            ++keeper_restarts_;
            note("logging back in (" + std::to_string(keeper_restarts_.load()) + " of " +
                 std::to_string(watch_settings.max_restarts) + ")");
        }

        AutoLogin flow;
        flow.set_settings(login);
        flow.set_templates(&store);
        // A game that comes up part way and stays on a screen the crossing
        // does not know is ended the way a crashed one is, and the next
        // attempt starts from VTCZone.
        flow.set_end_client([&watch] { return watch.shut_down(CrashWatch::Look{}); },
                            watch_settings.cooldown_s);
        const std::string doing = cold ? "Đang mở game và vào nhân vật: " : "Đang vào lại game: ";
        flow.on_step([this, doing, &flow](AutoLogin::Step s, const std::string& detail) {
            say_keeper(std::string(AutoLogin::step_name(s)) + ": " + detail);
            if (s == AutoLogin::Step::Blocked || s == AutoLogin::Step::InWorld) return;
            std::string vi = doing + AutoLogin::step_name_vi(s) + "…";
            // Waiting on a window in the way, not stuck: said while it waits,
            // so the player at the screen knows what to move.
            if (flow.holding())
                vi += " " + (flow.hiding() ? AutoLogin::hidden_advice(flow.held_by())
                                           : AutoLogin::advice(AutoLogin::Why::Covered,
                                                               flow.held_by()));
            say_keeper_vi(vi);
        });
        {
            std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
            keeper_flow_ = &flow;
        }
        const bool in = !keeper_stop_ && flow.run();
        {
            std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
            keeper_flow_ = nullptr;
        }
        if (keeper_stop_) break;
        if (!in) {
            // Stop and ask, as before: the crossing has already used its
            // attempts, and the next try is for a person to decide.
            give_up("could not get back in: " + flow.trouble(), flow.advice());
            alert("keeper_gave_up", alert_text::gave_up_crossing(flow.attempts_made(), flow.why()));
            break;
        }
        seen_running = true;
        crossed_in = true;
        note("back in the world");
        if (!cold) alert("back_in", alert_text::back_in(keeper_restarts_, watch_settings.max_restarts));
        say_keeper_vi(cold ? "Đã vào game. Đang theo dõi game."
                           : "Đã vào lại game. Đang theo dõi game.");
        keeper_reattach_ = true;
        keeper_stall_restart();
    }

    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        keeper_state_ = "not watching";
    }
    say_keeper("stopped");
    keeper_running_ = false;
}

/// Point everything at the game window again, after the keeper logged back in
/// -- or found the game up already, for a cold start.
///
/// The client it started is a new process with a new window. Without this the
/// reader goes on reading the window that closed, the macro holds for want of
/// a reading, the hunter looks at nothing -- and the run that the login was
/// meant to save stops anyway, just later.
bool GuiApp::reattach_game() {
    windows_ = InputManager::enumerate_windows();
    window_list_loaded_ = true;
    if (!input_.find_game_window(app_config_.window_title, app_config_.window_class,
                                 app_config_.process_name)) {
        // Said once: a cold start waiting on the window asks every few seconds.
        if (!reattach_missed_)
            GB_WARN("The game is up, but its window could not be found -- looking again");
        reattach_missed_ = true;
        return false;
    }
    reattach_missed_ = false;
    apply_vitals_config();
    if (detector_running_) detector_.set_target_window(input_.target_window());
    GB_INFO("Following the game window the keeper found");
    set_status("Following the game window", 6000);
    return true;
}

// ----------------------------------------------------------------- reviving

void GuiApp::say_revive(const std::string& line) {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    revive_log_.push_back(line);
    if (revive_log_.size() > 200) revive_log_.erase(revive_log_.begin());
}

void GuiApp::start_revive() {
    if (revive_running_) return;
    if (revive_thread_.joinable()) revive_thread_.join();
    revive_stop_ = false;
    revive_running_ = true;
    revive_thread_ = std::thread([this] { revive_loop(); });
}

void GuiApp::stop_revive() {
    revive_stop_ = true;
    if (revive_thread_.joinable()) revive_thread_.join();
    revive_running_ = false;
}

/// Watch for the death panel and revive on the spot, with a revive item.
///
/// What to click, and when, is the Reviver's: the panel's "Sử dụng vật phẩm
/// hồi sinh", then Xác Nhận on the question, three tries for each death, never
/// the village or Cash, and nothing while one of the trash bin's dialogs is up.
/// This runs it a little over once a second, and holds every other part of
/// the bot off from the first click at a death until the character is up
/// again: potion keys pressed at the death panel do nothing, and the hunter
/// walking would be worse. When the tries run out the player's rule is that
/// the bot stops; tick() does that, since stopping joins this thread.
void GuiApp::revive_loop() {
    const auto tell = [this](const std::string& line) {
        GB_INFO("Revive: %s", line.c_str());
        say_revive(line);
    };
    const auto finish = [&](const std::string& why) {
        tell(why);
        revive_running_ = false;
    };

    TemplateStore store;
    if (!store.load_best(exe_relative("configs/templates.ini"))) {
        finish("no UI pictures, so the death panel cannot be recognised");
        return;
    }
    ScreenCapture capture;
    const auto grab = [&](Frame& f) {
        return input_.window_valid() && capture.grab(input_.target_window(), f) && f.valid();
    };
    Reviver reviver(input_, store, grab);
    std::string why_not;
    if (!reviver.ready(&why_not)) {
        finish(why_not);
        return;
    }
    reviver.set_stop([this] { return revive_stop_.load(); });
    reviver.set_hp([this] { return vitals_.hp_percent(); });
    reviver.set_debug_dir(exe_relative("revive_debug"));
    reviver.on_report(tell);
    reviver.on_revived([this] { ++revive_count_; });
    // The phone hears of a death once, when its panel is first seen, and of
    // the revive once the Reviver is sure the character is up. Three failed
    // tries are told by stop_after_revive alone, with why the bot stopped.
    reviver.on_death([this] { alert("death", alert_text::death(true)); });
    reviver.on_back([this] { alert("revived", alert_text::revived()); });
    reviver.on_out_of_tries([this](const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            revive_gave_up_why_ = reason;
        }
        revive_gave_up_ = true;
    });

    // The hold, from the first click at a death until the character is up;
    // the mouse, for the clicks of one attempt.
    bool holding = false;
    bool said_held = false;
    const auto release = [&] {
        if (holding) end_hold("reviving");
        holding = false;
    };
    std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::defer_lock);
    Reviver::Guard guard;
    guard.take = [&] {
        // A trash pass under way finishes first -- it is bounded, and gives
        // back what is in the bin when it stops -- rather than having the
        // panel clicked in the middle of one of its drags. Waited for a
        // little at a time, so a stop is not held up behind the pass.
        while (!revive_stop_ && !mouse.try_lock())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (revive_stop_) return false;
        // Not over a hold that is somebody else's, which taking would end. A
        // trash pass that could not give an item back holds everything too,
        // but only to keep keys out of the bin's box; a revive presses no
        // keys, so it goes ahead under that hold and leaves it as it was. The
        // same for a chat line left in doubt, whose hold keeps keys out of the
        // line for as long as it takes: a character left dead meanwhile is
        // worse than a click on the death panel.
        const bool under_other = (trash_hold_ && coord_.critical_reason() == "trash cleaning") ||
                                 (chat_doubt_ && coord_.critical_reason() == kChatHold);
        if (!holding && !under_other) {
            if (!take_hold("reviving")) {
                mouse.unlock();
                if (!said_held)
                    tell("waiting: everything is held off (" + coord_.critical_reason() + ")");
                said_held = true;
                return false;
            }
            holding = true;
            // A direction key the hunter was holding when the character
            // died is let go, as a trash pass lets go of them: held across
            // the revive, it walks the character off the moment it is up.
            input_.release_all();
        }
        said_held = false;
        return true;
    };
    guard.done = [&] {
        if (mouse.owns_lock()) mouse.unlock();
    };
    reviver.set_guard(guard);

    tell("watching");
    while (!revive_stop_) {
        for (int i = 0; i < 12 && !revive_stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (revive_stop_) break;

        const Reviver::Turn turn = reviver.turn();
        if (turn == Reviver::Turn::Alive || turn == Reviver::Turn::Revived) release();
        if (turn == Reviver::Turn::OutOfTries) {
            // The hold stays: tick() ends it once everything else has
            // stopped, so nothing presses a key at the panel meanwhile.
            tell("out of tries -- asking for the whole bot to stop");
            revive_running_ = false;
            return;
        }
    }

    release();
    tell("stopped");
    revive_running_ = false;
}

void GuiApp::stop_after_revive() {
    std::string why;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        why = revive_gave_up_why_;
    }
    // Everything, as the panic key does -- with the trash pass giving its
    // item back on the way -- and the cold start that may be waiting.
    if (navigator_) navigator_->stop();
    simple_stop();
    stop_bot();
    input_.release_all();
    end_hold("reviving");

    const std::string vi = Reviver::out_of_tries_vi();
    simple_stopped_vi_ = vi;
    GB_WARN("Revive: %s -- every job is stopped and every key let go; the character is left "
            "where it is",
            why.c_str());
    alert("revive_failed", vi);
    set_status("Stopped: the revive failed 3 times -- probably out of revive items. "
               "Revive by hand, then start again.",
               60000);
}

// ------------------------------------------------------------ trash cleaning

void GuiApp::say_trash(const std::string& line) {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    trash_log_.push_back(line);
    if (trash_log_.size() > 200) trash_log_.erase(trash_log_.begin());
}

void GuiApp::start_trash() {
    if (trash_running_) {
        trash_oneshot_ = false;          // a one-off pass in progress becomes a run
        return;
    }
    if (trash_thread_.joinable()) trash_thread_.join();
    trash_stop_ = false;
    trash_oneshot_ = false;
    trash_running_ = true;
    trash_thread_ = std::thread([this] { trash_loop(); });
}

void GuiApp::request_trash(bool look_only) {
    trash_request_ = look_only ? 2 : 1;
    if (trash_running_) return;
    if (trash_thread_.joinable()) trash_thread_.join();
    trash_stop_ = false;
    trash_oneshot_ = true;
    trash_running_ = true;
    trash_thread_ = std::thread([this] { trash_loop(); });
}

bool GuiApp::stop_trash() {
    trash_stop_ = true;
    {
        std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
        if (trash_cleaner_) {
            // A pass under way gives its item back as it stops, which needs
            // the game in front. Brought forward from here, by the thread
            // whose window has the focus, rather than by the pass while this
            // thread sits waiting for it below.
            if (trash_pass_ && !input_.window_focused()) input_.focus_window();
            trash_cleaner_->cancel();
        }
    }
    if (trash_thread_.joinable()) trash_thread_.join();
    trash_running_ = false;

    // A pass that could not give its item back -- the game covered where Hủy
    // Bỏ is -- leaves the bin's dialog open, and with the worker gone nothing
    // is left watching it. Whatever presses keys stops too, rather than
    // typing potion keys into the dialog's box once the hold is lifted, and
    // the player is told whether or not those were running: DỪNG stops them
    // before it gets here, and BẮT ĐẦU would start them again straight away.
    // So nothing that presses keys starts until the game has been looked at;
    // see keys_may_start.
    //
    // The executor is not stopped by any of that: it runs from launch, and
    // its buff timer and potions ask only the coordinator, whose hold ends
    // below. It is paused before then, until the bin is seen with nothing
    // waiting -- looked for every couple of seconds, by watch_left_held when
    // no trash worker runs: see bin_seen_clear.
    if (trash_hold_.exchange(false)) {
        stop_hunt();
        stop_macro();
        stop_bot();
        if (navigator_ && navigator_->running()) navigator_->stop();
        ++left_held_gen_;
        trash_left_held_ = true;
        update_executor_pause();
        simple_note("Hộp thoại của thùng rác có thể vẫn đang mở trong game, nên GoBot dừng "
                    "cả đánh quái, bình và buff để không gõ phím vào đó. Bấm Hủy Bỏ trong game "
                    "rồi bấm BẮT ĐẦU.", 15000);
        GB_WARN("Trash: stopped while the bin may still hold an item; the keys are stopped too");
        alert("trash_stuck", alert_text::trash_stuck(true));
        end_hold("trash cleaning");
        return true;
    }
    return false;
}

void GuiApp::bin_seen_clear() {
    trash_left_held_ = false;
    // Not if a chat line left in doubt keeps it paused as well.
    update_executor_pause();
}

bool GuiApp::bin_looks_clear(const TemplateStore& store, std::string* why) {
    // Anything that leaves it in doubt is a no: the screen not captured, or
    // no pictures to know the bin's dialogs by.
    const auto no = [why](const char* text) {
        if (why) *why = text;
        return false;
    };
    if (!bag::can_check(store, TrashList::Action::Delete))
        return no("Không có hình giao diện của game, nên không xem được hộp thoại của thùng rác "
                  "đã đóng chưa.");
    ScreenCapture capture;
    Frame frame;
    if (!input_.window_valid() || !capture.grab(input_.target_window(), frame) || !frame.valid())
        return no("Chưa chụp được màn hình game để xem hộp thoại của thùng rác đã đóng chưa. Để "
                  "game hiện rõ rồi thử lại.");
    CleanSettings clean;
    clean.load(exe_relative("configs/trash.ini"));     // only for where the bin is in the bag
    if (!bag::nothing_held(frame, store, clean.layout))
        return no("Trong game vẫn còn một hộp thoại đang mở, hoặc thùng rác vẫn giữ một món đồ. "
                  "Bấm Hủy Bỏ trong game (thùng rác thì bấm vào nó rồi chọn Hủy Bỏ), rồi thử lại.");
    return true;
}

bool GuiApp::keys_may_start() {
    const auto refuse = [this](const std::string& why) {
        simple_note(why, 12000);
        if (!simple_view_) set_status(why, 12000);
        GB_WARN("Not started: %s", why.c_str());
        return false;
    };
    // A Tự chat worker told to stop with a line under way rubs it out and
    // shuts it first, and may yet hand over a line it cannot show shut: what
    // started meanwhile would type into it.
    if (chat_running_ && chat_quit_)
        return refuse("Đang dừng Tự chat: xoá câu đang gõ dở và đóng ô chat. Chờ một chút rồi bấm lại.");
    if ((chat_doubt_ && !chat_doubt_watched_) || trash_left_held_) {
        if (!simple_store_loaded_) {
            simple_store_loaded_ = true;
            simple_store_.load_best(exe_relative("configs/templates.ini"));
        }
    }
    // Tự chat was stopped with the chat line maybe open, and no worker is
    // left to watch it: every key would go into it. Not until two looks a
    // frame apart show it shut, as the sender and watch_chat_doubt ask: one
    // frame has been seen showing the line shut while it was open. (One a
    // worker still watches holds everything off by itself.)
    if (chat_doubt_ && !chat_doubt_watched_) {
        std::string why;
        if (!chat_line_looks_shut(simple_store_, 2, &why)) return refuse(why);
        chat_seen_shut();
    }
    if (!trash_left_held_) return true;

    // The last stop ended the trash worker's hold while the bin's dialog may
    // still have been open with an item in it, and that dialog's box takes
    // the macro's potion keys as numbers -- the hunter's keys go into it as
    // well. Not until the game is seen with no dialog up and nothing waiting
    // in the bin.
    std::string why;
    if (!bin_looks_clear(simple_store_, &why)) {
        simple_note(why, 12000);
        if (!simple_view_) set_status(why, 12000);
        GB_WARN("Not started, as the bin's dialog may still be open: %s", why.c_str());
        return false;
    }
    bin_seen_clear();
    return true;
}

void GuiApp::watch_left_held() {
    // With a trash worker running, its own look does this (see trash_loop).
    // Without one, nothing else would look until BẮT ĐẦU, and the executor's
    // buffs and potions -- paused by the stop -- would stay off all session
    // for a player who answered the bin's dialog and went on by hand.
    if (left_held_look_.valid()) {
        if (left_held_look_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const bool clear = left_held_look_.get();
        // Only for the doubt it was started for, and not over a trash worker
        // that has started since, which looks for itself.
        if (!clear || !trash_left_held_ || trash_running_ || left_held_look_gen_ != left_held_gen_)
            return;
        bin_seen_clear();
        const std::string note = "Thùng rác không còn giữ gì. Bấm BẮT ĐẦU để đánh quái, bình và "
                                 "buff chạy lại.";
        simple_note(note, 12000);
        if (!simple_view_) set_status(note, 12000);
        GB_INFO("Trash: the game shows nothing waiting in the bin any more; the executor's buffs "
                "and potions go on");
        return;
    }
    if (!trash_left_held_ || trash_running_) return;
    const std::uint64_t now = now_ms();
    if (now - left_held_looked_ms_ < 2000) return;
    left_held_looked_ms_ = now;
    left_held_look_gen_ = left_held_gen_;
    // One look at a time, so the pictures loaded here are only ever the
    // look's.
    left_held_look_ = std::async(std::launch::async, [this] {
        if (!left_held_store_loaded_) {
            left_held_store_loaded_ = true;
            left_held_store_.load_best(exe_relative("configs/templates.ini"));
        }
        return bin_looks_clear(left_held_store_, nullptr);
    });
}

bool GuiApp::take_hold(const std::string& reason) {
    std::lock_guard<std::mutex> lock(hold_mutex_);
    if (coord_.critical_active() && coord_.critical_reason() != reason) return false;
    coord_.engage_critical(reason);
    return true;
}

void GuiApp::end_hold(const std::string& reason) {
    std::lock_guard<std::mutex> lock(hold_mutex_);
    if (coord_.critical_active() && coord_.critical_reason() == reason) coord_.disengage_critical();
}

/// The timed passes, and the ones asked for.
///
/// Every other part of the bot is held off for the length of a pass: the
/// hunter walking off with the bag open drags it out of reach, and a potion key
/// pressed while a quantity box has the keyboard types a number into it.
void GuiApp::trash_loop() {
    // What the worker says for itself goes to the log as well as the screen;
    // what the cleaner reports it logs on its own.
    const auto tell = [this](const std::string& line) {
        GB_INFO("Trash: %s", line.c_str());
        say_trash(line);
    };
    const auto finish = [&](const std::string& why) {
        tell(why);
        trash_running_ = false;
    };

    TemplateStore store;
    if (!store.load_best(exe_relative("configs/templates.ini"))) {
        finish("Không có hình giao diện của game, nên không nhận ra được túi đồ.");
        return;
    }
    CountReader counts;
    if (!counts.load(GlyphFont::default_dir()))
        tell("Không có bộ chữ số của game, nên không đọc được số lượng trên ô đồ.");
    ItemCatalog catalog;
    catalog.load(ItemCatalog::default_dir());
    ScreenCapture capture;
    const auto grab = [&](Frame& f) { return capture.grab(input_.target_window(), f); };

    // Kept from one look at the file to the next, and replaced only by a
    // reading that worked: the trash tab rewrites the file in place as boxes
    // are ticked, and one caught half written reads as the built-in defaults
    // -- the equipment tabs ticked and nothing on the list.
    const std::string config = exe_relative("configs/trash.ini");
    CleanSettings settings;
    settings.load(config);
    std::uint64_t read_ms = now_ms();

    // The first timed pass shortly after the start, so what was put on the
    // list is dealt with now rather than in ten minutes.
    std::uint64_t next = now_ms() + 15'000;
    // A pass that could not give an item back keeps everything held off --
    // the bin's dialog may be open, and its box takes the macro's potion keys
    // as numbers -- until the game shows nothing waiting: the player answered
    // it, or a later pass did.
    bool held = trash_hold_;
    std::uint64_t held_looked_ms = 0;
    // The same from the start when the game already shows the bin asking, or
    // something waiting in it -- left by a pass a stop cut short, or by hand:
    // held off now, not from the first pass fifteen seconds on.
    if (!held) {
        Frame f;
        if (grab(f) && f.valid()) {
            const BagView v = bag::find(f, store, settings.layout);
            if ((bag::bin_asking(f, store) || (v.found && !v.bin_empty)) &&
                take_hold("trash cleaning")) {
                held = true;
                trash_hold_ = true;
                held_looked_ms = now_ms();
                tell("Thùng rác đang giữ một món đồ, hoặc hộp thoại của nó đang mở: đánh quái, "
                     "bình và buff tạm dừng đến khi không còn gì chờ. Bấm Hủy Bỏ trong game.");
            }
        }
    }
    // A fresh look at the game, and if it shows nothing of the bin's waiting,
    // the end of this worker's hold -- and of what an earlier stop left in
    // doubt, which held the executor's buffs and potions off with the worker
    // gone (see stop_trash), once the bin's dialogs can be told from the rest,
    // as keys_may_start asks. That stop stopped the fighting and the potion
    // keys as well, and those wait for BẮT ĐẦU.
    const auto look_for_clear = [&] {
        held_looked_ms = now_ms();
        Frame f;
        if (!grab(f) || !f.valid() || !bag::nothing_held(f, store, settings.layout)) return;
        const bool was_held = held;
        if (held) {
            held = false;
            trash_hold_ = false;
            end_hold("trash cleaning");
        }
        const bool left = trash_left_held_;
        if (left && bag::can_check(store, TrashList::Action::Delete)) bin_seen_clear();
        if (was_held && !left)
            tell("Không còn gì chờ trong thùng rác; đánh quái, bình và buff chạy lại.");
        else if (was_held || !trash_left_held_)
            tell("Không còn gì chờ trong thùng rác.");
    };
    while (!trash_stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if ((held || trash_left_held_) && now_ms() - held_looked_ms > 1500) look_for_clear();
        const int request = trash_request_.exchange(0);
        if (request == 0 && now_ms() - read_ms > 1000) {
            CleanSettings fresh;
            if (fresh.load(config)) settings = fresh;
            read_ms = now_ms();
        }
        if (request == 0) {
            // A pass asked for by a button ends here -- unless it left the
            // bin holding something, which is watched for until it is not.
            if (trash_oneshot_) {
                if (!held) break;
                continue;
            }
            if (!settings.enabled || now_ms() < next) continue;
        }

        // Read again for the pass itself, a few times if it is caught being
        // written.
        for (int i = 0; i < 5; ++i) {
            CleanSettings fresh;
            if (fresh.load(config)) { settings = fresh; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        read_ms = now_ms();

        // Not while the game's chat line may be open, which Tự chat could not
        // show shut: a pass presses the bag key, a letter in that line. One a
        // Tự chat worker still watches is under its hold as well, but one a
        // stop handed over holds nothing, and only this keeps a pass asked
        // for with a button out of it. Tried again shortly, as below.
        if (chat_doubt_) {
            tell(kTrashWaitsForChat);
            if (trash_oneshot_ && !held) break;
            next = now_ms() + 30'000;
            continue;
        }

        // Something else holding everything off -- a revive, say -- is not
        // something to clean the bag through, and its hold is not this
        // worker's to end. Tried again shortly. This worker's own hold is
        // what a pass is for: it starts by giving the item back.
        const auto held_by_other = [&] {
            return coord_.critical_active() && coord_.critical_reason() != "trash cleaning";
        };
        if (held_by_other()) {
            tell("Chờ: bot đang tạm dừng (" + coord_.critical_reason() + "), dọn rác sau.");
            if (trash_oneshot_ && !held) break;
            next = now_ms() + 30'000;
            continue;
        }
        next = now_ms() + static_cast<std::uint64_t>(settings.interval_minutes) * 60'000ull;

        TrashList list;
        list.build(exe_relative("configs/trash"), settings, catalog);
        if (list.items.empty()) {
            tell("Danh sách đồ bỏ đi đang trống, nên không có gì để dọn.");
            if (trash_oneshot_ && !held) break;
            continue;
        }
        if (!input_.window_valid()) {
            tell("Chưa thấy cửa sổ game.");
            if (trash_oneshot_ && !held) break;
            continue;
        }

        const bool look_only = request == 2;
        BagCleaner cleaner(input_, store, grab);
        cleaner.set_settings(settings);
        cleaner.set_trash(&list);
        cleaner.set_counts(&counts);
        cleaner.set_debug_dir(exe_relative("trash_debug"));
        cleaner.set_dry_run(look_only);
        cleaner.on_report([this](const std::string& line) { say_trash(line); });
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = &cleaner;
        }
        // A stop that came while all of the above was being done found no
        // cleaner to cancel; it is passed on now instead.
        if (trash_stop_) cleaner.cancel();

        BagCleaner::Result r;
        bool passed = false;
        {
            // Not while the revive sequence or the party worker is clicking,
            // nor they while this is. The hold is taken only now, with the
            // mouse: a revive that began while this waited for it keeps its
            // own, and the pass waits instead. The chat line is asked about
            // again with the mouse in hand: a line left in doubt since the
            // look above was under Tự chat's hold until then.
            std::lock_guard<std::mutex> mouse(game_mouse_mutex_);
            if (!chat_doubt_ && take_hold("trash cleaning")) {
                passed = true;
                trash_pass_ = true;
                tell(look_only ? "Thử tìm trong túi đồ..." : "Bắt đầu dọn rác...");
                input_.release_all();
                if (!input_.window_focused()) {
                    input_.focus_window();
                    std::this_thread::sleep_for(std::chrono::milliseconds(350));
                }
                r = cleaner.run();
                // Kept while the bin may still hold the item, and by a pass
                // that never got to a clean screen -- a look-only one, or one
                // that met a dialog it would not answer -- since it saw
                // nothing of an earlier item coming back. Otherwise ended, if
                // it is still this pass's: one taken over during the pass is
                // somebody else's to end.
                held = r.keeps_hold(held);
                trash_hold_ = held;
                if (!held) end_hold("trash cleaning");
                trash_pass_ = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = nullptr;
        }
        if (!passed) {
            if (chat_doubt_) tell(kTrashWaitsForChat);
            else tell("Chờ: bot đang tạm dừng (" + coord_.critical_reason() + "), dọn rác sau.");
            if (trash_oneshot_ && !held) break;
            next = now_ms() + 30'000;
            continue;
        }

        trash_thrown_ += r.deleted + r.dropped;
        std::string extra;
        if (r.failed > 0)
            extra += " " + std::to_string(r.failed) + " món không bỏ được.";
        if (r.doubtful > 0)
            extra += " " + std::to_string(r.doubtful) + " món giống nhưng không chắc, nên để lại.";
        extra += trash_use_said_vi(r, look_only);       // "Dùng" (simple_trash_use.cpp)
        if (!r.trouble.empty())
            tell("Dừng: " + r.trouble + "." + extra);
        else if (look_only)
            tell((r.matched + r.use_matched == 0
                ? std::string("Thử xong: không thấy món nào trong danh sách.")
                : r.matched == 0
                ? std::string("Thử xong: không có ô nào sẽ bị bỏ. Không bỏ gì.")
                : "Thử xong: thấy " + std::to_string(r.matched) + " ô sẽ bị bỏ. Không bỏ gì.") +
                 extra);
        else
            tell("Xong: xoá " + std::to_string(r.deleted) + ", vứt " +
                 std::to_string(r.dropped) + " món." + extra);
        if (held) {
            // Tried again in a minute -- a pass starts by giving it back --
            // and watched for in between, in case the player does it first.
            next = now_ms() + 60'000;
            held_looked_ms = now_ms();
            tell("Món đồ có thể vẫn đang ở trong thùng rác hoặc trong hộp thoại của nó. Bấm "
                 "Hủy Bỏ trong game; đánh quái, bình và buff tạm dừng đến khi không còn gì chờ.");
        }
        // A pass that left the bin clear ends a stop's doubt as well, now
        // rather than at the next look, which a pass asked for by a button
        // does not stay for.
        if (!held && trash_left_held_) look_for_clear();
        if (trash_oneshot_ && !held) break;
    }
    trash_running_ = false;
}

// --------------------------------------------------------------- auto chat

namespace {

/// Runs `fn` on the way out of a scope, however it is left.
struct OnExit {
    std::function<void()> fn;
    ~OnExit() { if (fn) fn(); }
};

const char* const kStopped = "Đã dừng.";
const char* const kUserActive = "Bạn đang dùng chuột hoặc bàn phím — chat sau.";

/// How far over the HP potion's mark HP has to be for a line to start, and the
/// most that asks for, so that a potion set high still lets a full bar chat.
constexpr int kChatHpMargin = 10;
constexpr int kChatHpTop = 95;

/// What a look at the bar said, for the log: the numbers it went by.
std::string bar_numbers(const ChatBar& bar) {
    char out[160];
    std::snprintf(out, sizeof out, " (ô chat %s, dark %d, giống ô chat đóng %.3f, vết chữ %d)",
                  chat_bar_name(bar.state), bar.dark, static_cast<double>(bar.closed_score),
                  bar.ink);
    return out;
}

} // namespace

void GuiApp::say_chat(const std::string& line) {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    chat_log_.push_back(line);
    if (chat_log_.size() > 200) chat_log_.erase(chat_log_.begin());
}

void GuiApp::start_chat() {
    {
        // A worker here already -- for a one-off look or line, or stopping
        // because Tự chat was unticked and waiting to see the chat line shut
        // -- becomes this run, started over from the first line's grace. Under
        // the lock the worker leaves by, so that it is not on its way out
        // meanwhile with the run lost.
        std::lock_guard<std::mutex> lock(chat_life_mutex_);
        if (chat_running_ && !chat_quit_) {
            chat_stop_ = false;
            chat_oneshot_ = false;
            chat_request_ = 0;
            chat_restart_ = true;
            return;
        }
    }
    // A worker told to quit finishes rubbing out its line first: a few
    // seconds at most, as it never waits on a line left in doubt.
    if (chat_thread_.joinable()) chat_thread_.join();
    chat_stop_ = false;
    chat_quit_ = false;
    chat_restart_ = false;
    chat_oneshot_ = false;
    // A Gửi thử click from before is not this run's to answer: it would type
    // straight away, skipping the first line's grace.
    chat_request_ = 0;
    chat_sent_ = 0;
    chat_running_ = true;
    chat_thread_ = std::thread([this] { chat_loop(); });
}

void GuiApp::request_chat(bool send_one) {
    {
        std::lock_guard<std::mutex> lock(chat_life_mutex_);
        if (chat_running_ && !chat_quit_) {
            chat_request_ = send_one ? 2 : 1;
            return;
        }
    }
    if (chat_thread_.joinable()) chat_thread_.join();
    chat_request_ = send_one ? 2 : 1;
    chat_stop_ = false;
    chat_quit_ = false;
    chat_restart_ = false;
    chat_oneshot_ = true;
    chat_running_ = true;
    chat_thread_ = std::thread([this] { chat_loop(); });
}

void GuiApp::stop_chat(bool quit, bool wait) {
    chat_stop_ = true;
    if (quit) chat_quit_ = true;
    {
        std::lock_guard<std::mutex> lock(chat_sender_mutex_);
        if (chat_sender_) {
            // A line under way is rubbed out and closed as it stops, which
            // needs the game in front -- and a stop is mostly pressed in this
            // window. Brought forward from here, by the thread that has the
            // foreground to give, rather than by the line meanwhile.
            if (!input_.window_focused()) input_.focus_window();
            chat_sender_->cancel();
        }
    }
    chat_next_ms_ = 0;
    if (!wait) return;
    if (chat_thread_.joinable()) chat_thread_.join();
    chat_running_ = false;
    // Asked for while it ran and never answered: not for the next run.
    chat_request_ = 0;
}

void GuiApp::remember_said(const std::string& typed) {
    std::lock_guard<std::mutex> lock(chat_said_mutex_);
    const std::uint64_t now = now_ms();
    chat_said_.emplace_back(typed, now);
    while (!chat_said_.empty() &&
           (chat_said_.size() > 20 || now - chat_said_.front().second > 30 * 60'000ull))
        chat_said_.erase(chat_said_.begin());
}

std::vector<std::string> GuiApp::recently_said() {
    std::lock_guard<std::mutex> lock(chat_said_mutex_);
    const std::uint64_t now = now_ms();
    std::vector<std::string> out;
    for (const auto& [text, when] : chat_said_)
        if (now - when <= 30 * 60'000ull) out.push_back(text);
    return out;
}

std::uint64_t GuiApp::last_said_ms() {
    std::lock_guard<std::mutex> lock(chat_said_mutex_);
    return chat_said_.empty() ? 0 : chat_said_.back().second;
}

void GuiApp::update_executor_pause() {
    // Both doubts pause it, and either clearing must not let it go while the
    // other is left: worked out from both, under one lock, whoever asks.
    std::lock_guard<std::mutex> lock(pause_mutex_);
    if (executor_) executor_->set_paused(trash_left_held_ || chat_doubt_);
}

void GuiApp::chat_seen_shut() {
    chat_doubt_ = false;
    update_executor_pause();
}

bool GuiApp::chat_line_looks_shut(const TemplateStore& store, int looks, std::string* why) {
    const auto no = [why](const char* text) {
        if (why) *why = text;
        return false;
    };
    // No game, no chat line to type into: a game started again starts with
    // its chat line shut.
    if (!input_.window_valid()) return true;
    ChatSettings settings;
    settings.load(exe_relative("configs/chat.ini"));       // only for the pictures' names
    if (!can_look_for_chat(store, settings))
        return no("Không có hình ô chat của game, nên không biết ô chat trong game đã đóng chưa.");
    ScreenCapture capture;
    for (int i = 0; i < looks; ++i) {
        // A frame or two apart: one odd frame does not decide it.
        if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds(150));
        Frame f;
        if (!capture.grab(input_.target_window(), f) || !f.valid())
            return no("Chưa chụp được màn hình game để xem ô chat đã đóng chưa. Để game hiện rõ rồi "
                      "thử lại.");
        const ChatBar bar = look_chat_bar(f, store, settings);
        if (bar.state == ChatBarState::Open)
            return no("Ô chat trong game vẫn đang mở. Xoá chữ trong ô chat rồi bấm Enter để đóng nó, "
                      "rồi bấm BẮT ĐẦU.");
        if (bar.state != ChatBarState::Closed)
            return no("Không thấy ô chat ở góc trái dưới màn hình game, nên chưa biết ô chat đã đóng "
                      "chưa. Đừng để cửa sổ nào che khung chat, rồi bấm BẮT ĐẦU.");
    }
    return true;
}

void GuiApp::watch_chat_doubt() {
    // With a Tự chat worker watching it, the doubt is the worker's, and it
    // looks every second. Without one -- it was stopped with the line maybe
    // open -- nothing would look until BẮT ĐẦU, and the executor's buffs and
    // potions would stay paused all session for a player who shut the line by
    // hand and went on.
    if (chat_doubt_look_.valid()) {
        if (chat_doubt_look_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const bool shut = chat_doubt_look_.get();
        // Only for the doubt it was started for, and not over a worker
        // watching one of its own since.
        if (!shut || !chat_doubt_ || chat_doubt_watched_ || chat_doubt_look_gen_ != chat_doubt_gen_)
            return;
        chat_seen_shut();
        const std::string note = "Ô chat trong game đã đóng. Bấm BẮT ĐẦU để đánh quái, bình và buff "
                                 "chạy lại.";
        simple_note(note, 12000);
        if (!simple_view_) set_status(note, 12000);
        GB_INFO("Chat: the game shows the chat line shut; the executor's buffs and potions go on");
        return;
    }
    if (!chat_doubt_ || chat_doubt_watched_) return;
    const std::uint64_t now = now_ms();
    if (now - chat_doubt_looked_ms_ < 2000) return;
    chat_doubt_looked_ms_ = now;
    chat_doubt_look_gen_ = chat_doubt_gen_;
    // One look at a time, so the pictures loaded here are only ever this
    // look's; on a thread of its own, as a capture can take a while.
    chat_doubt_look_ = std::async(std::launch::async, [this] {
        if (!chat_doubt_store_loaded_) {
            chat_doubt_store_loaded_ = true;
            chat_doubt_store_.load_best(exe_relative("configs/templates.ini"));
        }
        return chat_line_looks_shut(chat_doubt_store_, 2, nullptr);
    });
}

/// The timed lines, and the ones asked for.
///
/// Every other part of the bot is held off while a line is typed: with the
/// chat line open a skill key is a letter in it and a potion key a digit, and
/// with it shut the line's own letters are skill keys. And held off for as
/// long after as the line may still be open: until a look shows it shut.
void GuiApp::chat_loop() {
    const auto tell = [this](const std::string& line) {
        GB_INFO("Chat: %s", line.c_str());
        say_chat(line);
    };
    // The way out, under the lock start_chat turns a worker into a run under:
    // a run asked for meanwhile keeps the worker. True when it stays.
    const auto stays = [this] {
        std::lock_guard<std::mutex> lock(chat_life_mutex_);
        if (chat_restart_ && !chat_quit_) return true;
        chat_next_ms_ = 0;
        chat_running_ = false;
        return false;
    };
    const auto finish = [&](const std::string& why) {
        tell(why);
        std::lock_guard<std::mutex> lock(chat_life_mutex_);
        chat_next_ms_ = 0;
        chat_running_ = false;
    };

    TemplateStore store;
    if (!store.load_best(exe_relative("configs/templates.ini"))) {
        finish("Không có hình giao diện của game, nên không nhận ra được ô chat.");
        return;
    }
    // Kept from one look at the file to the next, and replaced only by a
    // reading that worked: the tab rewrites the file as things change.
    const std::string config = exe_relative("configs/chat.ini");
    ChatSettings settings;
    if (!settings.load(config)) tell("Không đọc được configs/chat.ini, nên dùng cài đặt mặc định.");
    if (!can_look_for_chat(store, settings)) {
        finish("Không có hình ô chat của game (" + settings.open_picture + "), nên không biết ô "
               "chat đang mở hay đóng — không chat.");
        return;
    }
    // Enter answers a dialog. Without the pictures to know a dialog by there
    // is no telling whether one is up, and nothing is typed.
    if (!ChatDialogs::can_check(store)) {
        finish("Không có hình hộp thoại của game, nên không biết có hộp thoại nào đang mở không — "
               "không chat.");
        return;
    }
    ChatDialogs dialogs;
    dialogs.load(exe_relative("configs/trash.ini"), exe_relative("configs/party.ini"),
                 exe_relative("configs/routines.ini"));
    const auto dialog_up = [&](const Frame& f) { return dialogs.up(f, store); };

    ScreenCapture capture;
    const auto grab = [&](Frame& f) { return capture.grab(input_.target_window(), f); };

    // Each reason for waiting said once a minute at most, not every look.
    std::map<std::string, std::uint64_t> said;
    const auto tell_now_and_then = [&](const std::string& why) {
        const std::uint64_t now = now_ms();
        const auto it = said.find(why);
        if (it != said.end() && now - it->second < 60'000) return;
        said[why] = now;
        tell(why);
    };

    // Waits that any stop cuts short: false when one came. And waits only
    // quitting cuts short, for a chat line in doubt, which is watched through
    // an unticking of Tự chat as through anything else.
    const auto nap = [&](int ms) {
        for (int slept = 0; slept < ms && !chat_stop_ && !chat_quit_; slept += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, ms - slept)));
        return !chat_stop_ && !chat_quit_;
    };
    const auto nap_unless_quit = [&](int ms) {
        for (int slept = 0; slept < ms && !chat_quit_; slept += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, ms - slept)));
        return !chat_quit_;
    };

    // Under the hold: a key going down as the hold began finishes first --
    // nothing else gets the coordinator's token while the hold lasts -- and
    // nothing is left held down.
    const auto settle = [&](bool for_doubt) {
        const auto stopped = [&] { return chat_quit_ || (!for_doubt && chat_stop_); };
        for (int i = 0; i < 40 && coord_.held() && !stopped(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (stopped() || coord_.held()) return false;
        input_.release_all();
        return true;
    };

    // Whether anybody has used the mouse or keyboard since `since`. Only
    // asked under the hold, after settle(): GoBot presses nothing then, so
    // whatever Windows counts is a person's -- who may be typing in the chat.
    const auto last_input = [] {
        LASTINPUTINFO info{};
        info.cbSize = sizeof info;
        return GetLastInputInfo(&info) ? info.dwTime : 0;
    };
    const auto touched_since = [&](DWORD since) { return last_input() != since; };

    // A chat line that may be open: everything that presses keys held off --
    // the hold, kept from the line that left it so, and the executor paused
    // -- until a look shows it shut. See ChatDoubt.
    bool doubt_held = false;            // this worker's doubt, with its hold
    void* doubt_window = nullptr;       // the game the line was open in
    ChatDoubt doubt;
    ChatDoubt::Hooks hooks;
    hooks.hold = [&] {
        if (!doubt_held) {
            doubt_held = true;
            doubt_window = input_.target_window();
            chat_doubt_watched_ = true;
            chat_doubt_ = true;
            update_executor_pause();
            tell("Ô chat có thể đang mở: đánh quái, bình và buff tạm dừng đến khi thấy ô chat đóng.");
        }
        // Taken back if somebody else's hold ended meanwhile; somebody else's
        // still on holds everything off as well.
        if (!coord_.critical_active()) take_hold(kChatHold);
    };
    hooks.release = [&](const std::string& why) {
        doubt_held = false;
        chat_doubt_ = false;
        chat_doubt_watched_ = false;
        update_executor_pause();
        end_hold(kChatHold);
        tell(why + " GoBot chạy tiếp.");
    };
    hooks.same_game = [&] {
        return input_.window_valid() && input_.target_window() == doubt_window;
    };
    // Closed only with the mouse to itself, nothing held down, and nobody at
    // the mouse or keyboard for a second: the open line may be the player's
    // by now, typing in it.
    hooks.when_quiet = [&](const std::function<bool()>& close) {
        std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::try_to_lock);
        if (!mouse.owns_lock() || trash_pass_) return false;
        if (!settle(true)) return false;
        const DWORD quiet = last_input();
        if (!nap_unless_quit(1000)) return false;
        if (touched_since(quiet)) {
            tell_now_and_then("Ô chat đang mở và có người đang dùng chuột hoặc bàn phím — chưa đóng.");
            return false;
        }
        return close();
    };
    hooks.say = [&](const std::string& why) { tell_now_and_then(why); };
    doubt.set_hooks(hooks);
    // The looks and the closing, one sender for the whole of a doubt: it
    // knows what it pressed itself.
    std::unique_ptr<ChatSender> closer;
    const auto note_doubt = [&](const ChatSender::Result& r) {
        if (!r.left_open) return;
        if (!closer) {
            closer = std::make_unique<ChatSender>(input_, store, grab);
            closer->set_settings(settings);
            closer->set_dialog_check(dialog_up);
            closer->set_debug_dir(exe_relative("chat_debug"));
        }
        doubt.note(r, r.typed + settings.clear_backspaces + 2, now_ms());
    };

    // One line: what can be told without a look, then the mouse and the hold,
    // then a look, then the line. Nothing has been pressed when the result
    // says not_now. A line that may have been left open keeps the hold.
    const auto attempt = [&](const ChatText& line, bool asked) {
        ChatSender::Result none;
        none.not_now = true;
        const auto wait = [&](std::uint64_t ms, std::string why) {
            none.wait_ms = ms;
            none.trouble = std::move(why);
            return none;
        };

        ChatGateInput in;
        in.window = input_.window_valid();
        // Asked for with a button in this window: the game is brought forward
        // first, as the trash tab's Thử tìm does. The timed lines never do
        // that -- the player may be busy in another window, and is left to it.
        if (asked && in.window && !input_.window_focused()) {
            input_.focus_window();
            if (!nap(350)) return wait(0, kStopped);
        }
        const auto fill = [&] {
            in.focused = input_.window_focused();
            {
                std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
                in.keeper = keeper_flow_ != nullptr;
            }
            in.trash = trash_pass_ || trash_hold_ || trash_left_held_;
            const std::string reason = coord_.critical_active() ? coord_.critical_reason()
                                                                : std::string();
            in.held_by = reason == kChatHold ? std::string() : reason;
            in.hp_pct = vitals_.hp_percent();
            // Started only with some way to go before the potion's mark, where
            // the guard below ends a line: HP hovering at the mark in a fight
            // would otherwise start a line, end it, and start it again,
            // holding the potion off each time.
            const int floor = chat_hp_floor_.load();
            in.hp_floor = std::max(floor, std::min(kChatHpTop, floor + kChatHpMargin));
        };
        fill();
        ChatGate gate = chat_gate(in);
        if (!gate.send) return wait(gate.wait_ms, gate.why);

        // Not while the trash cleaner, the revive sequence or the party worker
        // is clicking, nor they while this types: a click can shut the line,
        // and a trash pass has the mouse for the whole of it.
        std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::try_to_lock);
        if (!mouse.owns_lock() || trash_pass_)
            return wait(5000, trash_pass_ ? "Đang dọn rác — chat sau."
                                          : "Bot đang bấm chuột trong game — chat sau.");
        if (!take_hold(kChatHold))
            return wait(10000, "Bot đang tạm dừng (" + coord_.critical_reason() + ") — chat sau.");
        // How long everything else waits for this line, which the line's
        // report counts from: the potions are held off from here.
        const std::uint64_t held_at = now_ms();
        bool keep_hold = false;
        OnExit hold{ [&] { if (!keep_hold) end_hold(kChatHold); } };
        if (!settle(false)) return wait(5000, chat_stop_ ? kStopped : "Bot đang bấm phím — chat sau.");
        // GoBot presses nothing from here until the line, so any input
        // Windows counts from now on is a person's -- maybe typing in the
        // chat. Taken at once, so that the whole of the pause below counts,
        // and asked again just before the first Enter.
        const DWORD quiet = last_input();
        if (!nap(300)) return wait(0, kStopped);

        Frame frame;
        if (!grab(frame) || !frame.valid()) return wait(5000, "Chưa chụp được màn hình game.");
        fill();
        in.looked = true;
        in.hud = SessionManager::hud_present(frame);
        in.dialog = dialog_up(frame);
        in.bar = look_chat_bar(frame, store, settings).state;
        in.user_active = touched_since(quiet);
        gate = chat_gate(in);
        if (!gate.send) return wait(gate.wait_ms, gate.why);

        ChatSender sender(input_, store, grab);
        sender.set_settings(settings);
        sender.set_dialog_check(dialog_up);
        sender.set_debug_dir(exe_relative("chat_debug"));
        sender.set_held_since(held_at);
        sender.on_report([](const std::string& s) { GB_INFO("Chat: %s", s.c_str()); });
        sender.set_ready_check([&]() -> std::string {
            return touched_since(quiet) ? kUserActive : std::string();
        });
        // Between keys: HP falling under the potion's mark ends the line, so
        // the potion is not held off for the rest of it; so does the hold
        // being anything but this line's own, whoever took it over.
        sender.set_guard([this]() -> std::string {
            const float hp = vitals_.hp_percent();
            if (hp >= 0.0f && hp < static_cast<float>(chat_hp_floor_.load()))
                return "Máu thấp — dừng câu đang gõ.";
            const std::string reason = coord_.critical_reason();
            if (reason != kChatHold) return "Bot đang tạm dừng (" + reason + ").";
            return {};
        });
        {
            std::lock_guard<std::mutex> lock(chat_sender_mutex_);
            chat_sender_ = &sender;
        }
        OnExit forget{ [this] {
            std::lock_guard<std::mutex> lock(chat_sender_mutex_);
            chat_sender_ = nullptr;
        } };
        // A stop that came while all of the above was done found no line to
        // cancel; it is passed on now instead.
        if (chat_stop_ || chat_quit_) sender.cancel();

        ChatSender::Result r = sender.send(line);

        // A dialog came up with the line open: what was typed is rubbed out
        // and no Enter pressed. The mouse is let go so that whoever answers
        // the dialog -- the party worker, or the player -- can; the hold is
        // kept, so that nothing types into the open line meanwhile; and the
        // line is closed once the dialog has gone, ten seconds at most.
        if (!r.dialog.empty() && r.left_open) {
            mouse.unlock();
            tell(r.trouble);
            const std::uint64_t since = now_ms();
            while (!chat_quit_ && now_ms() - since < 10'000) {
                if (!nap_unless_quit(300)) break;
                Frame f;
                if (grab(f) && f.valid() && dialog_up(f).empty()) break;
            }
            if (!chat_quit_ && sender.close_left_open(r.typed + settings.clear_backspaces))
                r.left_open = false;
        }
        // Still maybe open: the hold is not let go, but kept for as long as
        // that lasts (see note_doubt).
        keep_hold = r.left_open;
        return r;
    };

    ChatSchedule schedule;
    int put_off_in_row = 0;                 // lines ended part way, one after another
    bool stopped_self = false;              // Tự chat stopped itself: done once the line is shut
    std::uint64_t read_ms = now_ms();
    // A run from its beginning: the first line after its grace, nothing sent.
    const auto begin_run = [&] {
        schedule.start(now_ms(), settings);
        chat_sent_ = 0;
        put_off_in_row = 0;
        stopped_self = false;
        tell(settings.lines.empty()
                 ? std::string("Danh sách câu đang trống.")
                 : "Bắt đầu: câu đầu tiên sau " + std::to_string(settings.first_delay_s) +
                       " giây, rồi mỗi " + std::to_string(settings.interval_s) + " giây một câu.");
        chat_next_ms_ = settings.lines.empty() ? 0 : schedule.due_at();
    };
    if (!chat_oneshot_) begin_run();

    for (;;) {
        while (!chat_quit_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // A one-off, or a worker stopping, turned into a run: from the
            // start, as a run of its own would be.
            if (chat_restart_.exchange(false)) begin_run();
            const int request = chat_request_.exchange(0);
            if (now_ms() - read_ms > 1000) {
                ChatSettings fresh;
                if (fresh.load(config)) {
                    settings = fresh;
                    schedule.update(settings);
                    if (closer) closer->set_settings(settings);
                }
                read_ms = now_ms();
            }

            if (request == 1) {
                // Kiểm tra ô chat: a look, and what it saw. Nothing is pressed.
                Frame f;
                if (!input_.window_valid() || !grab(f) || !f.valid()) {
                    tell("Chưa chụp được màn hình game.");
                } else {
                    const ChatBar bar = look_chat_bar(f, store, settings);
                    const std::string dialog = dialog_up(f);
                    char scores[200];
                    std::snprintf(scores, sizeof scores,
                                  " (điểm màu ô chat mở: %d, giống ô chat đóng: %.3f, vết chữ: %d)",
                                  bar.dark, static_cast<double>(bar.closed_score), bar.ink);
                    std::string said_line = std::string("Ô chat ") + chat_bar_name(bar.state) + scores + ".";
                    if (bar.state == ChatBarState::Unknown) {
                        said_line = bar.found
                            ? std::string("Thấy ô chat nhưng không giống hình ô chat đang mở hay đang "
                                          "đóng") + scores + "."
                            : std::string("Không thấy ô chat") + scores +
                                  ". Đừng để cửa sổ nào che khung chat ở góc trái dưới màn hình game.";
                        ChatSender keeper(input_, store, grab);
                        keeper.set_settings(settings);
                        keeper.set_debug_dir(exe_relative("chat_debug"));
                        keeper.keep(f, "check", &bar);
                    }
                    if (!dialog.empty()) said_line += " Đang có hộp thoại: " + dialog + ".";
                    tell(said_line);
                }
                continue;
            }
            if (request == 2) {
                // Gửi thử 1 câu: the first line, under every rule a timed one
                // goes by. The run's own turn and timer are left as they were.
                const std::string raw = settings.lines.empty() ? settings.test_line : settings.lines.front();
                const ChatText line = prepare_chat(raw, settings.method, settings.strip_diacritics,
                                                   settings.max_length);
                const std::uint64_t said_at = last_said_ms();
                const std::uint64_t floor_ms = ChatSettings::kMinInterval * 1000ull;
                const std::uint64_t asked_at = now_ms();
                if (chat_doubt_) {
                    tell("Chưa gửi thử được: ô chat trong game có thể đang mở. Đóng ô chat trong game "
                         "(xoá chữ trong đó rồi bấm Enter) rồi thử lại.");
                } else if (line.empty()) {
                    tell("Câu số 1 không còn chữ nào gõ được.");
                } else if (said_at != 0 && asked_at < said_at + floor_ms) {
                    // Never two lines closer together than the shortest
                    // interval, as for the timed ones: a click just after a
                    // timed line, or after the last Gửi thử, would otherwise
                    // put two in the channel seconds apart.
                    tell("Chưa gửi thử được: mới gửi một câu " +
                         std::to_string((asked_at - said_at) / 1000) + " giây trước — chờ ít nhất " +
                         std::to_string(ChatSettings::kMinInterval) + " giây giữa hai câu.");
                } else {
                    const ChatSender::Result r = attempt(line, true);
                    note_doubt(r);
                    if (r.not_now) {
                        tell("Chưa gửi thử được: " + r.trouble);
                    } else if (r.sent) {
                        remember_said(line.typed);
                        GB_INFO("Chat: gửi thử (%s): \"%s\"", chat_method_name(settings.method),
                                line.typed.c_str());
                        say_chat("Đã gửi thử: \"" + line.typed + "\"");
                        if (!r.trouble.empty()) tell(r.trouble);
                    } else {
                        tell("Gửi thử không được: " + r.trouble + bar_numbers(r.last));
                    }
                    if (r.stop) {
                        // The line would not close, or the bar did not look
                        // like either picture: a person has to look, run or no
                        // run -- once the line is known shut.
                        if (!chat_oneshot_) tell("Tự chat đã dừng.");
                        stopped_self = true;
                    }
                }
                continue;
            }

            if (doubt.active()) {
                chat_next_ms_ = 0;
                doubt.step(*closer, now_ms());
                if (doubt.active()) continue;   // nothing else while the line may be open
            }
            if (chat_stop_ || stopped_self || chat_oneshot_) break;
            if (!settings.enabled || settings.lines.empty()) {
                chat_next_ms_ = 0;
                if (settings.enabled) tell_now_and_then("Danh sách câu đang trống.");
                continue;
            }
            chat_next_ms_ = schedule.due_at();
            const std::uint64_t now = now_ms();
            if (!schedule.due(now)) continue;

            const int index = schedule.turn();
            const int count = static_cast<int>(settings.lines.size());
            const ChatText line = prepare_chat(settings.lines[static_cast<std::size_t>(index)],
                                               settings.method, settings.strip_diacritics,
                                               settings.max_length);
            if (line.empty()) {
                // Passed over for the next one, unless none of them can be typed.
                bool any = false;
                for (const std::string& l : settings.lines)
                    any = any || !prepare_chat(l, settings.method, settings.strip_diacritics,
                                               settings.max_length).empty();
                tell_now_and_then("Câu số " + std::to_string(index + 1) + " không còn chữ nào gõ được.");
                if (any) schedule.skip(now);
                else schedule.postpone(now, 30'000);
                continue;
            }
            // "Chỉ khi chưa có tổ đội": the party HUD looked at first, before
            // anything is held or pressed (simple_chat_alone.cpp).
            if (chat_alone_put_off(settings, index, store, schedule, now)) continue;
            if (chat_doubt_) {
                // Somebody else's doubt, left by a worker that was stopped:
                // nothing typed until the line is seen shut.
                schedule.postpone(now, 5000);
                tell_now_and_then("Ô chat có thể đang mở — chờ thấy ô chat đóng rồi mới chat.");
                continue;
            }
            // Never two lines closer together than the shortest interval --
            // a Gửi thử just before the run began among them.
            const std::uint64_t said_at = last_said_ms();
            const std::uint64_t floor_ms = ChatSettings::kMinInterval * 1000ull;
            if (said_at != 0 && now < said_at + floor_ms) {
                schedule.postpone(now, said_at + floor_ms - now);
                continue;
            }

            const ChatSender::Result r = attempt(line, false);
            const std::uint64_t end = now_ms();
            note_doubt(r);
            if (r.not_now) {
                schedule.postpone(end, r.wait_ms);
                tell_now_and_then(r.trouble);
            } else if (r.sent) {
                schedule.sent(end);
                ++chat_sent_;
                put_off_in_row = 0;
                remember_said(line.typed);
                GB_INFO("Chat: gửi câu %d/%d (%s): \"%s\"", index + 1, count,
                        chat_method_name(settings.method), line.typed.c_str());
                say_chat("Đã gửi câu " + std::to_string(index + 1) + "/" + std::to_string(count) +
                         ": \"" + line.typed + "\"");
                if (!r.trouble.empty()) tell(r.trouble);
                // Sent, and then would not close: stopped all the same -- the
                // line open is what the fighting keys would be typed into.
                if (r.stop) {
                    tell("Tự chat đã dừng.");
                    stopped_self = true;
                }
            } else if (r.aborted && !r.stop) {
                // Ended part way for something that is no fault of the line's --
                // HP, a dialog, the game losing the front -- and rubbed out. Its
                // turn kept and tried again soon; not a failure, or a long fight
                // would stop Tự chat by itself. Several in a row wait longer.
                ++put_off_in_row;
                const std::uint64_t wait_ms =
                    put_off_in_row >= 3 ? 60'000 : std::max<std::uint64_t>(r.wait_ms, 3000);
                schedule.postpone(end, wait_ms);
                tell_now_and_then("Câu " + std::to_string(index + 1) + " để sau: " + r.trouble);
            } else {
                // Its turn kept, tried again after a pause; and stopped for good
                // after a few in a row, which is not bad luck.
                schedule.failed(end, 30'000);
                tell("Không gửi được câu " + std::to_string(index + 1) + ": " + r.trouble +
                     bar_numbers(r.last));
                if (r.stop) {
                    tell("Tự chat đã dừng.");
                    stopped_self = true;
                } else if (schedule.failures() >= settings.max_failures) {
                    tell(std::to_string(schedule.failures()) +
                         " lần liền không gửi được câu chat — Tự chat đã dừng.");
                    alert("chat_stopped", alert_text::chat_stopped(schedule.failures()));
                    stopped_self = true;
                }
            }
            chat_next_ms_ = schedule.due_at();
        }

        // Told to quit -- DỪNG, F12, closing GoBot: the player has taken over.
        // A line still in doubt is handed over rather than waited on: its
        // hold stays on and the executor paused until tick() has stopped
        // everything else that presses keys, and nothing of that starts again
        // until a look shows the line shut (keys_may_start).
        if (chat_quit_ && doubt.active()) {
            doubt.hand_over();
            doubt_held = false;
            ++chat_doubt_gen_;
            chat_doubt_watched_ = false;
            tell("Đã dừng Tự chat, nhưng ô chat trong game có thể vẫn đang mở.");
            chat_handover_ = true;          // last: tick() acts on it from here
        }
        if (!stays()) break;
    }
}

void GuiApp::autodetect_bar(BarSpec& bar, const char* which) {
    if (!bar.valid()) {
        set_status("Give the region a width and height first", 6000);
        return;
    }
    std::uint8_t r, g, b;
    if (!vitals_.autodetect_colour(bar, r, g, b)) {
        set_status(std::string("No strongly coloured pixels inside the ") + which +
                   " region -- is it over the bar?", 7000);
        return;
    }
    bar.r = r; bar.g = g; bar.b = b;
    apply_vitals_config();

    char msg[128];
    std::snprintf(msg, sizeof(msg), "%s colour set to %u, %u, %u", which, r, g, b);
    set_status(msg, 5000);
}

GuiApp::FrameView GuiApp::draw_frame_region(const Frame& f, int x0, int y0,
                                            int w, int h, float scale,
                                            const char* id) {
    FrameView view;
    if (!f.valid() || w <= 0 || h <= 0) return view;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Plotted by hand rather than uploaded as a texture, which keeps the
    // front-end free of any texture lifetime to manage across device resets.
    //
    // One rect per pixel is a quarter of a million draw calls for a full
    // window, which stalls the GUI visibly. Two things keep it cheap:
    //
    //  - a stride, so a view of the whole window never costs more rects than
    //    a budget allows; clicks still map back at full precision, since they
    //    are converted through `scale` and never through the stride;
    //  - one rect per run of identical colour along a row. Game UI is mostly
    //    flat panels and solid bars, so the runs are long and this is where
    //    most of the saving comes from.
    constexpr long long kRectBudget = 50'000;
    int step = 1;
    while ((static_cast<long long>(w) / step) * (h / step) > kRectBudget) ++step;

    for (int y = 0; y < h; y += step) {
        int run_start = 0;
        std::uint32_t run_col = 0;
        bool have_run = false;

        for (int x = 0; x <= w; x += step) {
            std::uint32_t col = 0;
            if (x < w) {
                std::uint8_t r, g, b;
                f.get(x0 + x, y0 + y, r, g, b);
                col = IM_COL32(r, g, b, 255);
            }

            if (have_run && (x >= w || col != run_col)) {
                dl->AddRectFilled(
                    ImVec2(origin.x + run_start * scale, origin.y + y * scale),
                    ImVec2(origin.x + x * scale, origin.y + (y + step) * scale),
                    run_col);
                have_run = false;
            }
            if (x < w && !have_run) {
                run_start = x;
                run_col = col;
                have_run = true;
            }
        }
    }

    ImGui::InvisibleButton(id, ImVec2(w * scale, h * scale));

    view.drawn = true;
    view.ox = origin.x;
    view.oy = origin.y;
    view.scale = scale;
    view.x0 = x0;
    view.y0 = y0;
    return view;
}

void GuiApp::draw_window_picker_inline() {
    if (!window_list_loaded_) {
        windows_ = InputManager::enumerate_windows();
        window_list_loaded_ = true;
    }

    const bool have = input_.window_valid();

    // Open by default until something is selected, then collapse out of the
    // way: it is a one-off step, but nothing works before you do it.
    ImGui::SetNextItemOpen(!have, ImGuiCond_Once);
    char header[256];
    if (have)
        std::snprintf(header, sizeof(header), "Game window:  %s###win",
                      app_config_.window_title.c_str());
    else
        std::snprintf(header, sizeof(header), "Pick the game window###win");

    if (!have) ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    const bool open = ImGui::CollapsingHeader(header);
    if (!have) ImGui::PopStyleColor();
    if (!open) return;

    ImGui::Indent();
    if (ImGui::Button("Refresh list")) {
        windows_ = InputManager::enumerate_windows();
        window_list_loaded_ = true;
    }
    ImGui::SameLine();
    text_dim("Keys are only ever sent to the window you choose here.");

    if (ImGui::BeginTable("##winpick", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
            ImVec2(0, 140))) {
        ImGui::TableSetupColumn("Title");
        ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < windows_.size(); ++i) {
            const auto& w = windows_[i];
            // Some windows have no title -- the game's launcher is one of
            // them -- and a blank row is not pickable in any useful sense.
            const std::string shown =
                w.title.empty() ? "(untitled " + w.window_class + ")" : w.title;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char id[600];
            std::snprintf(id, sizeof(id), "%s##wp%zu", shown.c_str(), i);
            if (ImGui::Selectable(id, w.hwnd == input_.target_window(),
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                input_.set_target_window(w.hwnd);
                app_config_.window_title = w.title;
                app_config_.process_name = w.process;
                apply_vitals_config();
                set_status("Target window: " + shown);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(w.process.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Unindent();
}

void GuiApp::draw_vitals_panel() {
    ImGui::TextWrapped(
        "Reads your HP and MP by measuring how full the bars on screen are. "
        "No packet decoding and nothing poked into the game -- the numbers are "
        "already drawn, so counting coloured pixels is enough. That is what "
        "lets a potion key fire because you are hurt rather than because a "
        "timer went off.");
    ImGui::Dummy(ImVec2(0, 6));

    draw_window_picker_inline();
    ImGui::Dummy(ImVec2(0, 4));

    // ---- run control ----
    ImGui::PushStyleColor(ImGuiCol_Button, vitals_running_ ? kRed : kGreen);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    if (ImGui::Button(vitals_running_ ? "STOP READING" : "START READING", ImVec2(200, 34))) {
        if (vitals_running_) stop_vitals(); else start_vitals();
    }
    ImGui::PopStyleColor(2);

    const float hp = vitals_.hp_percent();
    const float mp = vitals_.mp_percent();
    const auto  vs = vitals_.stats();

    ImGui::SameLine();
    ImGui::BeginGroup();
    if (!vitals_running_) {
        text_dim("not reading");
    } else if (vs.capture_from_screen) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("capture falling back to screen -- keep the game unobscured");
        ImGui::PopStyleColor();
    } else {
        text_dim("%llu frames read, %llu failed",
                 static_cast<unsigned long long>(vs.reads),
                 static_cast<unsigned long long>(vs.failures));
    }
    ImGui::EndGroup();

    // ---- readout ----
    //
    // The figures the game prints across its bars, where they could be read,
    // and the percentage either way. Saying which of the two is on show
    // matters: measuring the bar is known to read a nearly-full bar as
    // two-thirds full, because the caption is painted over the end of the
    // fill, so a percentage with no number beside it is the one to distrust.
    ImGui::Dummy(ImVec2(0, 6));
    const auto vals = vitals_.values();
    const struct {
        const char* name; float pct; ImVec4 col; int value; int max;
    } kShow[] = {
        { "HP", hp, ImVec4(0.85f, 0.25f, 0.25f, 1.0f),
          vals.have_hp() ? vals.hp : -1, vals.hp_max },
        { "MP", mp, ImVec4(0.25f, 0.45f, 0.90f, 1.0f),
          vals.have_mp() ? vals.mp : -1, vals.mp_max },
    };
    for (const auto& s : kShow) {
        ImGui::Text("%s", s.name);
        ImGui::SameLine(40);
        if (s.pct < 0.0f) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted("-- no reading --");
            ImGui::PopStyleColor();
        } else {
            char lbl[48];
            if (s.value >= 0)
                std::snprintf(lbl, sizeof(lbl), "%d / %d   %.0f%%",
                              s.value, s.max, s.pct);
            else
                std::snprintf(lbl, sizeof(lbl), "%.0f%%", s.pct);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, s.col);
            ImGui::ProgressBar(s.pct / 100.0f, ImVec2(300, 20), lbl);
            ImGui::PopStyleColor();
        }
    }

    if (vitals_running_ && !vitals_.has_font()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped(
            "No glyph set installed, so these are measured from the length of "
            "the coloured part. The game prints its figures over the bars, "
            "which paints out the end of the fill -- expect a nearly-full bar "
            "to read short.");
        ImGui::PopStyleColor();
    } else if (vitals_running_ && vs.reads > 20 &&
               vs.number_reads * 4 < vs.reads * 3) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped(
            "Only %llu of %llu looks could read the printed figures. That "
            "usually means the bar geometry is wrong -- press FIND THE BARS.",
            static_cast<unsigned long long>(vs.number_reads),
            static_cast<unsigned long long>(vs.reads));
        ImGui::PopStyleColor();
    }

    // ---- bar setup ----
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("Where the bars are");

    Frame frame;
    const bool have_frame = vitals_.latest_frame(frame) && frame.valid();

    // The one button that should be enough. Typing eight coordinates is the
    // step that makes this hard to use, and a bar is a shape a machine can
    // recognise: a long flat run of one saturated colour.
    ImGui::BeginDisabled(!have_frame);
    if (ImGui::Button("FIND THE BARS", ImVec2(200, 32))) {
        BarSpec hp_found = macro_config_.hp_bar;
        BarSpec mp_found = macro_config_.mp_bar;
        const int n = VitalsReader::find_bars(frame, hp_found, mp_found);
        if (n == 0) {
            set_status("Could not find either bar -- is the game visible and "
                       "not covered by this window?", 8000);
        } else {
            macro_config_.hp_bar = hp_found;
            macro_config_.mp_bar = mp_found;
            apply_vitals_config();
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "Found %d bar%s -- check the percentages against the game",
                          n, n == 1 ? "" : "s");
            set_status(msg, 7000);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextWrapped(
        "Press this with the game on screen and your HP and MP full -- a full "
        "bar is the only one whose length is certain. Then check the two "
        "percentages above against the game, and Save.");

    text_dim("Coordinates are inside the game window, not the screen, so they hold "
             "when you move it. Adjust by hand below if the automatic answer is off.");

    struct BarUi { const char* name; BarSpec* spec; bool* picking; ImVec4 col; };
    BarSpec& hp_spec = macro_config_.hp_bar;
    BarSpec& mp_spec = macro_config_.mp_bar;
    const BarUi kBars[] = {
        { "HP", &hp_spec, &picking_hp_, ImVec4(1.0f, 0.35f, 0.35f, 1.0f) },
        { "MP", &mp_spec, &picking_mp_, ImVec4(0.40f, 0.60f, 1.0f, 1.0f) },
    };

    bool changed = false;
    for (const auto& b : kBars) {
        ImGui::PushID(b.name);
        ImGui::PushStyleColor(ImGuiCol_Text, b.col);
        ImGui::TextUnformatted(b.name);
        ImGui::PopStyleColor();
        ImGui::SameLine(40);

        ImGui::SetNextItemWidth(70);
        changed |= ImGui::InputInt("x", &b.spec->x, 0, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        changed |= ImGui::InputInt("y", &b.spec->y, 0, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        changed |= ImGui::InputInt("w", &b.spec->width, 0, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        changed |= ImGui::InputInt("h", &b.spec->height, 0, 0);

        ImGui::SameLine();
        float col[3] = { b.spec->r / 255.0f, b.spec->g / 255.0f, b.spec->b / 255.0f };
        if (ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs)) {
            b.spec->r = static_cast<std::uint8_t>(col[0] * 255.0f);
            b.spec->g = static_cast<std::uint8_t>(col[1] * 255.0f);
            b.spec->b = static_cast<std::uint8_t>(col[2] * 255.0f);
            changed = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Grab colour")) {
            autodetect_bar(*b.spec, b.name);
            changed = true;
        }
        help_marker("Takes the most common strongly-coloured pixel inside the "
                    "region. Set the region over a bar that is not empty, then "
                    "press this.");

        ImGui::SameLine();
        if (ImGui::Button(*b.picking ? "click it..." : "Click to place")) {
            *b.picking = !*b.picking;
            if (*b.picking) {
                // One at a time, or the first click would set both.
                for (const auto& o : kBars) if (o.spec != b.spec) *o.picking = false;
            }
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        changed |= ImGui::SliderInt("tol", &b.spec->tolerance, 5, 140);

        if (have_frame && b.spec->valid()) {
            const float pct = VitalsReader::measure(frame, *b.spec);
            ImGui::SameLine();
            if (pct < 0.0f) text_dim("  (unreadable)");
            else            ImGui::Text("  = %.0f%%", pct);
        }
        ImGui::PopID();
    }

    if (changed) apply_vitals_config();

    // ---- picture ----
    ImGui::Dummy(ImVec2(0, 6));
    if (!have_frame) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(vitals_running_
            ? "Waiting for the first frame..."
            : "Press START READING to see the game window here.");
        ImGui::PopStyleColor();
        return;
    }

    ImGui::Checkbox("Whole window", &vitals_show_full_frame_);
    help_marker("Off shows just the top-left corner magnified, which is where "
                "these bars normally live. On shows everything, shrunk, for "
                "finding them if they are somewhere else.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderInt("zoom", &vitals_zoom_, 1, 4);

    // The corner view is magnified so individual pixels are clickable; the
    // whole-window view is shrunk to fit whatever the panel is wide.
    int vw, vh;
    float scale;
    if (vitals_show_full_frame_) {
        vw = frame.width;
        vh = frame.height;
        const float avail = std::max(240.0f, ImGui::GetContentRegionAvail().x - 20.0f);
        scale = std::min(1.0f, avail / static_cast<float>(vw));
    } else {
        vw = std::min(frame.width, 360);
        vh = std::min(frame.height, 120);
        scale = static_cast<float>(vitals_zoom_);
    }

    const FrameView view = draw_frame_region(frame, 0, 0, vw, vh, scale, "##vitview");
    if (!view.drawn) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& b : kBars) {
        if (!b.spec->valid()) continue;
        const ImVec2 a(view.ox + b.spec->x * view.scale,
                       view.oy + b.spec->y * view.scale);
        const ImVec2 c(view.ox + (b.spec->x + b.spec->width) * view.scale,
                       view.oy + (b.spec->y + b.spec->height) * view.scale);
        dl->AddRect(a, c, ImGui::GetColorU32(b.col), 0.0f, 0, 1.5f);
    }

    // Click places the region's top-left and gives it a usable default size,
    // so one click gets you most of the way rather than four number boxes.
    if (ImGui::IsItemClicked()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int px = static_cast<int>((m.x - view.ox) / view.scale);
        const int py = static_cast<int>((m.y - view.oy) / view.scale);

        for (const auto& b : kBars) {
            if (!*b.picking) continue;
            *b.picking = false;

            if (!b.spec->valid()) {
                b.spec->width = 160;
                b.spec->height = 8;
            }
            // Centre the band vertically on the click: you aim at the middle
            // of a bar, not at its top edge.
            b.spec->x = px;
            b.spec->y = py - b.spec->height / 2;

            std::uint8_t r, g, bl;
            if (vitals_.sample_pixel(px, py, r, g, bl)) {
                b.spec->r = r; b.spec->g = g; b.spec->b = bl;
            }
            apply_vitals_config();

            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "%s bar at %d,%d -- drag the w box until the number matches "
                          "the game", b.name, b.spec->x, b.spec->y);
            set_status(msg, 7000);
            break;
        }
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("Save", ImVec2(140, 0))) save_macro_config();
    ImGui::SameLine();
    text_dim("Saved with the macro, in configs/macro.ini.");
}

// ------------------------------------------------------------------ radar

namespace {

/// The game's client area in screen coordinates.
bool game_client_rect(void* hwnd_raw, RECT& out) {
    HWND hwnd = static_cast<HWND>(hwnd_raw);
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    POINT tl{ rc.left, rc.top }, br{ rc.right, rc.bottom };
    if (!ClientToScreen(hwnd, &tl) || !ClientToScreen(hwnd, &br)) return false;
    out = RECT{ tl.x, tl.y, br.x, br.y };
    return out.right > out.left && out.bottom > out.top;
}

} // namespace

float GuiApp::overlap_fraction() const {
    RECT game{};
    if (!game_client_rect(input_.target_window(), game)) return 0.0f;

    HWND self = static_cast<HWND>(main_hwnd_);
    RECT mine{};
    if (!self || !GetWindowRect(self, &mine)) return 0.0f;
    if (IsIconic(self)) return 0.0f;

    RECT hit{};
    if (!IntersectRect(&hit, &game, &mine)) return 0.0f;

    const double area = static_cast<double>(game.right - game.left) *
                        (game.bottom - game.top);
    if (area <= 0.0) return 0.0f;
    const double over = static_cast<double>(hit.right - hit.left) *
                        (hit.bottom - hit.top);
    return static_cast<float>(over / area);
}

void GuiApp::move_clear_of_game() {
    RECT game{};
    if (!game_client_rect(input_.target_window(), game)) {
        set_status("No game window to move clear of", 5000);
        return;
    }
    HWND self = static_cast<HWND>(main_hwnd_);
    RECT mine{};
    if (!self || !GetWindowRect(self, &mine)) return;

    const int w = mine.right - mine.left;
    const int h = mine.bottom - mine.top;
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);

    // The free strips either side of the game, and the one below it. Each is
    // tried at the current size first, then as a resize into whatever room the
    // widest strip actually has -- a 1180px window simply does not fit beside
    // a 1024px game on a 1920px screen, and moving it without shrinking it
    // just puts the overlap on the other side.
    struct Spot { int x, y, room; };
    const Spot spots[] = {
        { 0, static_cast<int>(mine.top), static_cast<int>(game.left) },
        { static_cast<int>(game.right), static_cast<int>(mine.top),
          screen_w - static_cast<int>(game.right) },
        { static_cast<int>(mine.left), static_cast<int>(game.bottom),
          screen_h - static_cast<int>(game.bottom) },
    };

    int best_room = 0, best_x = 0, best_y = 0;
    for (const Spot& s : spots) {
        if (s.room <= best_room) continue;
        best_room = s.room; best_x = s.x; best_y = s.y;
    }

    if (best_room >= 360) {
        const int want_w = std::min(w, best_room - 8);
        const int want_h = std::min(h, screen_h - best_y - 8);
        SetWindowPos(self, nullptr, best_x, best_y,
                     std::max(360, want_w), std::max(320, want_h),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    set_status(overlap_fraction() < 0.02f
                   ? "Moved clear of the game window"
                   : "Not enough room beside the game -- move or resize the game itself",
               7000);
}

void GuiApp::start_detector() {
    if (detector_running_) return;
    if (!input_.window_valid()) {
        set_status("Pick the game window first", 6000);
        return;
    }
    int w = 0, h = 0;
    if (input_.window_size(w, h) && w > 0)
        minimap_.set_layout(MinimapTracker::default_layout(w, h));

    detector_.set_target_window(input_.target_window());
    detector_.set_tuning(detect_tuning_);
    detector_.set_minimap(&minimap_);
    detector_.set_camera(&camera_);

    // Read the name plates rather than guessing at them, when the game's own
    // font is available to read them with. Loaded once, on the first start.
    if (!font_.loaded()) {
        const std::string dir = GlyphFont::default_dir();
        if (!dir.empty() && font_.load(dir))
            set_status("Reading name plates with the game's own font", 5000);
    }
    if (font_.loaded()) detector_.set_font(&font_);
    detector_.set_character_name(hunt_config_.character_name);

    // Track the scenery over the play field only. The HUD does not scroll, and
    // matching it against itself would say the view never moves. Its own
    // default is the middle of whatever size the window is; a box measured
    // on a 1024x768 client sat off-centre on a 1280x720 one.
    camera_.set_play_area(ScreenRect{});
    camera_.reset();
    anchor_.reset();

    detector_.start();
    detector_running_ = true;
}

/// The thing worth knowing before anything that presses a key.
///
/// Windows refuses input injected by a process running at a lower integrity
/// level than the window that currently has the foreground. This game is
/// launched from an elevated shell, so a GoBot started normally sends keys
/// that go nowhere: SendInput reports success, the macro runs perfectly, and
/// the game receives nothing.
///
/// That is indistinguishable from an anticheat blocking macros until somebody
/// measures it, and it was mistaken for one here. What settles it is injecting
/// with an ordinary window in front instead of the game's.
void GuiApp::draw_input_warning() {
    if (input_injection_ok_) return;

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    ImGui::TextUnformatted("Keystrokes sent by GoBot are not reaching the game.");
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "The game is running with higher privileges than GoBot, and Windows "
        "will not let a lower one send input while a higher one is in front. "
        "Nothing is wrong with the keys or the macro -- they simply go nowhere. "
        "Restarting GoBot as Administrator fixes it, and packet capture wants "
        "that anyway.");

    if (ImGui::Button("Restart as Administrator", ImVec2(230, 0))) relaunch_elevated();
    ImGui::SameLine();
    if (ImGui::Button("Check again", ImVec2(130, 0)))
        input_injection_ok_ = InputManager::injection_works();
    ImGui::Dummy(ImVec2(0, 4));
}

/// Start ourselves again with elevation and bow out.
///
/// Not a privilege trick: Windows shows its usual prompt and the user decides.
/// It is here because the alternative is explaining integrity levels to
/// somebody who only wants the macro to press a key.
void GuiApp::relaunch_elevated() {
    // It starts a program -- and quits this one -- so it asks the switch like
    // every other way of starting one.
    if (refuse_real_input("restart GoBot as administrator")) {
        set_status("Not restarted -- this copy of GoBot runs with no real input", 6000);
        return;
    }
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        set_status("Could not work out where GoBot is, so cannot restart it", 6000);
        return;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = path;
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        // Almost always the user declining the prompt, which is their call.
        set_status("Not restarted -- the elevation prompt was declined", 6000);
        return;
    }
    GB_INFO("Restarting elevated so input can reach the game");
    quit_ = true;
}

void GuiApp::save_map_book() {
    const std::string path = exe_relative("configs/maps.ini");
    set_status(map_book_.save(path) ? "Saved " + path
                                    : "FAILED to save " + path, 6000);
}

/// A plan view of what is around the character.
///
/// The same positions as the boxes on the picture above, with the artwork
/// taken away. The character sits in the middle and the monsters fall where
/// they are relative to it, which is the one question the hunter is answering
/// and the one the picture makes you squint to see.
void GuiApp::draw_local_radar(const Nameplate* me, const std::vector<Nameplate>& plates,
                              const Nameplate* target, int frame_w, int frame_h) {
    const ImVec2 size(300.0f, 190.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      IM_COL32(16, 18, 22, 255));
    dl->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                IM_COL32(70, 76, 90, 255));
    ImGui::InvisibleButton("##localradar", size);

    if (!me || frame_w <= 0) {
        const ImVec2 at(origin.x + 12.0f, origin.y + size.y * 0.5f - 8.0f);
        dl->AddText(at, IM_COL32(150, 150, 160, 255), "waiting for your character");
        return;
    }

    // Everything within half a screen either way, drawn to fit the box.
    const float span_x = static_cast<float>(frame_w) * 0.5f;
    const float span_y = static_cast<float>(frame_h) * 0.5f;
    const float sx = size.x * 0.5f / span_x;
    const float sy = size.y * 0.5f / span_y;

    const ImVec2 centre(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
    dl->AddLine(ImVec2(origin.x, centre.y), ImVec2(origin.x + size.x, centre.y),
                IM_COL32(50, 56, 68, 255));
    dl->AddLine(ImVec2(centre.x, origin.y), ImVec2(centre.x, origin.y + size.y),
                IM_COL32(50, 56, 68, 255));

    // The band the hunter will attack from, so "reach" is a thing you can see
    // rather than a number you guess at: up and down apart, as a style sets
    // them. Inside it, shaded, the distance kept with "Giữ khoảng cách" --
    // or a style's minimum, which is that distance once it is ticked.
    const float reach = hunt_config_.reach * sx;
    const float up = hunt_config_.up() * sy;
    const float down = hunt_config_.down() * sy;
    dl->AddRect(ImVec2(centre.x - reach, centre.y - up),
                ImVec2(centre.x + reach, centre.y + down),
                IM_COL32(255, 190, 40, 70));
    if (hunt_config_.keep_distance || hunt_config_.min_distance > 0) {
        const float keep = static_cast<float>(Hunter::keep_away_px(hunt_config_)) * sx;
        dl->AddRectFilled(ImVec2(centre.x - keep, centre.y - up),
                          ImVec2(centre.x + keep, centre.y + down),
                          IM_COL32(255, 120, 40, 30));
    }

    for (const Nameplate& n : plates) {
        if (&n == me) continue;
        const float px = centre.x + (n.x() - me->x()) * sx;
        const float py = centre.y + (n.y() - me->y()) * sy;
        if (px < origin.x || px > origin.x + size.x ||
            py < origin.y || py > origin.y + size.y) continue;

        const bool is_target = target && n.x() == target->x() && n.y() == target->y();
        const ImU32 col = n.is_player ? IM_COL32(150, 150, 255, 255)
                        : is_target   ? IM_COL32(255, 190, 40, 255)
                                      : IM_COL32(90, 255, 120, 255);
        dl->AddCircleFilled(ImVec2(px, py), is_target ? 6.0f : 4.0f, col);
        if (is_target)
            dl->AddLine(centre, ImVec2(px, py), IM_COL32(255, 190, 40, 160), 1.5f);
    }

    dl->AddCircleFilled(centre, 5.0f, IM_COL32(90, 200, 255, 255));
    dl->AddCircle(centre, 8.0f, IM_COL32(90, 200, 255, 180), 0, 1.5f);
}

void GuiApp::save_places() {
    const std::string path = exe_relative("configs/waypoints.ini");
    set_status(places_.save(path) ? "Saved " + path
                                     : "FAILED to save " + path, 6000);
}

void GuiApp::draw_movement_section() {
    const CameraTracker::Fix fx = camera_.fix();
    const std::uint64_t map = minimap_.fingerprint();

    Nameplate me;
    const bool have_me = detector_.player(me);

    double wx = 0.0, wy = 0.0;
    const bool have_pos = have_me && fx.tracking;
    if (have_pos) camera_.to_world(me.x(), me.y(), wx, wy);

    // ---- where we are ----
    if (!detector_running_) {
        text_dim("Start watching to track your position.");
    } else if (!fx.tracking) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("Lost track of the view -- the scenery stopped matching.");
        ImGui::PopStyleColor();
        text_dim("Loading screens and full-screen effects do that. It picks up "
                 "again on its own; positions taken across the gap are off by "
                 "however far you moved during it.");
    } else if (!have_me) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("Tracking the view, but cannot see your character.");
        ImGui::PopStyleColor();
    } else {
        ImGui::Text("Position  %.0f, %.0f", wx, wy);
        ImGui::SameLine();
        text_dim("   view moved %.0f px so far, match %.0f%%",
                 camera_.travelled(), fx.confidence * 100.0f);
    }

    if (!anchor_.anchored()) {
        text_dim("Origin not pinned yet: walk into the left-hand edge of the map "
                 "once and these numbers become the same tomorrow as today.");
    }

    // ---- saved places ----
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SetNextItemWidth(170);
    ImGui::InputText("##wpname", place_name_, sizeof(place_name_));
    ImGui::SameLine();
    ImGui::BeginDisabled(!have_pos || map == 0 || place_name_[0] == '\0');
    if (ImGui::Button("Mark here")) {
        SavedPlace w;
        w.name = place_name_;
        w.x = wx;
        w.y = wy;
        w.map = map;
        places_.add(w);
        save_places();
    }
    ImGui::EndDisabled();
    if (map == 0) {
        ImGui::SameLine();
        text_dim("(waiting for the map to settle)");
    }

    const std::vector<SavedPlace> here = places_.for_map(map);
    const Navigator::Status ns = navigator_->status();

    if (here.empty()) {
        text_dim("No places saved on this map yet.");
    } else if (ImGui::BeginTable("##wps", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("At", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Away", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < here.size(); ++i) {
            const SavedPlace& w = here[i];
            ImGui::PushID(static_cast<int>(2000 + i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            const bool is_target = navigator_->running() && ns.target == w.name;
            if (is_target) ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextUnformatted(w.name.c_str());
            if (is_target) ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            ImGui::Text("%.0f, %.0f", w.x, w.y);

            ImGui::TableNextColumn();
            if (have_pos) ImGui::Text("%.0f", w.x - wx);
            else          text_dim("-");

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!have_pos);
            if (ImGui::SmallButton("Go") && keys_may_start()) navigator_->go_to(w);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Forget")) {
                places_.remove(map, w.name);
                save_places();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ---- going places ----
    ImGui::Dummy(ImVec2(0, 4));
    // The walk's jump key, hunt.ini's [Move] one, which nothing here sets: a
    // file from before it shipped as Up has Space, the game's attack, and a
    // walk up to a place above would attack instead. Said; never changed in
    // the player's file.
    {
        const std::string walk = JumpCheck::walk_key_warning_vi(nav_config_.jump_key, hunt_config_.attack_key);
        if (!walk.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextWrapped("%s", walk.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::BeginDisabled(here.size() < 2 || !have_pos);
    if (ImGui::Button("Patrol these", ImVec2(150, 0)) && keys_may_start())
        navigator_->patrol(here);
    ImGui::EndDisabled();
    help_marker("Walks back and forth along the saved places, in order. With "
                "hunting on as well, it fights whatever it meets on the way.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!navigator_->running());
    if (ImGui::Button("Stop moving", ImVec2(150, 0))) navigator_->stop();
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (navigator_->running() || ns.state != Navigator::State::Idle) {
        const bool good = ns.state == Navigator::State::Walking ||
                          ns.state == Navigator::State::Arrived;
        ImGui::PushStyleColor(ImGuiCol_Text, good ? kGreen : kAmber);
        ImGui::Text("%s", Navigator::to_string(ns.state));
        ImGui::PopStyleColor();
        if (navigator_->running() && !ns.target.empty()) {
            ImGui::SameLine();
            text_dim("-> %s, %.0f px away", ns.target.c_str(), ns.distance);
        }
    }

    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Close enough (px)", &nav_config_.arrive_tolerance, 8, 200))
        navigator_->set_config(nav_config_);
    ImGui::SameLine();
    int give_up = static_cast<int>(nav_config_.leg_timeout_ms);
    ImGui::SetNextItemWidth(180);
    if (ImGui::SliderInt("Give up on a leg (ms)", &give_up, 3000, 60000)) {
        nav_config_.leg_timeout_ms = static_cast<std::uint32_t>(give_up);
        navigator_->set_config(nav_config_);
    }
    help_marker("A wall, a gap or a ledge it cannot climb all look the same from "
                "here. After this long it abandons that leg and moves on.");
}

void GuiApp::draw_map_section() {
    const std::uint64_t fp = minimap_.fingerprint();

    // Pick up the stored name when the map changes under us.
    if (fp != map_fp_) {
        map_fp_ = fp;
        copy_to(map_name_, sizeof(map_name_), map_book_.name_for(fp));
    }

    Frame title, body;
    const bool have_title = minimap_.title_image(title) && title.valid();
    const bool have_body  = minimap_.body_image(body) && body.valid();

    if (!have_body) {
        text_dim("No minimap yet.");
        return;
    }

    ImGui::BeginGroup();
    // The game's own name bar, shown as pixels. Reading it would mean
    // recognising Vietnamese text in a bitmap font; showing it costs nothing
    // and the user can read it perfectly well.
    if (have_title) draw_frame_region(title, 0, 0, title.width, title.height, 1.0f, "##mmtitle");
    draw_frame_region(body, 0, 0, body.width, body.height, 1.0f, "##mmbody");
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Map");
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputText("##mapname", map_name_, sizeof(map_name_)) && fp != 0)
        map_book_.set_name(fp, map_name_);
    ImGui::SameLine();
    if (ImGui::Button("Save##map")) {
        if (fp != 0) map_book_.set_name(fp, map_name_);
        save_map_book();
    }

    text_dim("id %016llx", static_cast<unsigned long long>(fp));
    ImGui::TextWrapped(
        "Recognised by what the minimap looks like, so a name typed once here "
        "comes back every time you return. %zu map%s named so far.",
        map_book_.size(), map_book_.size() == 1 ? "" : "s");

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextWrapped(
        "Your position is not drawn on this picture: the game's minimap carries "
        "no marker for the character, and finding the view inside it by matching "
        "the artwork does not work -- the correlation is flat, which is what no "
        "signal looks like. The plan view beside it is the real one.");
    ImGui::EndGroup();
}

void GuiApp::stop_detector() {
    if (!detector_running_) return;
    stop_hunt();                  // nothing to hunt with the eyes shut
    if (navigator_) navigator_->stop();   // nor anywhere to walk to
    detector_.stop();
    detector_running_ = false;
}

void GuiApp::start_hunt() {
    if (hunting_) return;
    if (!input_.window_valid()) {
        set_status("Pick the game window first", 6000);
        return;
    }
    // Your character is found by its name and nothing else: a plate with bars
    // may be yours or another player's. Started without one, the hunter only
    // stood there -- after F11 had brought the game forward -- or, before the
    // detector stopped taking the nearest player for you, walked and hit
    // relative to a stranger.
    if (!hunt_named()) {
        simple_note("Chưa gõ tên nhân vật nên chưa bật Tự đánh quái. Gõ tên ở phần \"Trước khi "
                    "bắt đầu\" của thẻ Đánh quái.", 12000);
        if (!simple_view_)
            set_status("Type your character's name first: the hunter finds you by it", 12000);
        GB_WARN("Hunting not started: no character name typed, so there is no 'you' to walk from");
        return;
    }
    if (!keys_may_start()) return;
    if (!detector_running_) start_detector();
    if (!detector_running_) return;

    hunter_->set_config(hunt_config_);

    // Same reason as the macro: the hunter refuses to send anything to an
    // unfocused window, and clicking the button is what unfocused it.
    //
    // Done after the detector is up, so the first frames it captures are of
    // the game in front rather than of this window on top of it.
    if (hunt_config_.focus_on_start) focus_game_to_start("hunting");

    hunter_->start();
    hunting_ = true;
}

void GuiApp::stop_hunt() {
    if (!hunting_) return;
    hunter_->stop();
    hunting_ = false;
}

void GuiApp::toggle_hunt() {
    if (hunting_) stop_hunt(); else start_hunt();
}

void GuiApp::save_hunt_config() {
    const std::string path = exe_relative("configs/hunt.ini");
    // The detector's tuning goes with it, into [Detect]: what the Radar tab
    // edits by hand is kept, and a reset one is kept as following the window.
    if (hunt_config_.save(path) && detect_tuning_.save(path)) {
        hunter_->set_config(hunt_config_);
        set_status("Saved " + path);
    } else {
        set_status("FAILED to save " + path, 8000);
    }
}

/// The hunter's settings, shared by the Attack feature and the Radar tab.
void GuiApp::draw_hunt_settings() {
    // ---- hunting settings ----
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("Hunting");

    bool changed = false;

    // How the bot sees its own character. Nothing else on screen will do it:
    // every creature is found by its health bar, and a plate with bars may be
    // yours or another player's, so without a name here the bot knows where
    // the monsters are and not where it is standing.
    {
        char name[64];
        copy_to(name, sizeof(name), hunt_config_.character_name);
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputText("Your character's name", name, sizeof(name))) {
            hunt_config_.character_name = name;
            detector_.set_character_name(hunt_config_.character_name);
            changed = true;
        }
        help_marker("Exactly as the game writes it under your feet. The bot "
                    "reads the screen for the whole name -- the bars under a "
                    "name say a player stands there, not which one, and a "
                    "longer name with yours inside it is not you. Without "
                    "this it can see the monsters but not itself.");

        if (!font_.loaded()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "(no glyph set installed -- cannot read names)");
        } else if (hunt_config_.character_name.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "(not set)");
        }
        ImGui::Dummy(ImVec2(0, 4));
    }

    char key[24];
    copy_to(key, sizeof(key), InputManager::key_name(hunt_config_.attack_key));
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputText("Attack key", key, sizeof(key))) {
        if (const std::uint32_t vk = InputManager::parse_key(key)) {
            hunt_config_.attack_key = vk;
            changed = true;
        }
    }
    help_marker("The skill the hunter throws when it is in range. Your other "
                "skills keep running on the Macro tab's timers alongside it.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    changed |= ImGui::SliderInt("Reach (px)", &hunt_config_.reach, 40, 400);
    help_marker("How close it has to be before attacking instead of walking. "
                "Set this to about how far your attack actually reaches -- the "
                "boxes in the view above are in the same pixels.");

    // The band apart, above and below, as GoC has it: following
    // height_tolerance both ways until one of them is moved.
    {
        int up = hunt_config_.up();
        int down = hunt_config_.down();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        bool band = ImGui::SliderInt("Up (px)", &up, 0, 400);
        help_marker("How far above a monster may be and still count as reachable "
                    "-- GoC's highest hit. Too large and the bot swings at things "
                    "on the platform above. The simple screen's \"Kiểu đánh\" sets "
                    "it with the rest.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        band |= ImGui::SliderInt("Down (px)", &down, 0, 400);
        help_marker("How far below a monster may be and still count as reachable "
                    "-- GoC's lowest hit.");
        if (band) {
            hunt_config_.set_band(up, down);
            changed = true;
        }
    }

    int attack_ms = static_cast<int>(hunt_config_.attack_interval_ms);
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Swing every (ms)", &attack_ms, 200, 3000)) {
        hunt_config_.attack_interval_ms = static_cast<std::uint32_t>(attack_ms);
        changed = true;
    }

    ImGui::SameLine();
    int step_ms = static_cast<int>(hunt_config_.step_ms);
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Step (ms)", &step_ms, 40, 500)) {
        hunt_config_.step_ms = static_cast<std::uint32_t>(step_ms);
        changed = true;
    }

    ImGui::SameLine();
    int give_up = static_cast<int>(hunt_config_.give_up_ms);
    ImGui::SetNextItemWidth(170);
    if (ImGui::SliderInt("Give up after (ms)", &give_up, 1000, 15000)) {
        hunt_config_.give_up_ms = static_cast<std::uint32_t>(give_up);
        changed = true;
    }
    help_marker("A target it cannot reach in this long is abandoned for the next "
                "one. Without it the bot walks into the wall under a ledge for "
                "the rest of the session.");

    changed |= ImGui::Checkbox("Jump at things above you", &hunt_config_.jump_to_reach);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Only while the game has focus##hunt",
                               &hunt_config_.require_focus);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Bring the game to the front when starting##hunt",
                               &hunt_config_.focus_on_start);
    help_marker("Clicking START HUNTING takes focus off the game, and the hunter "
                "will not send anything to an unfocused window -- so without this "
                "it starts and waits forever. Only on the way on: it never grabs "
                "the foreground back while running.");


    // ---- what counts as dead, and what to finish first ----
    {
        int dead_pct = static_cast<int>(hunt_config_.dead_below * 100.0f + 0.5f);
        ImGui::SetNextItemWidth(160);
        if (ImGui::SliderInt("Dead below (%)", &dead_pct, 0, 30)) {
            hunt_config_.dead_below = static_cast<float>(dead_pct) / 100.0f;
            changed = true;
        }
        help_marker("A monster whose health bar reads below this is treated as "
                    "dead: the hunter lets go of it at once and never picks it. "
                    "Not zero, because reading a bar is approximate and an empty "
                    "one can measure slightly above nothing.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Finish the wounded first", &hunt_config_.finish_wounded);
        help_marker("Among monsters it can already reach, go for the most hurt "
                    "instead of the nearest. Kills come sooner, and damage is not "
                    "spread across a whole screen of monsters killing none of them.");
    }

    if (changed) hunter_->set_config(hunt_config_);
}

void GuiApp::draw_radar_panel() {
    ImGui::TextWrapped(
        "Finds monsters by their name plates. Every creature on screen wears a "
        "dark name box with a red health bar under it; players have a blue mana "
        "bar as well, and monsters do not. That one difference is what separates "
        "what to attack from who to leave alone.");
    ImGui::Dummy(ImVec2(0, 6));

    draw_window_picker_inline();
    ImGui::Dummy(ImVec2(0, 4));

    // ---- run control ----
    ImGui::PushStyleColor(ImGuiCol_Button, detector_running_ ? kRed : kGreen);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    if (ImGui::Button(detector_running_ ? "STOP WATCHING" : "START WATCHING", ImVec2(190, 34))) {
        if (detector_running_) stop_detector(); else start_detector();
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, hunting_ ? kRed : ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    if (ImGui::Button(hunting_ ? "STOP HUNTING" : "START HUNTING", ImVec2(190, 34)))
        toggle_hunt();
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    ImGui::BeginGroup();
    const auto ds = detector_.stats();
    if (!detector_running_) {
        text_dim("not watching");
    } else {
        ImGui::Text("%zu monster%s in view", ds.last_mobs, ds.last_mobs == 1 ? "" : "s");
        if (!ds.player_seen) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextUnformatted("cannot see your character");
            ImGui::PopStyleColor();
        } else if (ds.capture_from_screen) {
            text_dim("reading off the screen -- keep the game unobscured");
        } else {
            text_dim("%llu frames", static_cast<unsigned long long>(ds.frames));
        }
    }
    ImGui::EndGroup();

    text_dim("%s toggles hunting, %s is the emergency stop.",
             InputManager::key_name(hunt_config_.toggle_key).c_str(),
             InputManager::key_name(macro_config_.panic_key).c_str());

    // The failure that looks like a bug but is not.
    //
    // This game will not render itself into a bitmap on request, so frames are
    // lifted off the screen -- and whatever is covering the game is what gets
    // lifted. With this window on top of it, the detector is looking at a
    // picture of itself.
    const float covered = overlap_fraction();
    if (covered > 0.02f) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, covered > 0.2f ? kRed : kAmber);
        ImGui::Text("This window is covering %.0f%% of the game.", covered * 100.0f);
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "The game will not draw itself into a bitmap on request, so the "
            "picture comes off the screen -- and anything on top of the game is "
            "what gets read. Move this window off it.");
        if (ImGui::Button("Move GoBot clear", ImVec2(190, 0))) move_clear_of_game();
    }

    // ---- what the hunter is doing ----
    const Hunter::Status hs = hunter_->status();
    if (hunting_) {
        ImGui::Dummy(ImVec2(0, 2));
        const bool good = hs.state == Hunter::State::Attacking ||
                          hs.state == Hunter::State::Approaching;
        ImGui::PushStyleColor(ImGuiCol_Text, good ? kGreen : kAmber);
        ImGui::Text("%s", Hunter::to_string(hs.state));
        ImGui::PopStyleColor();
        if (hs.has_target) {
            ImGui::SameLine();
            text_dim("   target %d px %s, %d px %s   |   %llu attacks, %llu steps",
                     std::abs(hs.dx), hs.dx < 0 ? "left" : "right",
                     std::abs(hs.dy), hs.dy < 0 ? "up" : "down",
                     static_cast<unsigned long long>(hs.attacks),
                     static_cast<unsigned long long>(hs.steps));
        }
    }

    // ---- the view ----
    Frame frame;
    const bool have_frame = detector_.latest_frame(frame) && frame.valid();

    // Plan view and the game's own map, side by side above the picture: this
    // is the answer to "where is everything", and the picture below it is the
    // evidence for that answer.
    ImGui::Dummy(ImVec2(0, 6));
    {
        const std::vector<Nameplate> all = detector_.plates();
        Nameplate me;
        const bool have_me = detector_.player(me);
        const Nameplate* me_ptr = nullptr;
        for (const Nameplate& n : all)
            if (have_me && n.plate.x == me.plate.x && n.plate.y == me.plate.y) me_ptr = &n;

        draw_local_radar(me_ptr, all,
                         (hunting_ && hs.has_target) ? &hs.target : nullptr,
                         have_frame ? frame.width : 0,
                         have_frame ? frame.height : 0);
        ImGui::SameLine();
        ImGui::BeginGroup();
        draw_map_section();
        ImGui::EndGroup();
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (!have_frame) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(detector_running_ ? "Waiting for the first frame..."
                                                 : "Press START WATCHING.");
        ImGui::PopStyleColor();
    } else {
        // Capped well under 1:1. At full size the view pushes every control
        // off the bottom of the panel, and a radar is for seeing where things
        // are, not for reading the game over GoBot's shoulder.
        const float avail = std::min(760.0f,
            std::max(320.0f, ImGui::GetContentRegionAvail().x - 16.0f));
        const float scale = std::min(0.75f, avail / static_cast<float>(frame.width));
        const FrameView view =
            draw_frame_region(frame, 0, 0, frame.width, frame.height, scale, "##radar");

        if (view.drawn) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const auto box = [&](const ScreenRect& r, ImU32 col, float thick) {
                dl->AddRect(ImVec2(view.ox + r.x * view.scale, view.oy + r.y * view.scale),
                            ImVec2(view.ox + r.right() * view.scale,
                                   view.oy + r.bottom() * view.scale),
                            col, 0.0f, 0, thick);
            };

            // Where the detector is allowed to look, as it is for this size
            // of window.
            const DetectorTuning used = detect_tuning_.resolved(frame.width, frame.height);
            if (used.area.valid()) box(used.area, IM_COL32(120, 120, 140, 90), 1.0f);
            if (used.exclude.valid()) box(used.exclude, IM_COL32(150, 90, 90, 90), 1.0f);
            if (used.exclude2.valid()) box(used.exclude2, IM_COL32(150, 90, 90, 90), 1.0f);

            Nameplate me;
            const bool have_me = detector_.player(me);

            for (const Nameplate& n : detector_.plates()) {
                ScreenRect r = n.plate.valid() ? n.plate : n.bar;
                r.y -= 2; r.h = (n.bar.bottom() - r.y) + 2;

                const bool is_me = have_me && n.plate.x == me.plate.x && n.plate.y == me.plate.y;
                const bool is_target = hunting_ && hs.has_target && !n.is_player &&
                                       std::abs(n.x() - hs.target.x()) < 6 &&
                                       std::abs(n.y() - hs.target.y()) < 6;

                if (is_me)            box(r, IM_COL32(90, 200, 255, 255), 2.0f);
                else if (n.is_player) box(r, IM_COL32(150, 150, 255, 200), 1.5f);
                else if (is_target)   box(r, IM_COL32(255, 190, 40, 255), 3.0f);
                else                  box(r, IM_COL32(90, 255, 120, 220), 2.0f);

                // Mark the ground position -- what the hunter actually steers by.
                dl->AddCircleFilled(
                    ImVec2(view.ox + n.x() * view.scale, view.oy + n.y() * view.scale),
                    3.0f, is_me ? IM_COL32(90, 200, 255, 255)
                                : IM_COL32(255, 255, 255, 220));

                // What the plate says, when it could be read. Worth showing:
                // it is the difference between "something is there" and
                // knowing what, and it makes a misread obvious at a glance.
                if (!n.name.empty()) {
                    dl->AddText(ImVec2(view.ox + r.x * view.scale,
                                       view.oy + r.y * view.scale - 14.0f),
                                IM_COL32(255, 255, 255, 230), n.name.c_str());
                }
            }

            // The line the bot is about to walk along.
            if (have_me && hunting_ && hs.has_target) {
                dl->AddLine(ImVec2(view.ox + me.x() * view.scale,
                                   view.oy + me.y() * view.scale),
                            ImVec2(view.ox + hs.target.x() * view.scale,
                                   view.oy + hs.target.y() * view.scale),
                            IM_COL32(255, 190, 40, 180), 2.0f);
            }
        }
    }

    // ---- movement ----
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("Where you are, and places worth coming back to");
    draw_movement_section();

    draw_hunt_settings();

    // ---- detector tuning ----
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SetNextItemOpen(radar_tuning_open_, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("What counts as a monster")) {
        radar_tuning_open_ = true;
        bool t = false;

        ImGui::TextWrapped(
            "Only change these if the view above is marking the wrong things. "
            "The outlined rectangle is where it looks; the dull red ones are cut "
            "out of it, which is how the minimap, its notices and the icons down "
            "the right edge are kept out of the results.");

        ImGui::SetNextItemWidth(150);
        t |= ImGui::SliderInt("Health bar red at least", &detect_tuning_.red_min, 80, 250);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        t |= ImGui::SliderInt("other channels under", &detect_tuning_.other_max, 10, 160);

        ImGui::SetNextItemWidth(150);
        t |= ImGui::SliderInt("Shortest bar", &detect_tuning_.min_bar, 4, 60);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        t |= ImGui::SliderInt("Longest bar", &detect_tuning_.max_bar, 20, 400);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170);
        t |= ImGui::SliderInt("Rows of writing", &detect_tuning_.min_text_rows, 1, 8);
        help_marker("How many rows of white writing have to sit above a red bar before it "
                    "counts. This is what keeps red artwork out: flowers and fire "
                    "have no name written over them.");

        // The box and what is cut out of it, as they are for the window's
        // size now. While they follow the window they are shown worked out;
        // the first edit writes all three down, and from then on they are
        // used as written, whatever size the window is.
        int fw = have_frame ? frame.width : 0, fh = have_frame ? frame.height : 0;
        if (!have_frame) input_.window_size(fw, fh);
        DetectorTuning shown = detect_tuning_.resolved(fw, fh);
        text_dim(detect_tuning_.automatic()
                     ? "These follow the game window's size."
                     : "Written by hand: used as written. Reset to defaults to follow the "
                       "game window's size again.");
        bool rect_edit = false;
        const auto rect_row = [&](const char* label, const char* id, ScreenRect& r) {
            ImGui::TextUnformatted(label);
            int* const part[] = { &r.x, &r.y, &r.w, &r.h };
            const char* const name[] = { "x", "y", "w", "h" };
            for (int i = 0; i < 4; ++i) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                const std::string box = std::string(name[i]) + "##" + id + name[i];
                rect_edit |= ImGui::InputInt(box.c_str(), part[i], 0, 0);
            }
        };
        // With no window to measure there is nothing to show them for.
        ImGui::BeginDisabled(fw <= 0 || fh <= 0);
        rect_row("Look here:", "area", shown.area);
        rect_row("Not here:", "excl", shown.exclude);
        rect_row("Nor here:", "excl2", shown.exclude2);
        ImGui::EndDisabled();
        if (rect_edit) {
            detect_tuning_.area = shown.area;
            detect_tuning_.exclude = shown.exclude;
            detect_tuning_.exclude2 = shown.exclude2;
            t = true;
        }

        if (ImGui::Button("Reset to defaults")) {
            detect_tuning_ = DetectorTuning::defaults();
            t = true;
        }
        if (t) detector_.set_tuning(detect_tuning_);
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("Save", ImVec2(140, 0))) save_hunt_config();
    ImGui::SameLine();
    text_dim("Saved in configs/hunt.ini.");
}

void GuiApp::draw_macro_panel() {
    const auto st = macro_->stats();
    const bool focused = input_.window_focused();

    draw_window_picker_inline();
    draw_input_warning();

    // ---- the one control that matters ----
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushStyleColor(ImGuiCol_Button, macro_running_ ? kRed : kGreen);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        macro_running_ ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.45f, 0.9f, 0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    if (ImGui::Button(macro_running_ ? "STOP MACRO" : "START MACRO", ImVec2(-1, 46)))
        toggle_macro();
    ImGui::PopStyleColor(3);

    text_dim("%s toggles it, %s is an emergency stop -- both work while the game has focus.",
             InputManager::key_name(macro_config_.toggle_key).c_str(),
             InputManager::key_name(macro_config_.panic_key).c_str());

    // ---- live state ----
    ImGui::Dummy(ImVec2(0, 4));
    if (!input_.window_valid()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextUnformatted("No target window selected -- pick the game above");
        ImGui::PopStyleColor();
    } else if (macro_running_ && !focused) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("Holding: the game does not have focus. Keys resume when it does.");
        ImGui::PopStyleColor();
    } else {
        status_dot(focused, "game focused", "game not focused");
    }

    if (macro_running_) {
        ImGui::SameLine(320);
        ImGui::Text("%llu presses", static_cast<unsigned long long>(st.presses));
        if (st.skipped_unfocused) {
            ImGui::SameLine();
            text_dim("(%llu skipped, unfocused)",
                     static_cast<unsigned long long>(st.skipped_unfocused));
        }
        if (st.held_no_reading) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::Text("(%llu held: no HP/MP reading)",
                        static_cast<unsigned long long>(st.held_no_reading));
            ImGui::PopStyleColor();
        }
    }

    // Holding because the character is not in the world. Said plainly and in
    // its own line rather than tucked in with the rest: this is the state
    // somebody comes back to after a disconnect, and "the macro is on but has
    // pressed nothing for six hours" needs to explain itself on sight.
    if (macro_running_ && macro_->suspended()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        const std::string why = macro_->suspended_reason();
        ImGui::Text("HOLDING -- %s. %llu presses held back so far.",
                    why.empty() ? "the character is not in the world" : why.c_str(),
                    static_cast<unsigned long long>(st.held_suspended));
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // ---- timed keys ----
    ImGui::Checkbox("Repeating keys", &macro_config_.timers_enabled);
    help_marker("Each key runs on its own timer, independently of the others -- "
                "a skill every 1.2s alongside a buff every 5 minutes.");

    const auto due = macro_->next_due_ms();

    float live_hp, live_mp;
    macro_->current_vitals(live_hp, live_mp);
    // Of the potions under their mark, the one that drinks: only the more
    // urgent tier does (macro_urgent_potion), and the others wait.
    const int urgent_hp = macro_urgent_potion(macro_config_.keys, MacroKeyKind::HpPotion, live_hp);
    const int urgent_mp = macro_urgent_potion(macro_config_.keys, MacroKeyKind::MpPotion, live_mp);

    if (ImGui::BeginTable("##keys", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("On",      ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Key",     ImGuiTableColumnFlags_WidthFixed, 66);
        ImGui::TableSetupColumn("When",    ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("%",       ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("Every (ms)", ImGuiTableColumnFlags_WidthFixed, 96);
        ImGui::TableSetupColumn("Hold (ms)",  ImGuiTableColumnFlags_WidthFixed, 84);
        ImGui::TableSetupColumn("Jitter %",   ImGuiTableColumnFlags_WidthFixed, 72);
        ImGui::TableSetupColumn("Next");
        ImGui::TableHeadersRow();

        static const char* const kWhen[] = { "on a timer", "HP below", "MP below" };

        for (std::size_t i = 0; i < macro_config_.keys.size(); ++i) {
            auto& k = macro_config_.keys[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Checkbox("##en", &k.enabled);

            ImGui::TableNextColumn();
            char label[64];
            copy_to(label, sizeof(label), k.label);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##label", label, sizeof(label))) k.label = label;

            ImGui::TableNextColumn();
            char key[24];
            copy_to(key, sizeof(key), InputManager::key_name(k.keycode));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##key", key, sizeof(key))) {
                if (const std::uint32_t vk = InputManager::parse_key(key)) k.keycode = vk;
            }

            ImGui::TableNextColumn();
            int when = static_cast<int>(k.trigger);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##when", &when, kWhen, 3))
                k.trigger = static_cast<MacroTrigger>(when);

            ImGui::TableNextColumn();
            if (k.conditional()) {
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt("##thr", &k.threshold_pct, 0, 0))
                    k.threshold_pct = std::clamp(k.threshold_pct, 1, 99);
            } else {
                text_dim("-");
            }

            ImGui::TableNextColumn();
            int iv = static_cast<int>(k.interval_ms);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##iv", &iv, 0, 0)) k.interval_ms = static_cast<std::uint32_t>(std::max(10, iv));
            if (k.conditional() && ImGui::IsItemHovered())
                ImGui::SetTooltip("For a conditional key this is the cooldown: "
                                  "the shortest gap between two firings. It does "
                                  "not delay the reaction.");

            ImGui::TableNextColumn();
            int hold = static_cast<int>(k.hold_ms);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##hold", &hold, 0, 0)) k.hold_ms = static_cast<std::uint32_t>(std::max(0, hold));

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt("##jit", &k.jitter_pct, 0, 0);
            k.jitter_pct = std::clamp(k.jitter_pct, 0, 90);

            ImGui::TableNextColumn();
            if (!k.enabled) {
                text_dim("-");
            } else if (k.conditional()) {
                // A conditional key's countdown is meaningless -- it re-checks
                // several times a second. What matters is whether the trigger
                // can be evaluated at all, and how close it is.
                const float v = (k.trigger == MacroTrigger::HpBelow) ? live_hp : live_mp;
                const int urgent = (k.trigger == MacroTrigger::HpBelow) ? urgent_hp : urgent_mp;
                if (v < 0.0f) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
                    ImGui::TextUnformatted("no reading");
                    ImGui::PopStyleColor();
                } else if (v <= static_cast<float>(k.threshold_pct) && urgent >= 0 &&
                           static_cast<int>(i) != urgent) {
                    // Under its mark too, but another of its kind sits lower:
                    // that one drinks, and this one waits for the bar to rise.
                    text_dim("waits (%.0f%%)", v);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Under this mark too, but a potion of the same kind with a "
                                          "lower mark is the one that drinks: only the more urgent tier "
                                          "does, as in GoC.");
                } else if (v <= static_cast<float>(k.threshold_pct)) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                    ImGui::Text("firing (%.0f%%)", v);
                    ImGui::PopStyleColor();
                } else {
                    text_dim("armed (%.0f%%)", v);
                }
            } else if (macro_running_ && i < due.size()) {
                const float frac = k.interval_ms
                    ? 1.0f - static_cast<float>(due[i]) / static_cast<float>(k.interval_ms)
                    : 0.0f;
                char lbl[32];
                std::snprintf(lbl, sizeof(lbl), "%llums",
                              static_cast<unsigned long long>(due[i]));
                ImGui::ProgressBar(std::clamp(frac, 0.0f, 1.0f), ImVec2(-1, 0), lbl);
            } else {
                text_dim("-");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // The one failure mode a conditional key has: no reading, so it silently
    // never fires. Say so here rather than leaving the user to wonder.
    if (macro_config_.needs_vitals() && live_hp < 0.0f && live_mp < 0.0f) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("A key above waits on HP or MP, but nothing is being read. "
                           "Set the bars up on the HP / MP tab -- until then those "
                           "keys hold rather than fire.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Start reading HP / MP", ImVec2(220, 0))) start_vitals();
    }

    if (macro_config_.keys.size() < MacroConfig::kMaxKeys) {
        if (ImGui::Button("Add key")) {
            MacroKey k;
            k.label = "Key " + std::to_string(macro_config_.keys.size() + 1);
            macro_config_.keys.push_back(k);
        }
        ImGui::SameLine();
    }
    if (!macro_config_.keys.empty() && ImGui::Button("Remove last"))
        macro_config_.keys.pop_back();

    // ---- sequence ----
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Checkbox("Sequence", &macro_config_.sequence_enabled);
    help_marker("An ordered combo: each key, then a pause, then the next. "
                "Runs alongside the repeating keys above.");
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &macro_config_.sequence_loop);

    if (macro_config_.sequence_enabled) {
        if (ImGui::BeginTable("##seq", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 30);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("Key",  ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Hold (ms)",  ImGuiTableColumnFlags_WidthFixed, 95);
            ImGui::TableSetupColumn("Then wait (ms)");
            ImGui::TableHeadersRow();

            const std::size_t at = macro_->sequence_position();
            for (std::size_t i = 0; i < macro_config_.sequence.size(); ++i) {
                auto& s = macro_config_.sequence[i];
                ImGui::PushID(static_cast<int>(1000 + i));
                ImGui::TableNextRow();
                if (macro_running_ && i + 1 == at)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        ImGui::GetColorU32(ImVec4(0.25f, 0.45f, 0.25f, 0.55f)));

                ImGui::TableNextColumn();
                ImGui::Text("%zu", i + 1);

                ImGui::TableNextColumn();
                char label[64];
                copy_to(label, sizeof(label), s.label);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##l", label, sizeof(label))) s.label = label;

                ImGui::TableNextColumn();
                char key[24];
                copy_to(key, sizeof(key), InputManager::key_name(s.keycode));
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##k", key, sizeof(key))) {
                    if (const std::uint32_t vk = InputManager::parse_key(key)) s.keycode = vk;
                }

                ImGui::TableNextColumn();
                int hold = static_cast<int>(s.hold_ms);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt("##h", &hold, 0, 0)) s.hold_ms = static_cast<std::uint32_t>(std::max(0, hold));

                ImGui::TableNextColumn();
                int d = static_cast<int>(s.delay_after_ms);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt("##d", &d, 0, 0)) s.delay_after_ms = static_cast<std::uint32_t>(std::max(0, d));

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Add step")) macro_config_.sequence.push_back(MacroStep{});
        ImGui::SameLine();
        if (!macro_config_.sequence.empty() && ImGui::Button("Remove last step"))
            macro_config_.sequence.pop_back();
    }

    // ---- options ----
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("Options");

    ImGui::Checkbox("Only send while the game has focus", &macro_config_.require_focus);
    help_marker("Strongly recommended. With this off, keys go to whatever window is "
                "in front -- which will eventually be something you did not intend.");

    ImGui::Checkbox("Bring the game to the front when starting",
                    &macro_config_.focus_on_start);
    help_marker("Clicking START MACRO takes focus away from the game, and keys only "
                "ever go to a focused window -- so without this the macro starts and "
                "immediately holds. Only on the way on: it will not pull focus back "
                "while you are doing something else.");

    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Extra jitter %", &macro_config_.global_jitter_pct, 0, 50);
    help_marker("Added on top of each key's own jitter. Perfectly regular timing is "
                "the clearest sign a client is automated.");

    for (auto* hk : { &macro_config_.toggle_key, &macro_config_.panic_key }) {
        char buf[24];
        copy_to(buf, sizeof(buf), InputManager::key_name(*hk));
        ImGui::SetNextItemWidth(140);
        ImGui::PushID(hk);
        if (ImGui::InputText(hk == &macro_config_.toggle_key ? "Toggle key" : "Panic key",
                             buf, sizeof(buf))) {
            if (const std::uint32_t vk = InputManager::parse_key(buf)) *hk = vk;
        }
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Save macro", ImVec2(160, 0))) save_macro_config();
    ImGui::SameLine();
    if (ImGui::Button("Apply now", ImVec2(160, 0))) {
        macro_->set_config(macro_config_);
        apply_vitals_config();
        set_status("Applied");
    }
}

void GuiApp::apply_findings() {
    const auto specs = ProtocolCalibrator::to_specs(findings_);
    if (specs.empty()) {
        set_status("Nothing to apply", 5000);
        return;
    }
    for (const auto& s : specs) registry_.add(s);

    // The capture filter was limited to opcodes we already understood; the
    // newly mapped ones have to be let through or nothing changes.
    capture_.set_opcode_filter(registry_.mapped_opcodes());

    save_protocol();
    char msg[128];
    std::snprintf(msg, sizeof(msg), "Applied %zu packet layout(s)", specs.size());
    set_status(msg, 6000);
}

void GuiApp::draw_calibrate_panel() {
    ImGui::TextWrapped(
        "Works out which packet carries your position and mana by driving your "
        "character through a known routine and watching which numbers respond. "
        "A field that rises every time we hold Right and falls every time we hold "
        "Left is the x coordinate -- there is no other explanation for it.");
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "Every previous attempt to map this protocol failed because the capture "
        "caught a character standing still. Driving the input removes that.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("Before you start");
    ImGui::BulletText("Stand somewhere you can walk a few steps both ways and jump.");
    ImGui::BulletText("Somewhere quiet -- monsters knocking you about add noise.");
    ImGui::BulletText("Click the game window. It drives your character for ~20 seconds.");

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("Skill key (costs mana)", calib_cast_key_, sizeof(calib_cast_key_))) {
        if (const std::uint32_t vk = InputManager::parse_key(calib_cast_key_))
            calibrator_->set_cast_key(vk);
    }
    help_marker("Used to make mana drop on cue, which is what identifies the MP "
                "field. Pick something that always costs mana and is safe to use.");

    ImGui::Dummy(ImVec2(0, 8));

    const bool running = calibrator_->running();
    const CalibPhase phase = calibrator_->phase();

    if (!capture_running_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("Capture is not running -- start it on the Capture tab first.");
        ImGui::PopStyleColor();
    }

    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Button, kRed);
        if (ImGui::Button("Abort", ImVec2(240, 40))) calibrator_->stop();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 6));
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "%s  --  %zu packets",
                      to_string(phase), calibrator_->sample_count());
        ImGui::ProgressBar(calibrator_->progress(), ImVec2(-1, 24), lbl);

        ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
        ImGui::TextUnformatted("Hands off the keyboard while this runs.");
        ImGui::PopStyleColor();
    } else {
        if (!capture_running_) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, kGreen);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
        if (ImGui::Button(calib_ran_ ? "Run again" : "Start calibration", ImVec2(240, 40)) &&
            keys_may_start()) {
            if (const std::uint32_t vk = InputManager::parse_key(calib_cast_key_))
                calibrator_->set_cast_key(vk);

            // The fields we are hunting for are in opcodes that are not mapped
            // yet, so a filter built from the known ones would discard exactly
            // the packets this needs to see.
            capture_.set_opcode_filter({});

            if (calibrator_->start()) {
                calib_ran_ = false;
                findings_.clear();
            } else {
                set_status(calibrator_->status(), 8000);
            }
        }
        ImGui::PopStyleColor(2);
        if (!capture_running_) ImGui::EndDisabled();
    }

    // Collect the result once the run completes.
    if (!running && phase == CalibPhase::Done && !calib_ran_) {
        findings_ = calibrator_->analyse();
        calib_ran_ = true;
    }

    const std::string st = calibrator_->status();
    if (!st.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, phase == CalibPhase::Failed ? kRed : kDim);
        ImGui::TextWrapped("%s", st.c_str());
        ImGui::PopStyleColor();
    }

    if (!calib_ran_) return;

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("What responded");

    if (findings_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped(
            "No field tracked the movement. Either the character could not actually "
            "move where it was standing, or the framing in protocol.ini is wrong so "
            "the packets are being cut in the wrong places. Check the Protocol tab "
            "for a high resync count.");
        ImGui::PopStyleColor();
        return;
    }

    if (ImGui::BeginTable("##find", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Opcode", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Field",  ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("At",     ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Sure",   ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("Evidence");
        ImGui::TableHeadersRow();

        for (const auto& f : findings_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("0x%04X", f.opcode);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            ImGui::TextUnformatted(f.role.c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::Text("+%zu %s", f.offset,
                        f.type == FieldType::F32 ? "f32"
                        : f.type == FieldType::I16 ? "i16" : "i32");
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", f.confidence * 100.0);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", f.evidence.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Apply and save", ImVec2(200, 32))) apply_findings();
    ImGui::SameLine();
    text_dim("writes these into protocol.ini and starts decoding them live");
}

void GuiApp::draw_mapfile_panel() {
    ImGui::TextWrapped(
        "The game ships its own collision geometry in data\\Map -- 781 files, one per "
        "map section, with exact surface heights. This is the same data the original "
        "client's server handed out, which is why that client never needed to "
        "recognise platforms on screen. Reading it needs no capture and no walking.");
    ImGui::Dummy(ImVec2(0, 6));

    if (map_dir_.empty()) {
        map_dir_ = MapFile::default_map_dir();
        if (!map_dir_.empty()) map_stems_ = MapFile::list(map_dir_);
    }

    if (map_dir_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextWrapped("Could not find the game's data\\Map folder. Install path not "
                           "in the registry?");
        ImGui::PopStyleColor();
        return;
    }

    text_dim("%s  --  %zu maps", map_dir_.c_str(), map_stems_.size());
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Filter", map_filter_, sizeof(map_filter_));
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) map_stems_ = MapFile::list(map_dir_);

    // ---- map list ----
    if (ImGui::BeginListBox("##maps", ImVec2(260, 220))) {
        const std::string f = map_filter_;
        for (const auto& s : map_stems_) {
            if (!f.empty() && s.find(f) == std::string::npos) continue;
            const bool sel = (map_file_.loaded() && map_file_.name() == s);
            if (ImGui::Selectable(s.c_str(), sel)) {
                const std::string p = MapFile::find(map_dir_, s);
                if (map_file_.load(p)) {
                    char msg[160];
                    std::snprintf(msg, sizeof(msg),
                                  "%s: %d x %d cells (%d x %d px), %zu surface points",
                                  map_file_.name().c_str(),
                                  map_file_.width_cells(), map_file_.height_cells(),
                                  map_file_.width_px(), map_file_.height_px(),
                                  map_file_.surface_count());
                    map_status_ = msg;
                } else {
                    map_status_ = "Failed to parse " + s;
                }
            }
        }
        ImGui::EndListBox();
    }

    ImGui::SameLine();
    ImGui::BeginGroup();

    if (!map_file_.loaded()) {
        text_dim("Pick a map to load its geometry.");
    } else {
        ImGui::TextUnformatted(map_status_.c_str());
        ImGui::Dummy(ImVec2(0, 6));

        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Tile size", &map_tile_size_, 2, 32);
        help_marker("Resolution of the grid handed to the pathfinder. Smaller keeps "
                    "slopes accurate; larger keeps the graph small.");

        ImGui::CheckboxFlags("Layer 0", &map_layer_mask_, 1);
        ImGui::SameLine();
        ImGui::CheckboxFlags("Layer 1", &map_layer_mask_, 2);
        help_marker("The file carries two collision layers. Which one a map uses "
                    "varies, so try each if the graph looks wrong.");

        ImGui::Dummy(ImVec2(0, 6));
        if (ImGui::Button("Build pathfinding graph", ImVec2(220, 30))) {
            const MapData md = map_file_.to_map_data(map_tile_size_, map_layer_mask_);
            if (!md.valid()) {
                map_status_ = "Grid came out empty -- try the other layer";
            } else {
                pathfinder_.set_double_jump(bot_config_.double_jump);
                if (pathfinder_.init(md)) {
                    char msg[160];
                    std::snprintf(msg, sizeof(msg),
                                  "Graph built: %zu platforms from a %d x %d grid",
                                  pathfinder_.platform_count(), md.width, md.height);
                    map_status_ = msg;
                } else {
                    map_status_ = "Pathfinder rejected the grid";
                }
            }
        }
        if (pathfinder_.ready())
            text_dim("pathfinder holds %zu platforms", pathfinder_.platform_count());
    }
    ImGui::EndGroup();

    // ---- preview ----
    if (map_file_.loaded()) {
        ImGui::SeparatorText("Geometry");

        const float avail = ImGui::GetContentRegionAvail().x - 10.0f;
        const float scale = avail / static_cast<float>(std::max(1, map_file_.width_px()));
        const float ph = map_file_.height_px() * scale;

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + avail, origin.y + ph),
                          IM_COL32(12, 12, 18, 255));

        const ImU32 colours[2] = { IM_COL32(90, 220, 120, 255), IM_COL32(255, 170, 60, 255) };
        for (int layer = 0; layer < MapFile::kLayers; ++layer) {
            if (!(map_layer_mask_ & (1 << layer))) continue;
            for (const auto& [px, py] : map_file_.surfaces(layer)) {
                const float sx = origin.x + px * scale;
                const float sy = origin.y + py * scale;
                dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + 1.0f, sy + 1.0f),
                                  colours[layer]);
            }
        }
        ImGui::Dummy(ImVec2(avail, ph));
    }
}

void GuiApp::draw_mapper_panel() {
    ImGui::TextWrapped(
        "Learns the map from where your character actually walks, rather than trying "
        "to recognise platforms in the artwork. The terrain here is tree branches and "
        "foliage with nothing to distinguish a surface you can stand on from one "
        "painted behind it -- but a place you have stood is walkable by definition.");
    ImGui::Dummy(ImVec2(0, 6));

    const auto st = mapper_->stats();
    const auto hit = mapper_->last_hit();
    const auto id = mapper_->identity();

    // ---- run control ----
    ImGui::PushStyleColor(ImGuiCol_Button, mapper_running_ ? kRed : kGreen);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    if (ImGui::Button(mapper_running_ ? "STOP MAPPING" : "START MAPPING", ImVec2(220, 36))) {
        if (mapper_running_) {
            mapper_->stop();
            mapper_running_ = false;
        } else if (!input_.window_valid()) {
            set_status("Pick the game window on the Capture tab first", 6000);
        } else {
            mapper_->set_target_window(input_.target_window());
            mapper_->set_minimap(minimap_spec_);
            mapper_->start();
            mapper_running_ = true;
        }
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    ImGui::BeginGroup();
    status_dot(hit.found, "tracking the marker", "marker not found");
    if (st.capture_from_screen)
        text_dim("capture falling back to screen -- keep the game unobscured");
    ImGui::EndGroup();

    ImGui::Separator();

    // ---- calibration ----
    ImGui::SeparatorText("1. Point out the marker");
    ImGui::TextWrapped(
        "The map art is full of saturated yellow -- mushrooms, flowers -- so a fixed "
        "colour guess finds those instead of you. Click your marker on the minimap "
        "below and it samples the real colour.");

    if (ImGui::Button(picking_marker_ ? "Click the marker in the view below..."
                                      : "Pick marker colour", ImVec2(260, 0)))
        picking_marker_ = !picking_marker_;

    ImGui::SameLine();
    float col[3] = { minimap_spec_.marker_r / 255.0f,
                     minimap_spec_.marker_g / 255.0f,
                     minimap_spec_.marker_b / 255.0f };
    if (ImGui::ColorEdit3("##mk", col, ImGuiColorEditFlags_NoInputs)) {
        minimap_spec_.marker_r = static_cast<std::uint8_t>(col[0] * 255.0f);
        minimap_spec_.marker_g = static_cast<std::uint8_t>(col[1] * 255.0f);
        minimap_spec_.marker_b = static_cast<std::uint8_t>(col[2] * 255.0f);
        mapper_->set_minimap(minimap_spec_);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Tolerance", &minimap_spec_.tolerance, 0, 120))
        mapper_->set_minimap(minimap_spec_);

    // Live feedback: the numbers that tell you whether it is locked on.
    if (hit.pixels > 0 && !hit.found) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::Text("%d matching pixels but spanning %dpx -- that is terrain art, not the "
                    "marker. Narrow the tolerance or pick again.", hit.pixels, hit.extent);
        ImGui::PopStyleColor();
    } else if (hit.found) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::Text("marker at %.1f, %.1f  (%d px, %dpx across)",
                    hit.x, hit.y, hit.pixels, hit.extent);
        ImGui::PopStyleColor();
    }

    // ---- minimap region ----
    ImGui::SeparatorText("2. Minimap region");
    text_dim("Measured from the window's top-right corner, which is the edge it is "
             "anchored to.");
    bool changed = false;
    ImGui::SetNextItemWidth(110);
    changed |= ImGui::InputInt("Right margin", &minimap_spec_.right_margin, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    changed |= ImGui::InputInt("Top", &minimap_spec_.top, 0, 0);
    ImGui::SetNextItemWidth(110);
    changed |= ImGui::InputInt("Width", &minimap_spec_.width, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    changed |= ImGui::InputInt("Height", &minimap_spec_.height, 0, 0);
    if (changed) mapper_->set_minimap(minimap_spec_);

    // ---- live view ----
    Frame frame;
    if (mapper_->latest_frame(frame) && frame.valid()) {
        ImGui::SeparatorText("3. Live view");

        const int x0 = frame.width - minimap_spec_.right_margin - minimap_spec_.width;
        const int y0 = minimap_spec_.top;
        const float scale = 2.0f;

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Draw the minimap by hand. Uploading a texture every frame for a
        // 200x112 region costs more than plotting it, and this keeps the GUI
        // free of any texture lifetime to manage.
        for (int y = 0; y < minimap_spec_.height; ++y) {
            for (int x = 0; x < minimap_spec_.width; ++x) {
                std::uint8_t r, g, b;
                frame.get(x0 + x, y0 + y, r, g, b);
                dl->AddRectFilled(
                    ImVec2(origin.x + x * scale, origin.y + y * scale),
                    ImVec2(origin.x + (x + 1) * scale, origin.y + (y + 1) * scale),
                    IM_COL32(r, g, b, 255));
            }
        }

        const ImVec2 size(minimap_spec_.width * scale, minimap_spec_.height * scale);
        ImGui::InvisibleButton("##mm", size);

        if (ImGui::IsItemClicked() && picking_marker_) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const int px = static_cast<int>((m.x - origin.x) / scale);
            const int py = static_cast<int>((m.y - origin.y) / scale);
            std::uint8_t r, g, b;
            if (mapper_->sample_pixel(x0 + px, y0 + py, r, g, b)) {
                minimap_spec_.marker_r = r;
                minimap_spec_.marker_g = g;
                minimap_spec_.marker_b = b;
                mapper_->set_minimap(minimap_spec_);
                picking_marker_ = false;
                char msg[96];
                std::snprintf(msg, sizeof(msg), "Marker colour set to %u,%u,%u", r, g, b);
                set_status(msg);
            }
        }

        // Overlay the detection and the trace.
        if (hit.found)
            dl->AddCircle(ImVec2(origin.x + hit.x * scale, origin.y + hit.y * scale),
                          6.0f, IM_COL32(80, 255, 120, 255), 0, 2.0f);

        for (const auto& p : mapper_->platforms()) {
            dl->AddLine(ImVec2(origin.x + p.x1 * scale, origin.y + p.y * scale),
                        ImVec2(origin.x + p.x2 * scale, origin.y + p.y * scale),
                        IM_COL32(90, 190, 255, 220), 2.0f);
        }
    }

    // ---- results ----
    ImGui::SeparatorText("4. What it has learned");
    const auto plats = mapper_->platforms();
    ImGui::Text("%zu trace points   %zu platforms   %llu frames, %llu hits",
                mapper_->trace_size(), plats.size(),
                static_cast<unsigned long long>(st.frames),
                static_cast<unsigned long long>(st.hits));

    if (id.valid())
        text_dim("map fingerprint %016llx", static_cast<unsigned long long>(id.hash));

    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText("Map name", map_label_, sizeof(map_label_)))
        mapper_->set_label(map_label_);

    if (!plats.empty() && ImGui::BeginTable("##plats", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
            ImVec2(0, 150))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("x range", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("y", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("evidence");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& p : plats) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", p.id);
            ImGui::TableNextColumn(); ImGui::Text("%.0f .. %.0f", p.x1, p.x2);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.y);
            ImGui::TableNextColumn(); ImGui::Text("%d samples", p.samples);
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("Save map", ImVec2(140, 0))) {
        const std::string path = exe_relative("maps/" + mapper_->suggested_filename());
        make_folder(exe_relative("maps"));
        set_status(mapper_->save(path) ? "Saved " + path : "FAILED to save " + path, 6000);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load map", ImVec2(140, 0))) {
        const std::string path = exe_relative("maps/" + mapper_->suggested_filename());
        set_status(mapper_->load(path) ? "Loaded " + path
                                       : "No saved map for this fingerprint", 6000);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear trace", ImVec2(140, 0))) mapper_->clear_trace();

    ImGui::SameLine();
    if (ImGui::Button("Send to pathfinder", ImVec2(180, 0))) {
        const MapData md = mapper_->build_map_data();
        if (!md.valid()) {
            set_status("Not enough mapped yet to build a graph", 6000);
        } else {
            pathfinder_.set_double_jump(bot_config_.double_jump);
            if (pathfinder_.init(md)) {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "Graph built: %zu platforms",
                              pathfinder_.platform_count());
                set_status(msg, 6000);
            } else {
                set_status("Pathfinder rejected the grid", 6000);
            }
        }
    }
}

void GuiApp::draw_dashboard() {
    const Self me = state_.self();
    const auto cs = capture_.stats();
    const auto ps = protocol_ ? protocol_->stats() : ProtocolHandler::Stats{};

    if (!elevated_) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.18f, 0.05f, 1.0f));
        ImGui::BeginChild("##elev", ImVec2(0, 58), ImGuiChildFlags_Borders);
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("Not running as Administrator.");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("Npcap cannot open a network adapter without it, so packet capture "
                           "will fail and the interface list will be empty. Close GoBot, "
                           "right-click gobot_gui.exe and choose \"Run as administrator\".");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));
    }

    // ---- big toggle ----
    ImGui::Dummy(ImVec2(0, 4));
    const bool can_run = framing_verified_ && registry_.size() > 0 &&
                         input_.window_valid() && capture_running_;

    ImGui::PushStyleColor(ImGuiCol_Button, bot_running_ ? kRed : kGreen);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        bot_running_ ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.45f, 0.9f, 0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));

    if (!can_run && !bot_running_) ImGui::BeginDisabled();
    if (ImGui::Button(bot_running_ ? "STOP BOT" : "START BOT", ImVec2(-1, 46))) toggle_bot();
    if (!can_run && !bot_running_) ImGui::EndDisabled();

    ImGui::PopStyleColor(3);
    text_dim("Hotkey: %s (works while the game has focus)",
             InputManager::key_name(bot_config_.key_start_stop).c_str());

    // ---- blockers ----
    if (!can_run && !bot_running_) {
        ImGui::Dummy(ImVec2(0, 6));

        const bool autopilot_busy = auto_stage_ != Auto::Idle &&
                                    auto_stage_ != Auto::Done &&
                                    auto_stage_ != Auto::Failed;

        if (autopilot_busy) {
            ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
            ImGui::Text("Setting up automatically: %s", auto_stage_label());
            ImGui::PopStyleColor();
            ImGui::ProgressBar(auto_progress(), ImVec2(-1, 0));
            if (!auto_detail_.empty()) text_dim("%s", auto_detail_.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextUnformatted("Not ready to run:");
            ImGui::PopStyleColor();
            ImGui::Indent();
            if (!capture_running_)
                ImGui::BulletText("Capture is not running");

            if (!framing_verified_) {
                const auto s = capture_.stats();
                if (!capture_running_) {
                    ImGui::BulletText("Packet format not confirmed -- start capture and it "
                                      "confirms itself");
                } else if (s.frames_decoded < 150) {
                    ImGui::BulletText("Confirming the packet format: %llu / 150 frames seen",
                                      static_cast<unsigned long long>(s.frames_decoded));
                } else {
                    // Frames are arriving but not tiling. That is a wrong
                    // framing spec, and it is worth saying so rather than
                    // leaving a bullet point that never clears.
                    ImGui::BulletText("Packet format looks wrong: %llu resync bytes against "
                                      "%llu frames. Re-run automatic setup.",
                                      static_cast<unsigned long long>(s.resync_bytes),
                                      static_cast<unsigned long long>(s.frames_decoded));
                }
            }

            if (registry_.size() == 0)
                ImGui::BulletText("No packets mapped to meanings yet  (needs a capture with "
                                  "your character moving)");
            if (!input_.window_valid())
                ImGui::BulletText("Game window not selected  (Capture tab)");
            ImGui::Unindent();

            ImGui::Dummy(ImVec2(0, 6));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.56f, 0.38f, 1.0f));
            if (ImGui::Button("Set up automatically", ImVec2(220, 32))) auto_start();
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            text_dim("does all of the above -- about a minute, play normally while it runs");
        }
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();

    // ---- character ----
    if (ImGui::BeginTable("##dash", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        ImGui::SeparatorText("Character");

        const float hp = me.max_hp > 0 ? me.hp_pct() / 100.0f : 0.0f;
        const float mp = me.max_mp > 0 ? me.mp_pct() / 100.0f : 0.0f;

        char hp_label[64], mp_label[64];
        std::snprintf(hp_label, sizeof(hp_label), "%d / %d", me.hp, me.max_hp);
        std::snprintf(mp_label, sizeof(mp_label), "%d / %d", me.mp, me.max_mp);

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hp < 0.3f ? kRed : kGreen);
        ImGui::ProgressBar(hp, ImVec2(-1, 0), hp_label);
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kBlue);
        ImGui::ProgressBar(mp, ImVec2(-1, 0), mp_label);
        ImGui::PopStyleColor();

        ImGui::Text("Position  %.0f, %.0f", me.pos.x, me.pos.y);
        ImGui::Text("Facing    %s", me.facing == Facing::Right ? "right" : "left");
        ImGui::Text("Map       %s", me.map.empty() ? "(unknown)" : me.map.c_str());
        if (me.dead) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            ImGui::TextUnformatted("DEAD");
            ImGui::PopStyleColor();
        }

        ImGui::TableNextColumn();
        ImGui::SeparatorText("Bot");
        ImGui::Text("Phase     %s", brain_ ? to_string(brain_->phase()) : "-");
        ImGui::Text("Target    %d", brain_ ? brain_->current_target() : 0);
        ImGui::Text("Monsters  %zu", state_.monster_count());
        ImGui::Text("Queue     %zu", executor_ ? executor_->queue_size() : 0);

        if (brain_) {
            const auto bs = brain_->stats();
            ImGui::Text("Engaged   %llu", static_cast<unsigned long long>(bs.targets_engaged));
            ImGui::Text("Deaths    %llu", static_cast<unsigned long long>(bs.deaths));
        }
        if (executor_) {
            const auto es = executor_->stats();
            ImGui::Text("Skills    %llu", static_cast<unsigned long long>(es.skills_cast));
            ImGui::Text("Heals     %llu", static_cast<unsigned long long>(es.heals));
        }
        ImGui::EndTable();
    }

    // ---- pipeline health ----
    ImGui::SeparatorText("Pipeline");
    ImGui::Text("Frames decoded  %llu", static_cast<unsigned long long>(cs.frames_decoded));
    ImGui::SameLine(220);
    ImGui::Text("Applied  %llu", static_cast<unsigned long long>(ps.decoded));
    ImGui::SameLine(400);
    ImGui::Text("Unmapped  %llu", static_cast<unsigned long long>(ps.unknown));

    if (cs.resync_bytes > cs.frames_decoded * 4 && cs.frames_decoded > 20) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::Text("High resync count (%llu bytes) -- the framing is probably wrong",
                    static_cast<unsigned long long>(cs.resync_bytes));
        ImGui::PopStyleColor();
    }

    // ---- live monsters ----
    ImGui::SeparatorText("Monsters in range");
    auto monsters = state_.monsters();
    std::sort(monsters.begin(), monsters.end(), [&me](const Monster& a, const Monster& b) {
        return distance(me.pos, a.pos) < distance(me.pos, b.pos);
    });

    if (monsters.empty()) {
        text_dim("none");
    } else if (ImGui::BeginTable("##mobs", 5,
               ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
               ImVec2(0, 160))) {
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Position");
        ImGui::TableSetupColumn("Distance");
        ImGui::TableSetupColumn("HP");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        const std::int32_t target = brain_ ? brain_->current_target() : 0;
        for (const auto& m : monsters) {
            ImGui::TableNextRow();
            if (m.id == target)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(ImVec4(0.25f, 0.45f, 0.25f, 0.55f)));
            ImGui::TableNextColumn(); ImGui::Text("%d", m.id);
            ImGui::TableNextColumn(); ImGui::Text("%d", m.kind);
            ImGui::TableNextColumn(); ImGui::Text("%.0f, %.0f", m.pos.x, m.pos.y);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", distance(me.pos, m.pos));
            ImGui::TableNextColumn();
            if (m.hp < 0) text_dim("?");
            else          ImGui::Text("%d%%", static_cast<int>(m.hp_pct()));
        }
        ImGui::EndTable();
    }
}

// --------------------------------------------------------------- capture

void GuiApp::draw_capture_panel() {
    ImGui::SeparatorText("Interface");

    if (!PacketCapture::npcap_available()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextWrapped("Npcap service is not running. Install it from https://npcap.com "
                           "with 'WinPcap API-compatible Mode' ticked.");
        ImGui::PopStyleColor();
    }

    const char* preview = "(none selected)";
    if (selected_interface_ >= 0 && selected_interface_ < static_cast<int>(interfaces_.size())) {
        const auto& i = interfaces_[static_cast<std::size_t>(selected_interface_)];
        preview = i.description.empty() ? i.name.c_str() : i.description.c_str();
    }

    ImGui::SetNextItemWidth(-120);
    if (ImGui::BeginCombo("##iface", preview)) {
        for (std::size_t i = 0; i < interfaces_.size(); ++i) {
            const auto& iface = interfaces_[i];
            char label[320];
            std::snprintf(label, sizeof(label), "%-16s %s",
                          iface.ipv4.empty() ? "(no IPv4)" : iface.ipv4.c_str(),
                          iface.description.empty() ? iface.name.c_str() : iface.description.c_str());
            if (ImGui::Selectable(label, selected_interface_ == static_cast<int>(i)))
                selected_interface_ = static_cast<int>(i);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(-1, 0))) refresh_interfaces();

    ImGui::SeparatorText("Game server");
    text_dim("Narrowing this cuts noise sharply and makes framing detection far more reliable.");
    text_dim("Find it with:  netstat -n -b    while the game is connected.");
    ImGui::Dummy(ImVec2(0, 4));

    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Server IP", server_ip_, sizeof(server_ip_));
    help_marker("Leave blank to capture all TCP on the interface. "
                "Setting it is strongly recommended: a capture mixing several "
                "connections will not tile, and framing detection fails.");

    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Port", server_port_, sizeof(server_port_));

    ImGui::TextUnformatted("BPF filter");
    help_marker("Raw pcap filter. Overrides the IP and port fields when set.\n"
                "Example:  tcp and host 203.0.113.10 and port 15000");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##bpf", bpf_filter_, sizeof(bpf_filter_));

    ImGui::SeparatorText("Game window");
    ImGui::TextWrapped(
        "Pick the game window explicitly. Matching on a title substring alone will "
        "happily select an editor or browser tab that happens to be named after the "
        "game, and the bot would then type combat keys into it.");
    ImGui::Dummy(ImVec2(0, 4));

    if (ImGui::Button("Refresh list")) {
        windows_ = InputManager::enumerate_windows();
        window_list_loaded_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto-find by title")) find_game_window();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##title", window_title_, sizeof(window_title_));

    if (!window_list_loaded_) {
        windows_ = InputManager::enumerate_windows();
        window_list_loaded_ = true;
    }

    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::BeginTable("##wins", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
            ImVec2(0, 170))) {
        ImGui::TableSetupColumn("Title");
        ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Class",   ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < windows_.size(); ++i) {
            const auto& w = windows_[i];
            const bool selected = (w.hwnd == input_.target_window());
            const std::string shown = w.title.empty() ? "(untitled)" : w.title;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char id[600];
            std::snprintf(id, sizeof(id), "%s##w%zu", shown.c_str(), i);
            if (ImGui::Selectable(id, selected, ImGuiSelectableFlags_SpanAllColumns)) {
                input_.set_target_window(w.hwnd);
                app_config_.window_title = w.title;
                app_config_.process_name = w.process;
                set_status("Target window: " + shown);
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(w.process.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(w.window_class.c_str());
        }
        ImGui::EndTable();
    }

    if (input_.window_valid()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::Text("Target selected  (focused right now: %s)",
                    input_.window_focused() ? "yes" : "no");
        ImGui::PopStyleColor();
        text_dim("Input is only ever sent while that window has focus.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextUnformatted("No target window selected");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();

    if (capture_running_) {
        if (ImGui::Button("Stop capture", ImVec2(160, 32))) stop_capture();
    } else {
        if (ImGui::Button("Start capture", ImVec2(160, 32))) start_capture();
    }

    if (capture_running_) {
        const auto s = capture_.stats();
        ImGui::SeparatorText("Live");
        ImGui::Text("Packets seen    %llu", static_cast<unsigned long long>(s.packets_seen));
        ImGui::Text("TCP segments    %llu", static_cast<unsigned long long>(s.tcp_segments));
        ImGui::Text("Frames decoded  %llu", static_cast<unsigned long long>(s.frames_decoded));
        ImGui::Text("Resync bytes    %llu", static_cast<unsigned long long>(s.resync_bytes));
        ImGui::Text("Dropped (pcap)  %llu", static_cast<unsigned long long>(s.pcap_dropped));

        if (s.packets_seen == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextWrapped("No packets yet. Check the interface, and that the filter "
                               "matches the game's actual server.");
            ImGui::PopStyleColor();
        }
    }
}

// -------------------------------------------------------------- protocol

void GuiApp::draw_protocol_panel() {
    if (!framing_verified_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped(
            "GhostOnline's packet format has to be derived from your own traffic. "
            "The original client did not contain it either -- it uploaded raw frames "
            "to a vendor server that held the knowledge. Work through the steps below.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    if (ImGui::BeginTabBar("##prototabs")) {
        if (ImGui::BeginTabItem("Auto")) {
            draw_autopilot();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("1. Framing")) {
            draw_detect_section();
            ImGui::Separator();
            draw_framing_editor();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("2. Opcodes")) {
            draw_opcode_table();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("3. Inspect")) {
            draw_packet_inspector();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("4. Fields")) {
            draw_field_analyzer();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("5. Layouts")) {
            draw_layout_editor();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void GuiApp::draw_autopilot() {
    ImGui::SeparatorText("Automatic setup");
    ImGui::TextWrapped(
        "Finds the game's own server connection, picks the adapter carrying it, "
        "captures, works out the packet format, and proposes what the packets mean. "
        "No addresses to look up and nothing to configure.");
    ImGui::Dummy(ImVec2(0, 6));

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("While it runs, play normally: walk around, fight something, take "
                       "some damage. It reads the protocol from how the numbers move, so "
                       "it needs to see them move.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 10));

    const bool running = auto_stage_ != Auto::Idle &&
                         auto_stage_ != Auto::Done &&
                         auto_stage_ != Auto::Failed;

    if (!running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.56f, 0.38f, 1.0f));
        if (ImGui::Button(auto_stage_ == Auto::Done ? "Run again" : "Set up automatically",
                          ImVec2(240, 40)))
            auto_start();
        ImGui::PopStyleColor(2);
        text_dim("Takes about a minute.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, kRed);
        if (ImGui::Button("Cancel", ImVec2(240, 40))) auto_cancel();
        ImGui::PopStyleColor();
    }

    if (auto_stage_ == Auto::Idle) return;

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::SeparatorText("Progress");

    // Step list, so it is obvious where it is and what it already managed.
    struct Step { Auto stage; const char* label; };
    const Step steps[] = {
        { Auto::FindGame,       "Locate the game" },
        { Auto::FindConnection, "Find its server connection" },
        { Auto::StartCapture,   "Start capture on the right adapter" },
        { Auto::Recording,      "Record traffic" },
        { Auto::Detecting,      "Work out the packet format" },
        { Auto::Collecting,     "Collect packets" },
        { Auto::Analysing,      "Work out what packets mean" },
    };

    for (const auto& s : steps) {
        const bool done    = auto_stage_ > s.stage || auto_stage_ == Auto::Done;
        const bool current = auto_stage_ == s.stage;

        if (current) {
            ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
            ImGui::Text(">  %s", s.label);
            ImGui::PopStyleColor();
        } else if (done && auto_stage_ != Auto::Failed) {
            status_dot(true, s.label);
        } else if (auto_stage_ == Auto::Failed && auto_stage_ > s.stage) {
            status_dot(true, s.label);
        } else {
            text_dim("   %s", s.label);
        }
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (running) {
        ImGui::ProgressBar(auto_progress(), ImVec2(-1, 0), auto_stage_label());
    }

    if (!auto_detail_.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextWrapped("%s", auto_detail_.c_str());
    }

    if (auto_stage_ == Auto::Failed) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextWrapped("%s", auto_error_.c_str());
        ImGui::PopStyleColor();
    }

    if (auto_stage_ == Auto::Done) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::TextWrapped("Setup finished. Review the suggestions below, apply the ones "
                           "that look right, then tick 'Framing verified' on the Framing "
                           "tab once the Inspect tab shows values that match the game.");
        ImGui::PopStyleColor();
    }

    if (analysis_ran_) {
        ImGui::Dummy(ImVec2(0, 8));
        draw_suggestions();
    }
}

void GuiApp::draw_suggestions() {
    ImGui::SeparatorText("What the packets appear to mean");

    if (suggestions_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped(
            "Nothing identifiable. That usually means the capture caught too little "
            "movement -- run it again and walk around, fight, and take damage while "
            "it records.");
        ImGui::PopStyleColor();
        return;
    }

    ImGui::TextWrapped(
        "These are inferences from how the bytes behave, not certainties. Check each "
        "one against the game before applying it: Apply writes the layout, then the "
        "Inspect tab shows what it decodes to.");
    ImGui::Dummy(ImVec2(0, 6));

    for (std::size_t i = 0; i < suggestions_.size(); ++i) {
        const auto& g = suggestions_[i];
        ImGui::PushID(static_cast<int>(i));

        const bool already = registry_.find(g.opcode) != nullptr;

        char header[256];
        std::snprintf(header, sizeof(header), "0x%04X  %s   (%d%% confident)%s",
                      g.opcode, to_string(g.semantic),
                      static_cast<int>(g.confidence * 100),
                      already ? "   [mapped]" : "");

        if (ImGui::CollapsingHeader(header, i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ImGui::Indent();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("%s", g.reason.c_str());
            ImGui::PopStyleColor();

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Text("Layout:  %s", OpcodeRegistry::field_list_to_string(g.fields).c_str());
            ImGui::Dummy(ImVec2(0, 4));

            if (ImGui::Button("Apply", ImVec2(110, 0))) {
                PacketSpec s;
                s.opcode   = g.opcode;
                s.name     = g.suggested_name;
                s.semantic = g.semantic;
                s.fields   = g.fields;
                registry_.add(s);
                set_status("Applied 0x" + std::to_string(g.opcode) + " as " +
                           to_string(g.semantic));
            }
            ImGui::SameLine();
            if (ImGui::Button("Inspect", ImVec2(110, 0))) {
                selected_opcode_ = static_cast<int>(g.opcode);
                set_status("Selected 0x" + std::to_string(g.opcode) +
                           " -- open the Inspect tab");
            }
            ImGui::Unindent();
            ImGui::Dummy(ImVec2(0, 4));
        }
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Apply all above 70% confidence", ImVec2(280, 0))) {
        int n = 0;
        for (const auto& g : suggestions_) {
            if (g.confidence < 0.70) continue;
            PacketSpec s;
            s.opcode   = g.opcode;
            s.name     = g.suggested_name;
            s.semantic = g.semantic;
            s.fields   = g.fields;
            registry_.add(s);
            ++n;
        }
        set_status("Applied " + std::to_string(n) + " layouts");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to protocol.ini", ImVec2(200, 0))) save_protocol();
}

void GuiApp::draw_detect_section() {
    ImGui::SeparatorText("Record a sample");
    text_dim("Record while playing normally for 2-3 minutes.");
    text_dim("Walk, fight, take damage. Variety matters more than volume.");
    ImGui::Dummy(ImVec2(0, 4));

    std::size_t sample_size;
    bool        is_recording;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sample_size = raw_sample_.size();
        is_recording = recording_;
    }

    // Recording needs a running capture. Saying so up front beats a greyed-out
    // button whose reason is a line of small text further down the panel.
    if (!capture_running_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("Capture is not running -- nothing can be recorded yet.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Start capture now", ImVec2(180, 0))) start_capture();
        ImGui::SameLine();
        text_dim("or set the adapter and server on the Capture tab first");
        ImGui::Dummy(ImVec2(0, 6));
    }

    if (!capture_running_) ImGui::BeginDisabled();
    if (is_recording) {
        ImGui::PushStyleColor(ImGuiCol_Button, kRed);
        if (ImGui::Button("Stop recording", ImVec2(150, 0))) {
            std::lock_guard<std::mutex> lock(mutex_);
            recording_ = false;
        }
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("Start recording", ImVec2(150, 0))) {
            std::lock_guard<std::mutex> lock(mutex_);
            raw_sample_.clear();
            recording_ = true;
        }
    }
    if (!capture_running_) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::ProgressBar(static_cast<float>(sample_size) / static_cast<float>(kMaxSample),
                       ImVec2(-1, 0));

    if (is_recording) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::Text("RECORDING -- %zu bytes", sample_size);
        ImGui::PopStyleColor();
    } else {
        ImGui::Text("%zu bytes recorded", sample_size);
    }

    // Recording with capture up but no traffic arriving is the other silent
    // failure: the filter or adapter is wrong and nothing says so.
    if (is_recording && sample_size == 0) {
        const auto s = capture_.stats();
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        if (s.packets_seen == 0)
            ImGui::TextWrapped("No packets at all. Wrong adapter, or the server filter "
                               "excludes the game. Check the Capture tab.");
        else
            ImGui::TextWrapped("Packets are arriving but none from the game server. "
                               "Check the Server IP on the Capture tab, or clear it to "
                               "capture everything.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (sample_size < 2048) ImGui::BeginDisabled();
    if (ImGui::Button("Auto-detect framing", ImVec2(200, 30))) run_detection();
    if (sample_size < 2048) ImGui::EndDisabled();
    help_marker("Brute-forces every plausible header layout and ranks them by how "
                "cleanly they tile the recorded stream with no resyncs.");

    if (!detect_ran_) return;

    ImGui::SeparatorText("Candidates");
    if (detect_results_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextWrapped("Nothing tiled the stream. The payload is probably encrypted, "
                           "or the capture mixed several connections -- set the server IP "
                           "on the Capture tab and record again.");
        ImGui::PopStyleColor();
        return;
    }

    text_dim("Click a row to apply it. Coverage below 95%% is a guess, not an answer.");

    if (ImGui::BeginTable("##cands", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
            ImVec2(0, 200))) {
        ImGui::TableSetupColumn("Coverage", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Frames",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Opcodes",  ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Score",    ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Layout");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < detect_results_.size(); ++i) {
            const auto& r = detect_results_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            char id[32];
            std::snprintf(id, sizeof(id), "%.1f%%##c%zu", r.coverage * 100.0, i);
            if (ImGui::Selectable(id, false, ImGuiSelectableFlags_SpanAllColumns))
                apply_framing(r.spec);

            ImGui::TableNextColumn(); ImGui::Text("%zu", r.frames_parsed);
            ImGui::TableNextColumn(); ImGui::Text("%zu", r.distinct_opcodes);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", r.score);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.spec.describe().c_str());
        }
        ImGui::EndTable();
    }
}

void GuiApp::draw_framing_editor() {
    ImGui::SeparatorText("Framing");

    bool changed = false;
    int v;

    v = static_cast<int>(framing_.header_size);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Header size", &v)) { framing_.header_size = static_cast<std::uint32_t>(std::max(1, v)); changed = true; }

    v = static_cast<int>(framing_.length_offset);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Length offset", &v)) { framing_.length_offset = static_cast<std::uint32_t>(std::max(0, v)); changed = true; }

    v = static_cast<int>(framing_.length_size);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Length size", &v)) { framing_.length_size = static_cast<std::uint32_t>(std::clamp(v, 1, 4)); changed = true; }

    if (ImGui::Checkbox("Length is little-endian", &framing_.length_le)) changed = true;
    if (ImGui::Checkbox("Length includes header", &framing_.length_includes_header)) changed = true;

    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Length adjust", &framing_.length_adjust)) changed = true;
    help_marker("Added to the raw length value. Absorbs off-by-N conventions, "
                "such as a length counted from after the length field itself.");

    v = static_cast<int>(framing_.opcode_offset);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Opcode offset", &v)) { framing_.opcode_offset = static_cast<std::uint32_t>(std::max(0, v)); changed = true; }

    v = static_cast<int>(framing_.opcode_size);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Opcode size", &v)) { framing_.opcode_size = static_cast<std::uint32_t>(std::clamp(v, 1, 2)); changed = true; }

    if (ImGui::Checkbox("Opcode is little-endian", &framing_.opcode_le)) changed = true;

    v = static_cast<int>(framing_.max_frame);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Max frame size", &v)) { framing_.max_frame = static_cast<std::uint32_t>(std::max(1, v)); changed = true; }

    if (changed) {
        framing_.min_frame = framing_.header_size;
        push_framings();
    }

    if (!framing_.plausible()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextWrapped("This layout is not self-consistent -- fields must fit inside the "
                           "header and must not overlap.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("Confirmation");

    const auto health = capture_.stats();
    if (capture_running_ && health.frames_decoded > 0) {
        const double per_frame = static_cast<double>(health.resync_bytes) /
                                 static_cast<double>(health.frames_decoded);
        ImGui::Text("%llu frames decoded, %llu resync bytes (%.2f per frame)",
                    static_cast<unsigned long long>(health.frames_decoded),
                    static_cast<unsigned long long>(health.resync_bytes), per_frame);
        ImGui::PushStyleColor(ImGuiCol_Text, per_frame <= 2.0 ? kGreen : kRed);
        ImGui::TextUnformatted(per_frame <= 2.0
            ? "Frames are tiling cleanly -- this framing is right."
            : "Frames are not tiling -- this framing is wrong.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));
    }

    ImGui::TextWrapped(
        "This confirms itself once live traffic shows frames tiling cleanly, so there is "
        "normally nothing to do here. Whether the framing is right is measurable -- a "
        "wrong one resyncs constantly -- unlike a field layout, which only a human can "
        "check against the game.");
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Checkbox("Packet format confirmed", &framing_verified_);

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("Save protocol.ini", ImVec2(160, 0))) save_protocol();
}

void GuiApp::draw_opcode_table() {
    ImGui::SeparatorText("Live opcodes");
    text_dim("Do one thing at a time in game to correlate:");
    ImGui::Indent();
    text_dim("walk back and forth    -> the high-rate opcode is position");
    text_dim("stand still, take a hit -> a new opcode appears: HP");
    text_dim("pull a monster          -> monster spawn / update");
    ImGui::Unindent();
    ImGui::Dummy(ImVec2(0, 4));

    ImGui::Checkbox("Mapped only", &opcode_filter_mapped_only_);
    ImGui::SameLine();
    if (ImGui::Button("Reset counts")) {
        std::lock_guard<std::mutex> lock(mutex_);
        opcode_stats_.clear();
        samples_.clear();
        held_back_frames_ = 0;
    }

    std::vector<std::pair<std::uint16_t, OpcodeStat>> rows;
    std::uint64_t held_back = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rows.assign(opcode_stats_.begin(), opcode_stats_.end());
        held_back = held_back_frames_;
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.count > b.second.count; });

    // Said whether or not anything else is listed: they are in the capture,
    // and this table is not where they are counted.
    if (held_back > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%llu frames from inside 0x0081 or over UDP are not listed here "
                           "or used by the bot yet. gobot_sniff --watch counts them by opcode.",
                           static_cast<unsigned long long>(held_back));
        ImGui::PopStyleColor();
    }

    if (rows.empty()) {
        text_dim("No frames decoded yet. Start capture and check the framing.");
        return;
    }

    if (ImGui::BeginTable("##ops", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
            ImVec2(0, 360))) {
        ImGui::TableSetupColumn("Opcode", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Count",  ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Mapped as");
        ImGui::TableSetupColumn("Semantic", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& [op, st] : rows) {
            const PacketSpec* known = registry_.find(op);
            if (opcode_filter_mapped_only_ && !known) continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            char label[32];
            std::snprintf(label, sizeof(label), "0x%04X##op%u", op, op);
            if (ImGui::Selectable(label, selected_opcode_ == static_cast<int>(op),
                                  ImGuiSelectableFlags_SpanAllColumns))
                selected_opcode_ = static_cast<int>(op);

            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(st.count));
            ImGui::TableNextColumn();
            if (st.min_size == st.max_size) ImGui::Text("%zu B", st.max_size);
            else                            ImGui::Text("%zu-%zu B", st.min_size, st.max_size);
            ImGui::TableNextColumn();
            if (known) ImGui::TextUnformatted(known->name.c_str());
            else       text_dim("-");
            ImGui::TableNextColumn();
            if (known) ImGui::TextUnformatted(to_string(known->semantic));
            else       text_dim("-");
        }
        ImGui::EndTable();
    }

    if (selected_opcode_ >= 0)
        text_dim("Selected 0x%04X -- see the Inspect and Fields tabs.",
                 static_cast<unsigned>(selected_opcode_));
}

void GuiApp::draw_packet_inspector() {
    if (selected_opcode_ < 0) {
        text_dim("Select an opcode on the Opcodes tab first.");
        return;
    }

    const auto op = static_cast<std::uint16_t>(selected_opcode_);
    ImGui::SeparatorText("Recent packets");
    ImGui::Text("Opcode 0x%04X", op);

    std::deque<Bytes> samples;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = samples_.find(op);
        if (it != samples_.end()) samples = it->second;
    }

    if (samples.empty()) {
        text_dim("No samples captured.");
        return;
    }

    // If a layout exists, show what it decodes to — this is how you confirm
    // the layout is right rather than merely parseable.
    if (const PacketSpec* spec = registry_.find(op)) {
        ImGui::SeparatorText("Decoded with current layout");
        GamePacket pkt;
        pkt.opcode = op;
        pkt.payload = samples.back();
        if (auto d = registry_.decode(pkt)) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            ImGui::TextWrapped("%s", d->describe().c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            ImGui::TextWrapped("Layout does not fit this payload (%zu bytes). "
                               "It is probably too long.", pkt.payload.size());
            ImGui::PopStyleColor();
        }
        (void)spec;
    }

    ImGui::SeparatorText("Hex");
    ImGui::BeginChild("##hex", ImVec2(0, 320), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(nullptr);
    const std::size_t show = std::min<std::size_t>(samples.size(), 8);
    for (std::size_t i = samples.size() - show; i < samples.size(); ++i) {
        ImGui::Text("--- #%zu  %zu bytes ---", i, samples[i].size());
        ImGui::TextUnformatted(hexdump(samples[i], 16, 2).c_str());
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

void GuiApp::draw_field_analyzer() {
    if (selected_opcode_ < 0) {
        text_dim("Select an opcode on the Opcodes tab first.");
        return;
    }

    const auto op = static_cast<std::uint16_t>(selected_opcode_);

    ImGui::SeparatorText("Byte volatility");
    ImGui::TextWrapped(
        "Which byte offsets change between packets, and how. An entity id stays "
        "constant for one entity; a coordinate changes smoothly as you walk; HP "
        "moves only when you take damage.");
    ImGui::Dummy(ImVec2(0, 4));

    std::vector<Bytes> samples;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = samples_.find(op);
        if (it != samples_.end()) samples.assign(it->second.begin(), it->second.end());
    }

    if (samples.size() < 2) {
        text_dim("Need at least 2 samples of 0x%04X, have %zu.", op, samples.size());
        return;
    }

    std::size_t common = samples.front().size();
    for (const auto& s : samples) common = std::min(common, s.size());
    if (common == 0) {
        text_dim("Payloads are empty.");
        return;
    }

    ImGui::Text("%zu samples, %zu common bytes", samples.size(), common);

    std::vector<std::size_t> distinct(common, 0);
    for (std::size_t off = 0; off < common; ++off) {
        std::set<std::uint8_t> vals;
        for (const auto& s : samples) vals.insert(s[off]);
        distinct[off] = vals.size();
    }

    // Group adjacent varying bytes: they are almost always one multi-byte
    // field, and per-byte output buries that.
    ImGui::SeparatorText("Field candidates");
    if (ImGui::BeginTable("##fields", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
            ImVec2(0, 260))) {
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Likely type", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Sample values");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::size_t run_start = 0;
        bool in_run = false;

        for (std::size_t off = 0; off <= common; ++off) {
            const bool varies = (off < common) && distinct[off] > 1;

            if (varies && !in_run) { run_start = off; in_run = true; }
            else if (!varies && in_run) {
                const std::size_t len = off - run_start;
                const char* guess = (len == 1) ? "u8 / i8"
                                  : (len == 2) ? "u16 / i16"
                                  : (len == 4) ? "u32 / i32 / f32"
                                  : (len == 8) ? "u64 / f64"
                                               : "string or struct";

                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", run_start);
                ImGui::TableNextColumn(); ImGui::Text("%zu", len);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(guess);
                ImGui::TableNextColumn();

                // Reading a 4-byte run as both int and float settles most
                // cases instantly: a float coordinate is unmistakable.
                std::string vals;
                const std::size_t n = std::min<std::size_t>(samples.size(), 3);
                if (len == 4) {
                    vals = "i32: ";
                    for (std::size_t i = samples.size() - n; i < samples.size(); ++i) {
                        ByteReader r(samples[i].data(), samples[i].size());
                        r.seek(run_start);
                        vals += std::to_string(r.i32()) + " ";
                    }
                    vals += "  f32: ";
                    for (std::size_t i = samples.size() - n; i < samples.size(); ++i) {
                        ByteReader r(samples[i].data(), samples[i].size());
                        r.seek(run_start);
                        char b[32];
                        std::snprintf(b, sizeof(b), "%.2f ", r.f32());
                        vals += b;
                    }
                } else if (len == 2) {
                    vals = "i16: ";
                    for (std::size_t i = samples.size() - n; i < samples.size(); ++i) {
                        ByteReader r(samples[i].data(), samples[i].size());
                        r.seek(run_start);
                        vals += std::to_string(r.i16()) + " ";
                    }
                } else if (len == 1) {
                    vals = "u8: ";
                    for (std::size_t i = samples.size() - n; i < samples.size(); ++i)
                        vals += std::to_string(samples[i][run_start]) + " ";
                }
                ImGui::TextUnformatted(vals.c_str());
                in_run = false;
            }
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Per-byte detail");
    if (ImGui::BeginTable("##bytes", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
            ImVec2(0, 200))) {
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Distinct", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Reading");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t off = 0; off < common; ++off) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%zu", off);
            ImGui::TableNextColumn(); ImGui::Text("%zu", distinct[off]);
            ImGui::TableNextColumn();
            if (distinct[off] == 1)                       text_dim("constant -- sub-type, flag or padding");
            else if (distinct[off] <= 4)                  ImGui::TextUnformatted("few values -- enum or boolean");
            else if (distinct[off] > samples.size() / 2)  ImGui::TextUnformatted("high churn -- coordinate or low byte");
            else                                          text_dim("varies");
        }
        ImGui::EndTable();
    }
}

void GuiApp::draw_layout_editor() {
    ImGui::SeparatorText("Packet layouts");

    if (selected_opcode_ < 0) {
        text_dim("Select an opcode on the Opcodes tab to edit its layout.");
    } else {
        const auto op = static_cast<std::uint16_t>(selected_opcode_);
        ImGui::Text("Editing opcode 0x%04X", op);

        // Load the existing layout the first time this opcode is selected.
        static int last_loaded = -1;
        if (last_loaded != selected_opcode_) {
            last_loaded = selected_opcode_;
            if (const PacketSpec* s = registry_.find(op)) {
                copy_to(layout_name_, sizeof(layout_name_), s->name);
                copy_to(layout_fields_, sizeof(layout_fields_),
                        OpcodeRegistry::field_list_to_string(s->fields));
                copy_to(layout_repeat_field_, sizeof(layout_repeat_field_), s->repeat_count_field);
                layout_semantic_ = semantic_to_index(s->semantic);
            } else {
                layout_name_[0] = '\0';
                layout_fields_[0] = '\0';
                layout_repeat_field_[0] = '\0';
                layout_semantic_ = 0;
            }
        }

        ImGui::SetNextItemWidth(260);
        ImGui::InputText("Name", layout_name_, sizeof(layout_name_));

        ImGui::SetNextItemWidth(260);
        ImGui::Combo("Semantic", &layout_semantic_, kSemanticNames, kSemanticCount);
        help_marker("What this packet means to the bot. Everything above the protocol "
                    "layer dispatches on this, never on the opcode number.");

        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("Fields", layout_fields_, sizeof(layout_fields_));
        help_marker("name:type[@offset][*scale], comma-separated.\n\n"
                    "types: i8 u8 i16 u16 i32 u32 i64 u64 f32 f64\n"
                    "       str8 str16 str32 strz strfN bytesN skipN\n"
                    "suffix 'be' for big-endian, e.g. u16be\n"
                    "@N pins an absolute offset, *N applies a fixed-point scale\n\n"
                    "Example: id:u32,x:f32,y:f32,hp:i32");

        ImGui::SetNextItemWidth(260);
        ImGui::InputText("Repeat count field", layout_repeat_field_, sizeof(layout_repeat_field_));
        help_marker("For list packets: the field holding the element count. Fields after "
                    "it are read that many times as repeat groups.");

        // Validate live so a typo is caught before it is saved.
        std::string err;
        auto parsed = OpcodeRegistry::parse_field_list(layout_fields_, &err);
        if (!err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            ImGui::TextWrapped("%s", err.c_str());
            ImGui::PopStyleColor();
        } else if (!parsed.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            ImGui::Text("%zu fields parsed", parsed.size());
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0, 4));
        const bool valid = err.empty() && !parsed.empty() && layout_name_[0];
        if (!valid) ImGui::BeginDisabled();
        if (ImGui::Button("Apply layout", ImVec2(140, 0))) {
            PacketSpec s;
            s.opcode   = op;
            s.name     = layout_name_;
            s.semantic = semantic_from_string(kSemanticNames[layout_semantic_]);
            s.fields   = parsed;
            s.repeat_count_field = layout_repeat_field_;
            registry_.add(s);
            set_status("Layout applied to 0x" + std::to_string(op));
        }
        if (!valid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Save to file", ImVec2(140, 0))) save_protocol();
    }

    ImGui::SeparatorText("Mapped packets");

    const auto mapped = registry_.mapped_opcodes();
    if (mapped.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("None yet. The bot needs at least three: self position "
                           "(self_state or self_move), self HP (self_hpmp), and "
                           "monster position (monster_update).");
        ImGui::PopStyleColor();
        return;
    }

    if (ImGui::BeginTable("##layouts", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders, ImVec2(0, 200))) {
        ImGui::TableSetupColumn("Opcode", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("Semantic", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Fields");
        ImGui::TableHeadersRow();

        for (std::uint16_t op : mapped) {
            const PacketSpec* s = registry_.find(op);
            if (!s) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("0x%04X", op);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s->name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(to_string(s->semantic));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(OpcodeRegistry::field_list_to_string(s->fields).c_str());
        }
        ImGui::EndTable();
    }

    // The three things the bot actually cannot start without.
    ImGui::SeparatorText("Readiness");
    struct Req { Semantic alt_a, alt_b; const char* label; };
    const Req reqs[] = {
        { Semantic::SelfState, Semantic::SelfMove,  "Self position" },
        { Semantic::SelfHpMp,  Semantic::SelfState, "Self HP" },
        { Semantic::MonsterUpdate, Semantic::MonsterSpawn, "Monster position" },
    };
    for (const auto& r : reqs) {
        const bool have = !registry_.opcodes_for(r.alt_a).empty() ||
                          !registry_.opcodes_for(r.alt_b).empty();
        status_dot(have, r.label);
    }
}

// ------------------------------------------------------------------- bot

void GuiApp::draw_bot_panel() {
    ImGui::SeparatorText("Combat");
    ImGui::Checkbox("Auto attack", &bot_config_.auto_attack);
    ImGui::Checkbox("Auto buff", &bot_config_.auto_buff);
    ImGui::Checkbox("Auto loot", &bot_config_.auto_loot);

    ImGui::Checkbox("Lazy mode", &bot_config_.lazy_mode);
    help_marker("Wait for monsters to walk into range instead of chasing them. "
                "Slower, but far less conspicuous and it never walks off a ledge.");

    ImGui::Checkbox("Face target", &bot_config_.face_target);
    ImGui::Checkbox("Ignore flying monsters", &bot_config_.ignore_fly_monster);
    ImGui::Checkbox("Prioritise bosses", &bot_config_.priority_boss);
    ImGui::Checkbox("Back away when too close", &bot_config_.backing_away);
    ImGui::Checkbox("Double jump", &bot_config_.double_jump);
    ImGui::Checkbox("Attack while jumping", &bot_config_.allow_attack_while_jumping);

    ImGui::Checkbox("Rotate skills", &bot_config_.sequential_skill);
    help_marker("On: cycle through enabled skills so the whole kit gets used.\n"
                "Off: always prefer the lowest enabled slot.");

    ImGui::SeparatorText("Targeting");
    const char* modes[] = { "Nearest", "Lowest HP %", "Highest HP %", "Lowest absolute HP" };
    int mode = static_cast<int>(bot_config_.target_mode);
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("Target mode", &mode, modes, 4))
        bot_config_.target_mode = static_cast<TargetMode>(mode);

    ImGui::SetNextItemWidth(220);
    ImGui::SliderFloat("Attack range", &bot_config_.attack_range, 10.0f, 1000.0f, "%.0f");
    ImGui::SetNextItemWidth(220);
    ImGui::SliderFloat("Minimum range", &bot_config_.min_range, 0.0f, 400.0f, "%.0f");

    ImGui::SeparatorText("Vertical reach");
    text_dim("Side-scroller hitboxes are asymmetric -- you reach further below than above.");
    ImGui::SetNextItemWidth(220);
    ImGui::SliderFloat("Above##hit", &bot_config_.highest_hit_y, 0.0f, 400.0f, "%.0f");
    ImGui::SetNextItemWidth(220);
    ImGui::SliderFloat("Below##hit", &bot_config_.lowest_hit_y, 0.0f, 400.0f, "%.0f");

    ImGui::SeparatorText("Working area");
    text_dim("Monsters outside this box are ignored, which keeps the bot out of the next spawn.");
    ImGui::SetNextItemWidth(120); ImGui::InputFloat("Min X", &bot_config_.working_min_x, 0, 0, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120); ImGui::InputFloat("Max X", &bot_config_.working_max_x, 0, 0, "%.0f");
    ImGui::SetNextItemWidth(120); ImGui::InputFloat("Min Y", &bot_config_.working_min_y, 0, 0, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120); ImGui::InputFloat("Max Y", &bot_config_.working_max_y, 0, 0, "%.0f");

    ImGui::SeparatorText("Death");
    ImGui::Checkbox("Keep botting after death", &bot_config_.continue_auto_after_death);
    ImGui::Checkbox("Return to start position when idle", &bot_config_.return_to_original_location);

    ImGui::SeparatorText("Timing");
    text_dim("Uniform input timing is the clearest signal that a client is automated.");
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Delay min (ms)", &bot_config_.command_delay_min, 0, 300);
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Delay max (ms)", &bot_config_.command_delay_max, 0, 400);
    if (bot_config_.command_delay_max < bot_config_.command_delay_min)
        bot_config_.command_delay_max = bot_config_.command_delay_min;

    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Decision interval (ms)", &bot_config_.decision_interval_ms, 20, 500);

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Save bot config", ImVec2(160, 0))) save_bot_config();
}

// ---------------------------------------------------------------- skills

void GuiApp::draw_skills_panel() {
    ImGui::SeparatorText("Skills");
    text_dim("AoE bounds are relative to the character and mirror with facing.");
    text_dim("'Targets' is the minimum monsters inside the box before the skill fires --");
    text_dim("a wide cleave waits for a crowd instead of being wasted on one mob.");
    ImGui::Dummy(ImVec2(0, 4));

    if (ImGui::BeginTable("##skills", 7,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("On",       ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Slot",     ImGuiTableColumnFlags_WidthFixed, 46);
        ImGui::TableSetupColumn("Key",      ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Cooldown", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Range",    ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Targets",  ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Hits");
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < BotConfig::kSkillSlots; ++i) {
            auto& s = bot_config_.skills[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Checkbox("##en", &s.enabled);

            ImGui::TableNextColumn();
            ImGui::Text("%zu", i);

            ImGui::TableNextColumn();
            char key[32];
            copy_to(key, sizeof(key), InputManager::key_name(s.keycode));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##key", key, sizeof(key))) {
                if (const std::uint32_t vk = InputManager::parse_key(key)) {
                    s.keycode = vk;
                    s.key = InputManager::key_name(vk);
                }
            }

            ImGui::TableNextColumn();
            int cd = static_cast<int>(s.cooldown_ms);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##cd", &cd, 0, 0))
                s.cooldown_ms = static_cast<std::uint32_t>(std::max(0, cd));

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputFloat("##range", &s.aoe_max_x, 0, 0, "%.0f");

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt("##tgt", &s.targets, 0, 0);
            if (s.targets < 1) s.targets = 1;

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt("##hits", &s.hits, 0, 0);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Buffs");
    text_dim("Cooldown is in seconds here -- buff durations are minutes, not milliseconds.");
    ImGui::Dummy(ImVec2(0, 4));

    if (ImGui::BeginTable("##buffs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("On",   ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 46);
        ImGui::TableSetupColumn("Key",  ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Cooldown (s)");
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < BotConfig::kBuffSlots; ++i) {
            auto& b = bot_config_.buffs[i];
            ImGui::PushID(static_cast<int>(100 + i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Checkbox("##en", &b.enabled);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", i);

            ImGui::TableNextColumn();
            char key[32];
            copy_to(key, sizeof(key), InputManager::key_name(b.keycode));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##key", key, sizeof(key))) {
                if (const std::uint32_t vk = InputManager::parse_key(key)) {
                    b.keycode = vk;
                    b.key = InputManager::key_name(vk);
                }
            }

            ImGui::TableNextColumn();
            int cd = static_cast<int>(b.cooldown_sec);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##cd", &cd, 0, 0))
                b.cooldown_sec = static_cast<std::uint32_t>(std::max(0, cd));

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Movement keys");
    struct KeyRow { const char* label; std::uint32_t* vk; };
    const KeyRow keys[] = {
        { "Left",        &bot_config_.key_left },
        { "Right",       &bot_config_.key_right },
        { "Up",          &bot_config_.key_up },
        { "Down",        &bot_config_.key_down },
        { "Jump",        &bot_config_.key_jump },
        { "Attack",      &bot_config_.key_normal_attack },
        { "Loot",        &bot_config_.key_loot },
        { "Start/stop",  &bot_config_.key_start_stop },
    };
    for (const auto& k : keys) {
        char buf[32];
        copy_to(buf, sizeof(buf), InputManager::key_name(*k.vk));
        ImGui::SetNextItemWidth(140);
        ImGui::PushID(k.label);
        if (ImGui::InputText(k.label, buf, sizeof(buf))) {
            if (const std::uint32_t vk = InputManager::parse_key(buf)) *k.vk = vk;
        }
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Save bot config", ImVec2(160, 0))) save_bot_config();
}

// ------------------------------------------------------------------ heal

void GuiApp::draw_heal_panel() {
    ImGui::SeparatorText("Potions");
    text_dim("Healing pre-empts combat -- dying with a full potion belt is the one");
    text_dim("failure mode that ends a session.");
    ImGui::Dummy(ImVec2(0, 6));

    struct Row { const char* label; HealRule* rule; };
    const Row rows[] = {
        { "HP potion 1", &bot_config_.hp_heal1 },
        { "HP potion 2", &bot_config_.hp_heal2 },
        { "MP potion",   &bot_config_.mp_heal1 },
    };

    for (const auto& r : rows) {
        ImGui::PushID(r.label);
        ImGui::SeparatorText(r.label);
        ImGui::Checkbox("Enabled", &r.rule->enabled);

        char key[32];
        copy_to(key, sizeof(key), InputManager::key_name(r.rule->keycode));
        ImGui::SetNextItemWidth(140);
        if (ImGui::InputText("Key", key, sizeof(key))) {
            if (const std::uint32_t vk = InputManager::parse_key(key)) r.rule->keycode = vk;
        }

        ImGui::SetNextItemWidth(240);
        ImGui::SliderInt("Use below %", &r.rule->trigger_pct, 1, 99);
        ImGui::PopID();
    }

    ImGui::SeparatorText("Revive");
    ImGui::Checkbox("Auto revive", &bot_config_.player_auto_revive);
    help_marker("Requires a self_revive packet mapped in the Protocol tab. Without one "
                "there is nothing reliable to press on a death screen.");

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Save bot config", ImVec2(160, 0))) save_bot_config();
}

// ------------------------------------------------------------------- log

void GuiApp::draw_log_panel() {
    const char* levels[] = { "Trace", "Debug", "Info", "Warn", "Error" };
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("Level", &log_level_filter_, levels, 5);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &log_autoscroll_);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) Logger::instance().clear_ring();
    ImGui::SameLine();
    if (ImGui::Button("Verbose logging"))
        Logger::instance().set_level(LogLevel::Debug);

    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const auto lines = Logger::instance().ring();
    for (const auto& l : lines) {
        if (static_cast<int>(l.level) < log_level_filter_) continue;
        ImVec4 col = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        if (l.level == LogLevel::Warn)  col = kAmber;
        if (l.level == LogLevel::Error) col = kRed;
        if (l.level == LogLevel::Debug || l.level == LogLevel::Trace) col = kDim;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopStyleColor();
    }

    if (log_autoscroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
}

void GuiApp::draw_status_bar() {
    if (status_message_.empty()) return;
    if (now_ms() > status_until_ms_) {
        status_message_.clear();
        return;
    }
    ImGui::Separator();
    // A message in Vietnamese -- the one saying the bin's dialog may still be
    // open, which the player has to be able to read -- in the face that has
    // its letters. The built-in one has none of them.
    const bool vietnamese = text_font_ != nullptr &&
        std::any_of(status_message_.begin(), status_message_.end(),
                    [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
    if (vietnamese) ImGui::PushFont(text_font_, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::TextUnformatted(status_message_.c_str());
    ImGui::PopStyleColor();
    if (vietnamese) ImGui::PopFont();
}

} // namespace gobot
