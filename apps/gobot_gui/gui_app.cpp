#include "gui_app.hpp"
#include "gobot/alerts.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/jump_check.hpp"
#include "gobot/reviver.hpp"
#include "gobot/routine.hpp"
#include "gobot/screen_layout.hpp"
#include "gobot/session_manager.hpp"

#include "gobot/byte_reader.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_view.hpp"
#include "gobot/no_real_input.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/quiet_second.hpp"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

/// The hold everything is stopped under (stop_jobs), by name.
const char* const kStopHold = "stopping";

/// The hold a protocol calibration runs under, by name.
const char* const kCalibHold = "calibrating";

/// What a trash pass says while it waits for the chat line to be seen shut.
const char* const kTrashWaitsForChat = "Chờ: ô chat trong game có thể đang mở, dọn rác sau.";
/// The longest the trash worker holds its timed jobs back for a Tự chat line,
/// and how soon a line put off for a trash job asks again: that job's end is
/// its turn. A job held off by a line (its hold) is tried again as soon.
constexpr std::uint64_t kChatFirstMaxMs = 30'000;
constexpr std::uint64_t kChatAfterTrashMs = 1'000;
constexpr std::uint64_t kAfterChatLineMs = 3'000;
/// How long nobody may touch the mouse or keyboard, with the hold taken, before
/// the potion shop starts on the bag -- as long as the quick quest waits. With
/// the hold on GoBot presses nothing, so whatever Windows counts meanwhile is a
/// person's; and a pointer moved over the bag's tabs spoils which one reads as
/// chosen (2026-11-03, the shop stopped 4 s after the quest had found the
/// player at the mouse). When somebody did, it asks again in 30 s, then 2, 5
/// and 10 minutes, never past its own interval (QuietBackOff): each look held
/// every key off for a second, and on 2026-11-08 that came every 31 s.
constexpr int kShopQuietMs = 1000;
/// A button's run ("Mua ngay", "Thử mua") looks for its quiet second this many
/// times under the one hold -- the hand that clicked it is still on the mouse --
/// with the hold and HP watched as the run watches them; a timed run once.
constexpr int kShopQuietTries = 5;
/// A quiet second in which GoBot itself sent something -- a key let go of,
/// another job's -- is measured again this many times at most, not counted as
/// one of those tries (2026.11.10): GoBot's own input is never the player's.
constexpr int kShopOwnRelooks = 2;
/// How long the charms wait after a run that stopped on the bag's strip not
/// reading as Tiêu Dùng (BagCleaner::Result::tab_unsure): as long as a charm
/// not found waits. A minute changes nothing of what spoils the read, and
/// every try holds the bot off for a couple of seconds.
constexpr std::uint64_t kCharmTabRetryMs = 600'000;
/// The first charm check and the first shop look after a trash worker starts
/// -- a BẮT ĐẦU -- or after "Dùng bùa" / "Tự mua bình" is ticked on again. A
/// clock the last worker left later than that is kept (GuiApp::TrashClocks);
/// with none, the shop waits for what is left of the plan time saved in
/// shop_clock.txt, never less than this nor more than its interval
/// (bag::shop_first_wait_s).
constexpr std::uint64_t kCharmFirstMs = 8'000;
constexpr std::uint64_t kShopFirstMs = 45'000;
static_assert(kShopFirstMs == static_cast<std::uint64_t>(bag::kShopFirstWaitS) * 1000,
              "the shop's first look after a start, as bag::shop_first_wait_s has it");

/// Whether the whole of the game's client area lies on some screen. The bag
/// jobs read the game off the screen, which shows nothing of a part that is
/// not: a game window as large as the screen, or dragged partly off it.
bool client_on_screen(void* window) {
    const HWND h = static_cast<HWND>(window);
    RECT rc{};
    if (h == nullptr || !GetClientRect(h, &rc) || rc.right <= 4 || rc.bottom <= 4) return false;
    const POINT corners[] = { { 2, 2 }, { rc.right - 3, 2 }, { 2, rc.bottom - 3 }, { rc.right - 3, rc.bottom - 3 } };
    for (POINT p : corners) {
        if (!ClientToScreen(h, &p) || MonitorFromPoint(p, MONITOR_DEFAULTTONULL) == nullptr) return false;
    }
    return true;
}
const char* const kGameOffScreen =
    "Cửa sổ game có phần nằm ngoài màn hình (cửa sổ lớn hơn màn hình, hoặc bị kéo lệch ra ngoài): GoBot đọc game "
    "từ màn hình nên không thấy phần đó. Kéo cửa sổ game vào trong màn hình hoặc chọn độ phân giải nhỏ hơn.";

/// The game's process `pid`, its server connections and its own UDP ports,
/// as Windows' socket tables have them now: what the capture for positions
/// is built from. `conns` gets the connections themselves.
GameLink game_link_of(std::uint32_t pid, std::vector<ProcessConnection>& conns) {
    GameLink link;
    link.pid = pid;
    conns = pid != 0 ? game_connection_candidates(pid) : std::vector<ProcessConnection>{};
    for (const ProcessConnection& c : conns) link.servers.push_back(c.remote);
    link.udp_ports = udp_ports_for_pid(pid);
    return link;
}

/// `v` with `places` decimals, for the log.
std::string decimals(double v, int places) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", places, v);
    return buf;
}

/// The crossing's own pictures, from the same place as the command line's
/// crossing. Only those: the whole set is some sixty megabytes, of which a
/// keeper that runs for hours needs seven.
TemplateStore keeper_pictures(const AutoLogin::Settings& for_these) {
    TemplateStore all;
    AutoLogin::load_pictures(all, exe_relative("configs/templates.ini"));
    return AutoLogin::pictures_for(for_these, all);
}

} // namespace

// =========================================================================

GuiApp::GuiApp() = default;

GuiApp::~GuiApp() {
    // The last Load line, over the time since the one before -- ten seconds
    // of it at least -- while everything still runs as it ran.
    if (performance_.load_log_s > 0 && load_last_.at_ms != 0 && now_ms() - load_last_.at_ms >= 10'000)
        Logger::instance().write(LogLevel::Info, load_report(load_last_, load_snapshot(), "Load at exit"));
    // The packets' minute lines too -- Vitals check, Bag, Casts -- over what is left of their minute.
    if (packets_minute_ms_ != 0 && now_ms() - packets_minute_ms_ >= 10'000) packets_minute(now_ms());

    // Everything that acts on the game, told at once and then waited for, as
    // F12 stops it (stop_jobs) -- the hunter and the macro waited for first,
    // not last: they went on pressing skills and potions into the game while
    // the workers were waited for one by one. The workers own a thread that
    // clicks the game or types into it, and are joined before anything they
    // read goes away: a worker still running while the things it reads are
    // torn down is a worker clicking at a target that is no longer there.
    stop_jobs(true);
    // Tự chat waited for this time: a line half typed is left in the chat line
    // as it stops, and its thread has to be gone before what it reads is.
    stop_chat();
    if (mapper_) mapper_->stop();
    stop_detector();
    // A look at the game for the bin, or for the chat line, may still be under
    // way; it reads the window handle and its own pictures, which go away
    // below. So may the login tab's look at VTCZone, with pictures of its own.
    if (left_held_look_.valid()) left_held_look_.wait();
    if (chat_doubt_look_.valid()) chat_doubt_look_.wait();
    if (login_look_.valid()) login_look_.wait();
    stop_vitals();
    // The Bản đồ tab's worker, while what it reads -- the packets, the
    // capture's stats, the map files -- is still there: the hunting zones
    // still owed written to configs\spots.ini first.
    simple_map_stop();
    // A capture start or stop under way on the worker is let finish, and none
    // begins after it: a start it made then, nothing here would stop.
    capture_worker_.stop();
    stop_capture();
    // Whoever started it, a job's start not yet taken in by tick() included:
    // its thread hands frames to packet_self_ and frame_tally_, which go
    // before the capture itself does.
    capture_.stop();
    // The log lines said for writing off this thread (log_later) -- the last
    // minute's Casts lines among them -- written before the close goes on;
    // any still kept after the worker's end, here.
    log_worker_.wait_idle();
    log_worker_.stop();
    write_logged_later();
    // Last, once nothing is left to raise an alert: it ends a request under
    // way and forgets the webhook's address.
    alerts_shutdown();
}

void GuiApp::join_pumping(std::thread& t) {
    if (!t.joinable()) return;
    // The thread's own handle, signalled when it ends. reinterpret_cast: it is
    // a HANDLE with MSVC's library, an integer with some others.
    const HANDLE h = reinterpret_cast<HANDLE>(t.native_handle());
    for (;;) {
        // What was sent meanwhile is answered before each wait; nothing posted
        // is taken off the queue.
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        const DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_SENDMESSAGE);
        if (r != WAIT_OBJECT_0 + 1) break;      // it ended -- or the wait failed: join() below
    }
    t.join();
}

void GuiApp::initialise() {
    // GoBot's own thread, the window's: it draws, and tick() reads the keys.
    name_this_thread("gui");
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
    // The pixels GoBot works in (gobot/dpi.hpp), said once: real ones, or a
    // view Windows scales, which no reader can work on.
    if (threads_view_percent() > 100)
        GB_WARN("Display scale: Windows shows GoBot itself the screen scaled to %d%%, so nothing it reads off "
                "the screen is the screen's real pixels -- %s",
                threads_view_percent(),
                real_pixels_possible()
                    ? "Windows holds it to that, most likely by a high-DPI override on gobot_gui.exe's "
                      "Compatibility tab: take it off (the main screen's button) and start GoBot again"
                    : "this Windows is older than 10 1607: set Windows' display scale to 100%");
    else
        GB_INFO("Display scale: GoBot works in %s",
                become_dpi_aware() ? "real screen pixels" : "the screen's own pixels, which Windows does not scale here");

    // Which matcher is live. Linked statically, so there is no DLL in the
    // build folder to see it by, and "no DLLs" looks exactly like "not used".
    GB_INFO("Template matching: %s", TemplateMatcher::backend().c_str());

    // How GoBot's own window is drawn and what kind of PC this is, said once,
    // and light mode ("Nhẹ máy") worked out from them: a VMware guest, or a
    // PC with no real graphics card, draws every frame of this window on its
    // CPU, beside the game. A tester's log then says which it was.
    load_performance(app_ini);
    // [Packets]: which uses of the game's own packets may act once their
    // gates trust them. Each gate logs its counts either way.
    packets_settings_ = packets_settings(app_ini);
    GB_INFO("Packets: %s", describe_packets_settings(packets_settings_).c_str());

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
    // Kept as read: a framing applied later -- Autopilot's, detected, has no
    // 0x0081 rule -- gets the rule back from it (push_framings).
    configured_framing_ = framing_;

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
    // Handed to the capture at once: the one for positions is started from
    // the first tick on, before any hunt, and a capture with the default
    // framing would read the game's stream as garbage until HUNT ON.
    push_framings();

    protocol_ = std::make_unique<ProtocolHandler>(registry_, state_);
    executor_ = std::make_unique<CommandExecutor>(bot_config_, state_, input_, coord_);
    brain_    = std::make_unique<BotBrain>(bot_config_, state_, *executor_);

    executor_->set_pathfinder(&pathfinder_);
    brain_->set_pathfinder(&pathfinder_);

    protocol_->set_map_data_callback([this](const MapData& map) {
        pathfinder_.set_double_jump(bot_config_.double_jump);
        pathfinder_.init(map);
    });
    // On the capture thread, once for each death: tick() stops the bot, or
    // not, as "Keep botting after death" says (bot_died_).
    protocol_->set_death_callback([this] { bot_died_ = true; });

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
    // On the watchdog's thread, which has said "No game traffic" already:
    // tick() says what it means -- the bot stopped, or the game gone quiet
    // under the capture that runs all session -- and stops the bot.
    capture_.set_disconnect_callback([this] {
        traffic_lost_ = true;
    });

    copy_to(server_ip_,    sizeof(server_ip_),    app_config_.server_ip_start);
    copy_to(bpf_filter_,   sizeof(bpf_filter_),   app_config_.bpf_filter);
    copy_to(window_title_, sizeof(window_title_), app_config_.window_title);
    if (app_config_.port_start != 0)
        std::snprintf(server_port_, sizeof(server_port_), "%u", app_config_.port_start);

    refresh_interfaces();

    // Only auto-select a window when the config names something specific.
    // A title on its own is too loose to act on unprompted — a folder or a
    // browser tab named after the game can carry that whole title — so
    // otherwise we just load the list and let the user choose on the Capture tab.
    windows_ = InputManager::enumerate_windows();
    window_list_loaded_ = true;
    if (!app_config_.process_name.empty() || !app_config_.window_class.empty()) {
        input_.find_game_window(app_config_.window_title, app_config_.window_class,
                                app_config_.process_name);
    }
    // What retarget() compares against: the reader is pointed at it below, and
    // nothing else looks at the game yet.
    followed_window_ = input_.target_window();

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
    packets_off_.store(hunt_config_.packet_positions == PacketPositions::Off);
    hunter_ = std::make_unique<Hunter>(input_, coord_, detector_);
    hunter_->set_config(hunt_config_);
    // A GoBot ended between a drop's ↓ and its up -- killed, or crashed --
    // left ↓ and Ctrl down in Windows: let go of here, ups only, with
    // "Nhảy xuống bậc dưới" on.
    if (hunt_config_.drop_down) input_.release_stray_drop_keys();
    // And a high jump's ↑ and Ctrl, with "Nhảy lên bục trên" on (2026.11.05b).
    if (hunt_config_.jump_up && !hunt_config_.drop_down) input_.release_stray_drop_keys();
    // Its eyes, set once before any start: the detector's plates, passed
    // through untouched, or -- with "Vị trí từ gói tin" ticked and the packets
    // healthy -- you and the monsters from the packet world. Everything here
    // is asked from the hunter's thread: the capture's flag and stats, the
    // window's size and the hunter's config are each under their own lock,
    // and pass() asks for its eyes holding none of the hunter's.
    {
        PacketEyes::Screen screen;
        screen.player = [this](Nameplate& out, bool* fresh) { return detector_.player(out, fresh); };
        screen.mobs = [this] { return detector_.mobs(); };
        screen.frame = [this] { return detector_.stats().frames; };
        PacketEyes::Inputs in;
        in.link = [this] {
            PacketLink link;
            link.capturing = capture_.running();
            // A capture running is Npcap working, whoever started it. The
            // flag is only what the last start for positions found, and one
            // that found the service stopped is looked at again only when
            // none runs -- not while the Protocol tab's or Autopilot's does.
            link.npcap = link.capturing || position_npcap_ok_.load();
            link.last_tcp_ms = capture_.stats().last_tcp_ms;
            return link;
        };
        in.now = [] { return now_ms(); };
        in.view_size = [this](int& w, int& h) { return input_.window_size(w, h); };
        in.config = [this] { return hunter_->config(); };
        in.walk_held_until = [this] { return hunter_->walk_held_until_ms(); };
        in.takeoffs = [this] { return packet_takeoffs(); };
        // And for its jumps up (2026.11.05b, "Nhảy lên bục trên"): your
        // take-offs by kind and your own last reports, under the same guards.
        in.air = [this] { return packet_air_counts(); };
        in.own_since = [this](std::uint64_t ms) { return packet_own_since(ms); };
        // And for "Tự tìm quái trong vùng": the map tab's floors the monsters
        // are placed on, read by the tab's worker -- none read here.
        in.floors = [this] {
            std::lock_guard<std::mutex> lock(plate_floors_mx_);
            return plate_floors_;
        };
        packet_eyes_ = std::make_unique<PacketEyes>(packet_world_, std::move(screen), std::move(in));
        hunter_->set_eyes(packet_eyes_->eyes());
    }
    // Its drop through a platform ("Nhảy xuống bậc dưới"): the game's own map
    // files, and what GoBot says against a drop -- read on the hunter's
    // thread, so only atomics (drop_veto_now). Its keys are the input's own
    // (InputManager::drop_ctrl_down), through no seam.
    hunter_->set_map_book(&game_maps_);
    hunter_->set_drop_veto([this] { return drop_veto_now(); });
    // Its own attack key's presses, beside the macro's below, for the log's
    // casts: the macro skips that key, and its casts went to the macro's.
    hunter_->set_on_attack([this](std::uint32_t vk, std::uint64_t at_ms) { more_note_attack(vk, at_ms); });
    // One line per walk of yours (2026.11.10), written on the capture's
    // thread: GoBot's own keys about it -- the hunter's arrows, held or
    // tapped, and the last key put down -- to find what walks the character
    // out of its hunting area. The hunter's config under its own lock, the
    // keys out of InputManager's ring: nothing of the GUI's read there.
    packet_world_.set_walk_keys([this](const PacketWorld::OwnWalk& w) {
        if (!hunter_) return std::string();
        const HunterConfig hc = hunter_->config();
        const std::uint64_t from = w.from_ms > 5000 ? w.from_ms - 5000 : 0;
        return InputManager::walk_keys_text(InputManager::key_marks(from), hc.left_key, hc.right_key, w.from_ms,
                                            w.to_ms);
    });
    // Skills that follow the fight ask the hunter, from the macro's own
    // threads, whether it is fighting: only its atomics are read there. No
    // hunter running is no hunter, and every key keeps its timer. Walking
    // back to the hunting area (2026.11.10), every skill waits.
    macro_->set_fight([this] {
        MacroEngine::Fight f;
        if (!hunter_ || !hunter_->running()) return f;
        f.mode = hunter_->engaged()     ? MacroEngine::Fight::Mode::Fighting
                 : hunter_->returning() ? MacroEngine::Fight::Mode::Returning
                                        : MacroEngine::Fight::Mode::Idle;
        f.dx = hunter_->engaged_dx();
        f.attack_key = hunter_->attack_key();
        return f;
    });
    // Every key the macro pressed, with its time, for what the packets say
    // came of it: a potion's bag frame, a skill's cast (PacketSelf's presses).
    macro_->set_on_press([this](std::uint32_t vk, std::uint64_t at_ms) { packet_self_.note_press(vk, at_ms); });
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
    // The game losing the foreground lets go of every key GoBot holds down. A
    // key held when the player clicked another window stayed down in Windows
    // until whatever held it let go -- with the jump key on Ctrl, their typing
    // there turned into shortcuts -- though the README says losing focus lets
    // go of held keys. On the change only: nothing goes down with the game
    // behind, and every tick would cut short what is pressed the moment it is
    // back.
    {
        const bool front = input_.window_focused();
        if (game_was_front_ && !front) input_.release_all();
        game_was_front_ = front;
    }

    // A drop's ↓-up or Ctrl-up that did not go out (InputManager::
    // drop_ctrl_down): sent again at most every 100 ms, whatever is in front.
    // Still owed after kOwedTries, both may be stuck down in Windows, and a
    // walk onto a gate with ↓ down changes the map: the hunt stops.
    if (input_.keys_owed() && now_ms() - owed_look_ms_ >= 100) {
        owed_look_ms_ = now_ms();
        if (!input_.settle_owed() && input_.owed_tries() >= InputManager::kOwedTries && hunting_) {
            stop_hunt();
            // The red line under BẮT ĐẦU, as drop_tick says it: the hunt
            // stopped here, drop_tick finds it stopped and says nothing. A
            // high jump's ↑ owed is named as ↑ (2026.11.05b).
            const bool up = input_.up_owed();
            drop_stopped_vi_ = up ? "GoBot đã dừng đánh quái: phím ↑ hoặc Ctrl có thể vẫn đang bị giữ — nhấn rồi nhả ↑ "
                                    "và Ctrl trên bàn phím."
                                  : "GoBot đã dừng đánh quái: phím ↓ hoặc Ctrl có thể vẫn đang bị giữ — nhấn rồi nhả ↓ "
                                    "và Ctrl trên bàn phím.";
            simple_note(drop_stopped_vi_, 15000);
            if (!simple_view_) set_status(drop_stopped_vi_, 15000);
            if (up)
                GB_WARN("Hunter: keys owed after a jump up (Up or Ctrl up not sent, %d tries) -- hunting stopped",
                        input_.owed_tries());
            else
                GB_WARN("Hunter: keys owed after a drop (Down or Ctrl up not sent, %d tries) -- hunting stopped",
                        input_.owed_tries());
        }
    }
    // The hunter's drop: what GoBot says against one, and a hunt it wants
    // stopped -- the map changed after a press, or a key-up stuck.
    drop_tick();

    // How Windows shows the game, every few seconds: the display scale can be
    // changed under a running GoBot, and a game started again is shown afresh.
    if (now_ms() - game_scaling_looked_ms_ > 3000) check_game_scaling();

    // GoBot's own keys, read whether or not the GUI has focus, so the bot can
    // be toggled from inside the game. GetAsyncKeyState rather than
    // RegisterHotKey: a registered hotkey is swallowed and never reaches the
    // game, which matters when the toggle shares a key with something. So a
    // key goes to whatever window is in front as well, and acts only with the
    // game or GoBot there -- the panic key alone from anywhere -- and never
    // while a key box is waiting for a key: binding something to it would
    // otherwise start the bot as a side effect. See simple_hotkeys.cpp.
    const HotkeyActions keys = hotkeys_tick();

    // Panic key: stop everything and let go of every held key, unconditionally.
    // Worth a dedicated binding — when a macro misbehaves the useful response
    // is always "stop now", and hunting for the window to click is exactly
    // what you cannot do while keys are being pressed into the game.
    //
    // Acted on alone: a toggle or hunting key caught in the same tick -- the
    // same key in a file edited by hand, or F11 pressed with F12 -- would
    // start again straight after everything stopped.
    if (keys.panic) {
        // Every job told at once, every key let go of, and only then each
        // waited for (stop_jobs). The chat stops a line half typed at its
        // next key and leaves it in the chat line, on its own thread: not
        // waited for. A chat line it cannot show shut is handed over below.
        stop_jobs(true);
        set_status("Panic stop -- all input released", 5000);
        GB_WARN("Panic key pressed: everything stopped");
    } else {
        // On the simple screen the key is that screen's one button, which
        // starts the fighting and the potions along with the timers.
        if (keys.toggle) {
            if (simple_view_) simple_toggle(RunFrom::ToggleKey);
            else              toggle_macro();
        }

        // Hunting gets its own toggle. It presses movement keys, which the
        // macro never does, so being able to stop just that without stopping
        // the skill timers is worth a key of its own.
        if (keys.hunt) toggle_hunt();
    }

    if (hunting_ && hunter_ && !hunter_->running()) {
        hunting_ = false;
        // The hunter ended by itself: its eyes go with it, as at F11, unless
        // something else still looks through them.
        release_detector();
    }
    simple_track_run(simple_running());
    // HP and MP at the run's pace while anything that acts on them, or
    // presses a key, is alive, and at vitals_idle_ms -- a second -- once
    // nothing is: 6.7 grabs a second down to 1 with GoBot open and idle.
    // Every start sets the run's pace itself, before its thread can press
    // anything (vitals_for_job); this keeps it, and lets it go. The same
    // period asked again changes nothing. A run whose HP and MP both go by
    // the packets, checked against the bars, rests the reader at a second at
    // most (vitals_pace, packets_vitals.cpp).
    vitals_.set_period_ms(vitals_pace());
    // The full screen's FIND THE BARS, once a frame read after its click is in.
    full_find_bars_tick();
    // A calibration that has ended by itself -- finished, or given up when the
    // game lost focus -- lets its hold go, and its thread is joined.
    if (calib_held_ && calibrator_ && !calibrator_->running()) stop_calibration();

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

    // A stop that left the bin's dialog in doubt holds the quest rounds and
    // the popup closer back, and the executor paused; with no trash worker to
    // look, that ends once the game shows nothing waiting. The executor
    // presses only while the packet bot runs, and the stop stopped that.
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
            stop_calibration();
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
    // The same doubt, with no worker to watch it: it ends by itself once the
    // game shows the line shut, and the executor's pause with it. The packet
    // bot stopped above waits to be started again.
    watch_chat_doubt();
    // A worker that has finished is joined here, rather than at the next start.
    if (!chat_running_) join_pumping(chat_thread_);

    if (keeper_reattach_.exchange(false)) {
        const bool found = reattach_game();
        // Where the character came back: said once the capture follows the
        // new game and has it (positions_tick) -- the capture may still be on
        // the old game's process now, with the old session's place. Its
        // sockets looked at on the next tick, not up to 5 s on, so the new
        // game's map entry is not missed for that.
        if (found) {
            where_pending_ms_ = now_ms();
            position_checked_ms_ = 0;
        }
        // Logged back in, the character may stand anywhere -- and the capture
        // that placed it is on the old game's process still -- so the hunting
        // spot ("Chỗ đánh quái") stands aside until the character stands
        // within 150 px of where it was last known, or "Đặt chỗ này" sets one
        // anew (Hunter::suspend_spot). retarget() has asked so already when
        // the game's window is a new one; this is for a log in in the same
        // window. Only a hunter that runs: one a cold start begins below takes
        // its spot afresh.
        if (hunter_ && hunter_->running()) hunter_->suspend_spot();
        // And the Bản đồ tab lets go of the map it knew, hunting or not: the
        // old session's map entry is no place of the new one's.
        if (found) simple_map_lose();
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
    keep_game_in_front();
    alerts_tick();
    potions_tick();
    simple_lists_tick();    // "Quái đang thấy", and the end of a look (simple_lists.cpp)
    simple_style_tick();    // a jump key's "Thử" that has finished (simple_style.cpp)
    positions_tick();       // the capture for positions, and its log line
    simple_map_tick();      // the Bản đồ tab's worker, and its zone handed to the hunter (simple_map.cpp)
    detector_pace_tick(now_ms());   // how often the name plates are read (packets_detector.cpp)

    // The packets' map entries, followed at every tick, so that below only
    // one made while the detector runs is a map change: a detector started
    // long after the last entry stops no walk for it.
    const PacketWorld::Place packet_place = packet_world_.place();
    const bool packet_entered = packet_place.epoch != map_epoch_seen_;
    map_epoch_seen_ = packet_place.epoch;

    if (detector_running_) {
        // The camera tracker fed only while something uses it (camera_wanted):
        // afresh from the frame it is given again, not from where it was left.
        const bool feed = camera_wanted();
        if (feed != camera_fed_) {
            camera_fed_ = feed;
            if (feed) camera_.reset();
            detector_.set_camera(feed ? &camera_ : nullptr);
        }

        // A new map invalidates everything about where we were: the old level's
        // scenery has nothing to do with the new one's, and carrying a position
        // across would put every saved place a map's width out.
        //
        // Who says so: the packets, by their map entries (map_epoch), once one
        // has been seen since the world last started afresh, on the game's own
        // process, live, and not just relinked (map_change_by); otherwise the
        // minimap's fingerprint, as before. The fingerprint changed six times
        // in a minute and a half of one hunt with no door taken: with the
        // packets deciding, a change of it alone is said, counted and passed
        // over. Looked into further only when one of the two has moved.
        const bool fingerprint = minimap_.take_changed();
        if (packet_entered || fingerprint) {
            const std::uint64_t at = now_ms();
            const bool on_game = capture_.running() && position_capture_ && position_link_.pid != 0 &&
                                 position_link_.pid == pid_for_window(input_.target_window());
            const bool by_packets =
                map_change_by(packet_place, on_game, packet_self_.link(at).last_server_ms, at) == MapChangeBy::Packets;
            // One line a change, here: the tracker's own is the debug log's.
            if (by_packets && fingerprint && !packet_entered) {
                ++minimap_overruled_;
                GB_INFO("Map changed -- minimap fingerprint %016llx; the packets say still t%u_s%u -- ignored",
                        static_cast<unsigned long long>(minimap_.fingerprint()),
                        static_cast<unsigned>(packet_place.map), static_cast<unsigned>(packet_place.area));
            }
            if (!by_packets && fingerprint)
                GB_INFO("Map changed -- minimap fingerprint %016llx",
                        static_cast<unsigned long long>(minimap_.fingerprint()));
            if (by_packets ? packet_entered : fingerprint) {
                camera_.reset();
                anchor_.reset();
                if (navigator_) navigator_->stop();
            }
        }

        // Watch for the map running out. The character crossing the screen
        // while the view stays put is the one moment the level itself gives a
        // coordinate away. Nothing applies what it catches to the positions
        // yet (M27): lasting places are to come from the packets instead.
        Nameplate me;
        if (detector_.player(me)) {
            const auto fx = camera_.fix();
            const bool walking = input_.moving() ||
                                 (navigator_ && navigator_->running()) ||
                                 (hunter_ && hunter_->running());
            anchor_.update(fx.tracking ? fx.dx : 1, me.x(), walking);
        }
    }

    // Your HP and MP from the packets, checked against the bars just read
    // (packets_vitals.cpp): every value that came and every read of the bars
    // given to the check, and what it makes of them said -- before anything
    // below goes by them.
    vitals_check_tick(now_ms());

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
    //
    // By how long HP has gone unread, not by the latest read: one unreadable
    // frame held the macro and let go of every key, eight times in 3 s while
    // the character died on 2026-10-26, and 29 times in 76 s of hunting at full
    // health, each costing the skill that fell due. The potions are not held by
    // this -- an unread HP holds them by itself, as no reading.
    //
    // At the reader's last read, not at the clock: grabs that fail -- the game
    // minimised -- leave it as the last frame had it, as hp_percent() always
    // did. Judged at the clock, a game minimised was out of the world, and its
    // return cast every buff again as after a death. last_read_ms() before
    // hp_seen_ms(), the reverse of the order the reader stores them in, so a
    // new read is never paired with an old sight of HP. A game gone is out.
    //
    // How long HP may go unread is MacroEngine::kOutOfWorldAfterMs at the
    // run's pace, and wider while the reader rests (in_world_window_ms): one
    // unreadable read at a second's pace is no reason to hold. The packets
    // never say the character is in the world; they may only add a hold, while
    // a map loads -- the server's map entry under 3 s ago and no report of
    // yours since (packet_map_loading). A load is under the ten seconds that
    // re-cast the buffs.
    if (vitals_running_ && macro_ && macro_->running()) {
        const std::uint64_t frame_at = vitals_.last_read_ms();
        const bool loading = packet_map_loading(now_ms());
        const bool in_world = input_.window_valid() &&
                              VitalsReader::in_world(vitals_.hp_seen_ms(), frame_at, in_world_window_ms()) && !loading;
        if (in_world == macro_->suspended())
            macro_->set_suspended(!in_world,
                                  in_world  ? std::string{}
                                  : loading ? "a map is loading (packets)"
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
            if (revived > revive_seen_ && capture_.running()) note_where("revived");
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

    // Your HP and MP from the packets go to the macro, bar by bar, while that
    // bar is usable -- checked against the bars on the screen and passed,
    // its value of the packets' current epoch, on the capture running now,
    // the server heard within 5 s, a maximum known -- asked afresh now
    // (gate_vitals, packets_vitals.cpp). A bar whose check has just
    // disagreed hands over the read of the bars it disagreed with, while
    // that is fresh and the screen has none of its own to give. Otherwise
    // the bars read off the screen, as before; the pet's HP always.
    //
    // With neither, while the packet bot runs, its own packets' HP and MP, as
    // before: the registry's path, which no layout shipped feeds. No reading
    // once the game's traffic has been silent longer than the macro trusts
    // one.
    SelfVitals packets = gate_vitals(now_ms());
    // A bar gone back to the screen while the reader rests -- a gap, a
    // restart, a check that left Trusted with no read to hand over -- has
    // the run's pace now, not at the next tick: the cut stamps paced_from_ms
    // before the read below is judged, so a read up to a second old goes to
    // the macro as no reading, until the read the cut wakes.
    if ((packets.hp < 0.0f || packets.mp < 0.0f) && vitals_.period_ms() > vitals_run_period())
        vitals_.set_period_ms(vitals_pace());
    if (packets.hp < 0.0f && packets.mp < 0.0f && bot_running_)
        packets = packet_vitals(state_.self(), PacketCapture::silent_for_ms(capture_.stats(), now_ms()),
                                MacroEngine::kVitalsStaleMs);
    const bool from_packets = packets.hp >= 0.0f || packets.mp >= 0.0f;
    // With no reader of the bars to say the character is out of the world, a
    // death read from the packets holds the macro instead: no potion, buff or
    // timed skill on the death panel. Let go once it is over, or the bot or
    // the reader takes over.
    if (macro_ && macro_->running()) {
        const bool down = bot_running_ && !vitals_running_ && (state_.self().dead || packets.hp == 0.0f);
        if (down != packet_dead_hold_) {
            packet_dead_hold_ = down;
            macro_->set_suspended(down, down ? "the character is dead" : std::string{});
        }
    }

    if (vitals_running_) {
        // As no reading once the reader has not read the bars for as long as
        // the macro trusts a reading: its grabs failing, it keeps its last
        // values, and pushed on as fresh every frame they would hold the HP
        // potion back as above its mark, and fill the black box with an HP
        // nobody read (R8R-3). The macro holds a potion on no reading. The
        // pet's HP, off the same frames, likewise.
        //
        // And no reading from a read taken before a job cut the pace from
        // vitals_idle_ms to the run's (paced_from_ms): up to a second old,
        // it would pass as fresh, and a potion due at the macro's first look
        // drinks on an HP the player has since drunk back up. The read the
        // job woke lands tens of ms on.
        const std::uint64_t last = vitals_.last_read_ms();
        const std::uint64_t read_at = last >= vitals_.paced_from_ms() ? last : 0, at = now_ms();
        if (from_packets)
            macro_->set_vitals(
                packets.hp >= 0.0f ? packets.hp
                                   : VitalsReader::if_fresh(vitals_.hp_percent(), read_at, at, MacroEngine::kVitalsStaleMs),
                packets.mp >= 0.0f ? packets.mp
                                   : VitalsReader::if_fresh(vitals_.mp_percent(), read_at, at, MacroEngine::kVitalsStaleMs),
                VitalsReader::if_fresh(vitals_.pet_percent(), read_at, at, MacroEngine::kVitalsStaleMs));
        else
            macro_->set_vitals(VitalsReader::if_fresh(vitals_.hp_percent(), read_at, at, MacroEngine::kVitalsStaleMs),
                               VitalsReader::if_fresh(vitals_.mp_percent(), read_at, at, MacroEngine::kVitalsStaleMs),
                               VitalsReader::if_fresh(vitals_.pet_percent(), read_at, at, MacroEngine::kVitalsStaleMs));

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
    } else if (from_packets && macro_) {
        macro_->set_vitals(packets.hp, packets.mp);
    }

    // A death, or the traffic gone quiet, as the capture's threads asked: the
    // bot stopped here, on the thread that starts and stops it, so a stop
    // cannot land in the middle of a start and be lost (bot_died_).
    if (bot_died_.exchange(false) && !bot_config_.continue_auto_after_death) stop_bot();
    if (traffic_lost_.load()) say_traffic_lost();
    if (traffic_lost_.exchange(false)) stop_bot();

    if (bot_running_ && brain_ && !brain_->running()) {
        // The brain stopped itself (death with continue-after-death off).
        bot_running_ = false;
        set_status("Bot stopped itself");
    }

    auto_step();
    check_framing_health();
    // Last: the Load line, once every [Performance] load_log_s seconds.
    load_tick();
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
    // Shown at once, though nobody is touching the window (the panic key's
    // line, pressed in the game): a frame for it. Not for the same line said
    // again while it shows, which some callers do on every tick.
    if (msg != status_message_ || now_ms() >= status_until_ms_) request_redraw();
    status_message_ = msg;
    status_until_ms_ = now_ms() + ms;
}

// =========================================================================
//                       HOW HEAVY GOBOT IS ON THE PC
// =========================================================================

void GuiApp::load_performance(const IniFile& app_ini) {
    performance_ = performance_settings(app_ini);
    machine_ = read_machine(renderer_);
    light_signals_ = judge_machine(machine_);
    light_ = resolve_light_mode(performance_.light, light_signals_);
    GB_INFO("%s", describe_renderer(renderer_).c_str());
    GB_INFO("%s", describe_machine(machine_, light_signals_, performance_.light, light_).c_str());
    // The pace in use, not only the setting: never slower than vitals_idle_ms
    // (vitals_run_period), so 500 with vitals_idle_ms=300 reads every 300 ms,
    // as the Load line says.
    std::string run = performance_.vitals_run_ms == 0 ? "auto (" + std::to_string(vitals_run_period()) + ")"
                                                      : std::to_string(performance_.vitals_run_ms);
    if (performance_.vitals_run_ms != 0 && vitals_run_period() != performance_.vitals_run_ms)
        run += " (" + std::to_string(vitals_run_period()) + " in use)";
    GB_INFO("Performance: gui_pacer %s, vitals_idle_ms %d, vitals_run_ms %s, load_log_s %d",
            performance_.gui_pacer ? "on" : "off", performance_.vitals_idle_ms, run.c_str(), performance_.load_log_s);
    // Light mode never asks PrintWindow again of a window this session has
    // seen refuse it (PrintWindowVerdicts).
    print_window_verdicts().set_light(light_.on);
}

bool GuiApp::any_job_alive() const {
    // One-shot passes count: a "Dọn ngay", a shop's purchase, a "Gửi thử", a
    // quest round each watch HP while they hold the potions off, and the
    // keeper's "Vào game ngay" presses keys. Whatever presses a key goes at
    // the run's pace; so does "Tự tìm", which takes the next frame.
    const bool autopilot = auto_stage_ != Auto::Idle && auto_stage_ != Auto::Done && auto_stage_ != Auto::Failed;
    return simple_running() || macro_running_ || (macro_ && macro_->running()) || hunting_ || party_running_ ||
           revive_running_ || popups_running() || keeper_running_ || trash_running_ || chat_running_ ||
           quest_running_ || simple_find_bars_ || full_find_bars_ || coord_.critical_active() || jump_test_running() ||
           jump_kind_running() || bot_running_ || autopilot || (calibrator_ && calibrator_->running()) ||
           (navigator_ && navigator_->running());
}

int GuiApp::vitals_run_period() const {
    // Never slower than with nothing running: vitals_idle_ms=150, "as before",
    // with light mode's 250 would read faster idle than in a fight.
    return std::min(gobot::vitals_run_period(performance_.vitals_run_ms, light_.on), performance_.vitals_idle_ms);
}

void GuiApp::vitals_for_job() { vitals_.set_period_ms(vitals_run_period()); }

GuiApp::Pacing GuiApp::pacing() const {
    Pacing p;
    p.pacer_on = performance_.gui_pacer;
    p.rates = light_.on ? FramePacer::light_rates() : FramePacer::normal_rates();
    // Waiting on the player: a key box listens for its key, and "Tự tìm"
    // and FIND THE BARS finish on a frame (simple_checklist,
    // full_find_bars_tick), so they get the interactive rate for as long as
    // they last.
    p.want_interactive = simple_listening_ != 0 || simple_find_bars_ || full_find_bars_;
    // The Bản đồ tab shown: the character and the monsters move on it, so
    // behind the game and idle it is drawn kMapTabFps times a second, 10
    // since 2026.11.05 (the owner's Q3 (a); 5 before) -- kMapTabLightFps, 2,
    // with "Nhẹ máy" -- rather than 2 and 1. Its cost shows on the Load line.
    if (simple_view_ && map_tab_open()) {
        const int rate = light_.on ? kMapTabLightFps : kMapTabFps;
        p.rates.background = std::max(p.rates.background, rate);
        p.rates.idle = std::max(p.rates.idle, rate);
    }
    return p;
}

void GuiApp::load_tick() {
    // How long since the pass before: the keys are read once a pass, so a gap
    // over two of Windows' clock ticks is a quick tap of F12 that could have
    // gone unseen. A clock read a pass.
    const std::uint64_t at = load_clock_us();
    if (load_pass_at_us_ != 0 && at > load_pass_at_us_) {
        const double gap_ms = static_cast<double>(at - load_pass_at_us_) / 1000.0;
        if (gap_ms > load_pass_gap_max_ms_) load_pass_gap_max_ms_ = gap_ms;
        if (gap_ms > kLatePassMs) ++load_passes_late_;
    }
    load_pass_at_us_ = at;

    const int every_s = performance_.load_log_s;
    if (every_s <= 0) return;
    // This is the thread that reads F12, so nothing between two lines but a
    // clock read; the line itself is a lock, a GetThreadTimes per named
    // thread and a few cheap queries (take_load_snapshot), once a minute.
    if (load_last_.at_ms != 0 && now_ms() - load_last_.at_ms < static_cast<std::uint64_t>(every_s) * 1000u) return;
    LoadSnapshot now = load_snapshot();
    // Written whole, not through GB_INFO, whose line stops at 2 KB.
    if (load_last_.at_ms != 0) Logger::instance().write(LogLevel::Info, load_report(load_last_, now));
    load_last_ = std::move(now);
}

LoadSnapshot GuiApp::load_snapshot() {
    LoadSnapshot s;
    take_load_snapshot(s);

    s.gui = true;
    LoopCounters& loop = loop_counters_;
    s.passes = loop.passes;
    s.pass_gap_max_ms = load_pass_gap_max_ms_;
    s.passes_late = load_passes_late_;
    load_pass_gap_max_ms_ = 0.0;
    s.frames = loop.frames;
    s.occluded_skips = loop.occluded_skips;
    s.build_ms = loop.build_ms;
    s.build_max_ms = loop.build_max_ms;
    s.present_ms = loop.present_ms;
    s.present_max_ms = loop.present_max_ms;
    loop.build_max_ms = 0.0;
    loop.present_max_ms = 0.0;

    s.vitals_running = vitals_running_;
    s.vitals_reads = vitals_.stats().reads;
    s.vitals_period_ms = vitals_.period_ms();
    s.detector_running = detector_running_;
    s.detector_frames = detector_.stats().frames;
    s.capture_running = capture_.running();
    const PacketCapture::Stats packets = capture_.stats();
    s.packets = packets.packets_seen;
    s.pcap_dropped = packets.pcap_dropped;
    // The frames by kind, since the last "frames:" line: every ten minutes,
    // at a hunt's start and end (frames_line_ms_ set to 0 there), and when
    // what the game sends has changed -- not in every entry.
    {
        const std::uint64_t at = now_ms();
        if (frames_line_ms_ == 0 || at - frames_line_ms_ >= 600'000 || frame_tally_.top_changed()) {
            const std::string kinds = frame_tally_.take(12);
            if (!kinds.empty()) {
                const std::uint64_t over = frames_line_ms_ == 0 ? 0 : (at - frames_line_ms_) / 1000;
                s.frame_kinds = (over > 0 ? "over " + std::to_string(over) + " s: " : std::string()) + kinds;
                s.frames_filtered = capture_.filtering();
            }
            frames_line_ms_ = at;
        }
    }

    s.jobs = load_jobs();
    s.tab = simple_view_ ? simple_tab_shown_ : "Nâng cao";
    s.light = light_.on;
    return s;
}

std::string GuiApp::load_jobs() const {
    std::string out;
    const auto add = [&out](bool on, const char* name) {
        if (!on) return;
        if (!out.empty()) out += '+';
        out += name;
    };
    add(hunting_, "hunt");
    add(macro_running_, "macro");
    add(bot_running_, "packet bot");
    add(party_running_, "party");
    add(revive_running_, "revive");
    add(popups_running(), "popups");
    add(keeper_running_, keeper_oneshot_ ? "keeper(once)" : "keeper");
    add(trash_running_, trash_oneshot_ ? "trash(once)" : "trash");
    add(chat_running_, chat_oneshot_ ? "chat(once)" : "chat");
    add(quest_running_, quest_oneshot_ ? "quest(once)" : "quest");
    add(navigator_ && navigator_->running(), "walk");
    add(simple_find_bars_ || full_find_bars_, "find bars");
    add(coord_.critical_active(), "hold");
    return out;
}

bool GuiApp::take_redraw() {
    // A job started or stopped however it happened -- F10, F11 or F12 with
    // the game in front, which sends GoBot's window nothing, or a worker that
    // ended by itself -- shows on the next frame, not at the background
    // rate's.
    const unsigned jobs = (hunting_ ? 1u : 0u) | (macro_running_ ? 2u : 0u) | (party_running_ ? 4u : 0u) |
                          (keeper_running_ ? 8u : 0u) | (revive_running_ ? 16u : 0u) |
                          (trash_running_ ? 32u : 0u) | (chat_running_ ? 64u : 0u) | (quest_running_ ? 128u : 0u) |
                          (popups_running() ? 256u : 0u) | (bot_running_ ? 512u : 0u);
    const bool changed = jobs != redraw_jobs_;
    redraw_jobs_ = jobs;
    const bool asked = redraw_.exchange(false);
    return asked || changed;
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
                find_and_retarget(app_config_.window_title, app_config_.window_class,
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
            // A start or stop of the capture for positions under way on the
            // worker is waited out a tick at a time, never here; one it has
            // finished is taken in first, so that capture_running_ says what
            // runs.
            if (capture_worker_.busy()) return;     // tried again next tick
            if (auto out = take_position_out()) position_started(*out);
            if (capture_running_) {
                // Ours is stopped as it is, not by stop_capture(), which is a
                // hand's stop and would keep the packets off after Autopilot.
                // Autopilot's own feeds the same on_packets meanwhile, and
                // position_reattach leaves it alone while it records.
                if (position_capture_) {
                    capture_.stop();
                    capture_running_ = false;
                    position_capture_ = false;
                } else {
                    stop_capture();
                }
            }

            // Point the adapter at whichever local address the game's own
            // socket is bound to — no guessing, no default-route heuristic.
            const std::string local_ip = ip_to_string(game_connections_.front().local.ip);
            std::string iface = PacketCapture::find_interface_by_ip(local_ip);
            if (iface.empty()) iface = PacketCapture::default_interface();

            for (std::size_t i = 0; i < interfaces_.size(); ++i) {
                if (interfaces_[i].name == iface) { selected_interface_ = static_cast<int>(i); break; }
            }

            // The game's own UDP sockets as well, by its process: its reports of
            // where your character is go over UDP, to wherever they go, and a
            // filter of its connections alone may never see them.
            const std::string filter = build_filter_for(game_connections_, udp_ports_for_pid(game_pid_));
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
            // A new capture: the packet world starts from nothing, as it
            // hears nothing of what went before.
            packet_world_.reset();

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

    if (find_and_retarget(window_title_, app_config_.window_class, app_config_.process_name)) {
        set_status("Game window found -- confirm it is the right one on the Capture tab", 6000);
    } else {
        set_status("Game window not found -- pick it manually on the Capture tab", 6000);
    }
}

void GuiApp::retarget(void* hwnd) {
    const bool changed = hwnd != followed_window_;
    followed_window_ = hwnd;
    input_.set_target_window(hwnd);
    apply_vitals_config();                   // the reader: this window, and the bars
    // Whether it runs or not: start_detector sets it again anyway. Another
    // window than the one it had makes it forget all it saw in the old one.
    detector_.set_target_window(hwnd);
    if (mapper_) mapper_->set_target_window(hwnd);
    check_game_scaling();
    if (!changed) return;
    // Another game window while hunting -- a game opened again by hand and
    // found with "Tìm lại", or the keeper's after a log in: the character may
    // stand anywhere now, and the capture is on the old game's process until
    // it follows the new one. The hunting spot stands aside as at a log in
    // (Hunter::suspend_spot), until the character stands within 150 px of
    // where it was, or "Đặt chỗ này" sets one anew.
    if (hunter_ && hunter_->running()) hunter_->suspend_spot();
    // The Bản đồ tab lets go of the map it knew, as at a log in.
    simple_map_lose();
    // Where the view was, the map's edge, and a walk steered by them mean
    // nothing in another window.
    camera_.reset();
    anchor_.reset();
    if (navigator_ && navigator_->running()) navigator_->stop();
    GB_INFO("Following game window %p: keys, the HP/MP reader, the monster detector and the mapper",
            hwnd);
}

bool GuiApp::find_and_retarget(const std::string& title, const std::string& window_class,
                               const std::string& process) {
    if (!input_.find_game_window(title, window_class, process)) return false;
    retarget(input_.target_window());
    return true;
}

void GuiApp::check_game_scaling() {
    game_scaling_looked_ms_ = now_ms();
    const ScaleCure was = game_scaling_cure_;
    void* const game = input_.target_window();
    game_scaling_ = input_.window_valid() ? window_scaling(game) : WindowScaling{};
    game_scaling_cure_ = cure_for(game, game_scaling_);
    game_scaling_program_ = game_scaling_cure_ == ScaleCure::None ? std::string() : window_program(game);
    // GoBot's own exe, while Windows holds GoBot to its scaling.
    own_override_found_ = threads_view_percent() > 100 && has_any_dpi_override(own_exe_path());
    if (game_scaling_cure_ != was && game_scaling_cure_ != ScaleCure::None)
        GB_WARN("Windows shows the game %s to %d%% (drawn %dx%d, shown %dx%d), and GoBot cannot read it "
                "like this: %s",
                game_scaling_.percent >= 100 ? "enlarged" : "shrunk", game_scaling_.percent,
                game_scaling_.own_w, game_scaling_.own_h, game_scaling_.real_w, game_scaling_.real_h,
                cure_advice(game_scaling_cure_));
}

std::string GuiApp::fix_game_scaling() {
    const std::string exe = window_program(input_.target_window());
    if (exe.empty()) {
        GB_WARN("Display scale: Windows would not say which program the game's window belongs to");
        return "Không biết game chạy từ file nào nên không sửa được. Tự sửa: chuột phải vào file chạy "
               "game (Game.exe trong thư mục game) > Properties > Compatibility > Change high DPI settings "
               "> đánh dấu Override high DPI scaling behavior, chọn Application.";
    }
    std::string why;
    if (!set_high_dpi_override(exe, &why)) {
        GB_WARN("Display scale: could not set the high-DPI override for %s: %s", exe.c_str(), why.c_str());
        return "Windows không cho lưu cài đặt này. Tự sửa: chuột phải vào " + exe + " > Properties > "
               "Compatibility > Change high DPI settings > đánh dấu Override high DPI scaling behavior, "
               "chọn Application.";
    }
    game_scaling_cure_ = ScaleCure::RestartGame;
    return "Đã sửa. Tắt game và mở lại: từ lần mở sau Windows không phóng to game nữa.";
}

std::string GuiApp::fix_own_scaling() {
    const std::string exe = own_exe_path();
    std::string why;
    if (!clear_dpi_override(exe, &why)) {
        GB_WARN("Display scale: could not take the high-DPI override off %s: %s", exe.c_str(), why.c_str());
        return "Windows không cho sửa. Tự sửa: chuột phải vào " + exe + " > Properties > Compatibility > "
               "Change high DPI settings > bỏ dấu Override high DPI scaling behavior, rồi tắt GoBot và mở lại.";
    }
    own_scaling_fixed_ = true;
    return "Đã sửa. Tắt GoBot và mở lại.";
}

void GuiApp::start_capture() {
    // A hand's start, on the Capture tab, never a key's: a start or stop of
    // the capture for positions under way on the worker is waited for, and
    // taken in, so that capture_running_ says what runs.
    capture_worker_.wait_idle();
    if (auto out = take_position_out()) position_started(*out);
    // Started by hand: the packets come back with it, after a hand's stop.
    position_capture_off_ = false;
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

    // A new capture: the packet world starts from nothing.
    packet_world_.reset();
    if (capture_.start(cfg)) {
        capture_running_ = true;
        set_status("Capture started");
    } else {
        set_status("Capture failed to start -- run as Administrator?", 8000);
    }
}

void GuiApp::stop_capture() {
    // As start_capture: a hand's stop, never a key's, after the worker.
    capture_worker_.wait_idle();
    if (auto out = take_position_out()) position_started(*out);
    if (!capture_running_) return;
    capture_.stop();
    capture_running_ = false;
    recording_ = false;
    // Whoever started it -- the one for positions included -- a capture
    // stopped here stays stopped: the hunter goes back to the screen at its
    // next pass, and the capture for positions is not started again until
    // the next hunt, or the Capture tab's Start.
    position_capture_ = false;
    position_capture_off_ = true;
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
    // Arms the executor as well: it presses nothing but from here to the
    // bot's stop.
    brain_->start();
    bot_running_ = true;
    GB_INFO("=== BOT ENABLED ===");
    // As bot_default.ini ships, everything it would press is off: said, or a
    // bot shown running that only stands there looks like one that is broken.
    // Its potions and buffs are the macro's, and go only while the macro runs.
    if (!bot_config_.any_action_on()) {
        set_status("The packet bot is on, but its attack is off (skills go with the attack): it will not "
                   "attack or cast. Switch it on under Attack, or in configs/bot_default.ini. Its potions "
                   "and buffs are the macro's, and go only while the macro runs.", 12000);
        GB_WARN("Packet bot started with its attack off");
    } else if (!macro_running_) {
        set_status("The packet bot is on. Its potions and buffs are the macro's, and the macro is off: "
                   "start it under Features > Timed keys, or with the macro's key (" +
                       InputManager::key_name(macro_config_.toggle_key) + "), for them to go.",
                   12000);
        GB_WARN("Packet bot started with the macro off: no potions, no buffs");
    }
}

void GuiApp::stop_bot() {
    if (!bot_running_) return;
    // Halts the executor as well: its queue and route dropped, its keys let
    // go of, and nothing pressed again until the next start.
    if (brain_) brain_->stop();
    input_.release_all();
    bot_running_ = false;
    GB_INFO("=== BOT DISABLED ===");
}

void GuiApp::toggle_bot() {
    if (bot_running_) stop_bot(); else start_bot();
}

void GuiApp::on_packets(const std::vector<GamePacket>& all) {
    // Every frame to the packet world first, UDP and unpacked ones included:
    // your own position comes only over UDP, and the map's monster table only
    // from inside an 0x0081. It reads each opcode from one side and of one
    // size only (PacketWorld::opcodes), and passes over the rest.
    packet_world_.on_packets(all);
    // And to what is kept of your own character -- under this capture's start
    // count, whoever started it -- the count of every kind for the Load line,
    // and the casts' and buffs' part.
    packet_self_.on_packets(all, capture_.start_count());
    frame_tally_.add(all);
    more_on_packets(all);

    // Frames found inside 0x0081, and frames that came over UDP, are counted
    // and go no further than that. The tables here key everything by opcode,
    // the same opcode number need not mean the same thing in those places,
    // and the analyzer's suggestions become layouts the bot acts on -- so
    // what the packet bot sees, and what this screen suggests, stays what it
    // was. gobot_sniff --watch lists them.
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
    FramingSpec f = framing_;
    // Autopilot applies the framing it detected, and detection never finds
    // the 0x0081 rule (detect_framing reads no trailers): every wrapper then
    // costs twelve bytes of resync, and nothing inside one -- the map's table
    // of monsters -- is ever read. With protocol.ini's header, the rule is
    // protocol.ini's.
    const FramingSpec& c = configured_framing_;
    const bool same_header = f.header_size == c.header_size && f.length_offset == c.length_offset &&
                             f.length_size == c.length_size && f.opcode_offset == c.opcode_offset &&
                             f.opcode_size == c.opcode_size;
    if (!f.has_trailer() && c.has_trailer() && same_header) {
        f.trailer_opcode = c.trailer_opcode;
        f.trailer_bytes = c.trailer_bytes;
        f.inner_offset = c.inner_offset;
    }
    // Each wrapper unpacked, so that its one frame comes out whole for the
    // packet world. What else reads frames takes only those off the stream
    // (on_packets), and those come out the same either way; a wrapper that
    // does not unpack -- the client's are ciphered -- is left shut and counted.
    f.unpack = f.unwraps();
    capture_.set_framing(f);
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
                 : hunt_config_.packet_positions == PacketPositions::On
                     ? "picks a target and fights it -- by the game's packets while healthy, by name plates otherwise"
                     : "picks a target and fights it -- by name plates",
                 [this] { draw_attack_panel(); });
    if (attacking != engaged) {
        if (via_packets) { if (attacking) start_bot();  else stop_bot();  }
        else             { if (attacking) start_hunt(); else stop_hunt(); }
    }

    draw_feature("Weapon ranges", nullptr,
                 "how far to stand, from the weapon",
                 [this] { draw_profile_panel(); });

    // Buffs are the macro's keys marked as buffs, as Healing's are its potion
    // keys: bot_default.ini has none any more. The switch turns each on or off.
    {
        const std::vector<std::size_t> buffs = macro_keys_of_kind(macro_config_.keys, MacroKeyKind::Buff);
        bool buffing = false;
        for (const std::size_t i : buffs)
            if (macro_config_.keys[i].enabled) { buffing = true; break; }
        if (draw_feature("Buffs", &buffing, "re-casts each on its own timer",
                         [this] { draw_buff_panel(); })) {
            for (const std::size_t i : buffs) macro_config_.keys[i].enabled = buffing;
            apply_macro_config();
        }
    }

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
                         "drinks when HP, MP or the pet's HP drops",
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
    // Read again when the file changes -- the simple screen's Tổ đội tab and
    // a text editor write it too -- rather than on every frame the panel is
    // open (gobot/file_watch.hpp). What this panel writes, it forgets below,
    // so the next frame reads it back as before.
    if (party_panel_watch_.due(path)) {
        Party::Settings fresh;
        party_panel_have_ = fresh.load(path);
        if (party_panel_have_) party_panel_rules_ = std::move(fresh);
        else                   party_panel_watch_.retry();
    }
    if (!party_panel_have_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::Text("No %s yet.", path.c_str());
        ImGui::PopStyleColor();
        return;
    }
    Party::Settings& rules = party_panel_rules_;

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
        party_panel_watch_.forget();
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

    // Both files read again when they change, not on every frame the panel is
    // open (gobot/file_watch.hpp): the simple screen writes app.ini at every
    // switch, and a person writes both by hand.
    const std::string path = exe_relative("configs/app.ini");
    if (keeper_panel_app_watch_.due(path)) {
        IniFile ini;
        if (!ini.load(path)) keeper_panel_app_watch_.retry();
        CrashWatch::Settings fresh;
        fresh.load(ini);
        keeper_panel_crash_ = fresh;
    }
    const CrashWatch::Settings& s = keeper_panel_crash_;

    ImGui::Text("Watching   %s, and %s behind it",
                s.game_process.c_str(), s.launcher_process.c_str());
    ImGui::Text("Gives up   after %d restart(s)", s.max_restarts);
    text_dim("A restart is every time the game had to be closed and entered again: found gone, hung or "
             "stalled, or ended by a crossing stuck part way in -- the login key's included. "
             "A client that falls over that often is not failing at random, "
             "and a program that keeps reconnecting to a server which is "
             "refusing it is indistinguishable from an attack on it. Change "
             "these in [Crash] in configs/app.ini.");

    ImGui::Dummy(ImVec2(0, 6));

    // With no file, the table that ships, which the keeper writes out.
    const std::string login_path = exe_relative("configs/login.ini");
    if (keeper_panel_login_watch_.due(login_path)) {
        AutoLogin::Settings login;
        if (!login.load(login_path)) keeper_panel_login_watch_.retry();
        keeper_panel_can_login_ = login.unidentified().empty();
    }
    const bool can_login = keeper_panel_can_login_;
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
        { "Pet and avatar",  "pet looting and revive, avatar reset window" },
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
                         !s.check ? "(not checked)" : s.gone ? "(gone)" : s.confirm.c_str(),
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
                          "in part. Its potions and buffs are this macro's.");

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
            "health bars, and your character by its name -- or, with Positions "
            "from packets On and the packets healthy, you and the monsters from "
            "the game's own traffic. The packet bot takes over by itself once the "
            "protocol map can say where you are standing; its layouts cannot, so "
            "for now this is the engine that can fight.");
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
        // failing to reach something, and only the split says which. The
        // fights picked straight back, in reach -- without a style every one,
        // every give_up_ms -- on a line of their own: counted as given up,
        // they read as ten a minute out of reach when there was none.
        const bool style = hunt_config_.goc_rules();
        const std::uint64_t repicked = std::min(hs.targets_repicked, hs.targets_dropped);
        text_dim("%llu attacks   %llu finished off   %llu given up on (%s)",
                 static_cast<unsigned long long>(hs.attacks),
                 static_cast<unsigned long long>(hs.targets_finished),
                 static_cast<unsigned long long>(hs.targets_dropped - repicked),
                 style ? "out of reach, or no damage" : "out of reach");
        if (!style)
            text_dim("%llu re-picked while in reach (every %u s, Như trước)",
                     static_cast<unsigned long long>(repicked), hunt_config_.give_up_ms / 1000);
        else if (repicked > 0)
            text_dim("%llu picked straight back (a hold or the game behind, not out of reach)",
                     static_cast<unsigned long long>(repicked));

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
    // The macro's buff keys, the only buffs there are: the packet bot's own,
    // in bot_default.ini, cast beside them.
    ImGui::TextWrapped("Buffs are the macro's keys set to \"a buff\" under Timed keys, or the Tu buff row on "
                       "the simple screen -- set them there. They go while the macro runs, the packet bot's "
                       "included.");
    ImGui::Dummy(ImVec2(0, 4));
    const auto buffs = macro_keys_of_kind(macro_config_.keys, MacroKeyKind::Buff);
    if (buffs.empty()) text_dim("No buff keys yet.");
    for (const std::size_t i : buffs) {
        const MacroKey& k = macro_config_.keys[i];
        text_dim("%s: key %s, every %u s%s", k.label.c_str(), InputManager::key_name(k.keycode).c_str(),
                 static_cast<unsigned>(macro_timer_interval_ms(k) / 1000), k.enabled ? "" : "  (off)");
    }
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
    vitals_for_job();       // HP and MP at the run's pace before any key
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
    if (macro_) macro_->set_vitals(-1.0f, -1.0f, -1.0f);
}

void GuiApp::full_find_bars_tick() {
    if (!full_find_bars_) return;
    // The first frame the reader has read since the click, which asked for a
    // read at once (vitals_for_job) -- as "Tự tìm" does. The full screen keeps
    // find_bars' fallback, the topmost red and blue anywhere: its panel shows
    // the bars it took, and tells the player to check them before a Save.
    Frame frame;
    if (vitals_.last_read_ms() >= full_find_bars_since_ms_ && vitals_.latest_frame(frame) && frame.valid()) {
        full_find_bars_ = false;
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
    } else if (now_ms() - full_find_bars_since_ms_ > 4000) {
        full_find_bars_ = false;
        set_status("Could not get a fresh picture of the game -- is it visible, and is "
                   "the HP/MP reader running?", 8000);
    }
}

// ------------------------------------------------------------------ parties

// A worker's line, like set_status's, asks for a frame: with the game in front
// GoBot draws at the background rate, a frame a second in light mode, and the
// line would wait for it. request_redraw is an atomic store, safe from any
// thread; the pacer holds asked-for frames to the interactive rate, so a
// chatty worker cannot draw more than that.
void GuiApp::say_party(const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        party_log_.push_back(line);
        if (party_log_.size() > 200) party_log_.erase(party_log_.begin());
    }
    request_redraw();
}

void GuiApp::say_keeper(const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        keeper_log_.push_back(line);
        if (keeper_log_.size() > 200) keeper_log_.erase(keeper_log_.begin());
    }
    request_redraw();
}

void GuiApp::say_keeper_vi(const std::string& now, const std::string& problem) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        // Said again on every poll of a crossing: a frame only for new words.
        changed = now != keeper_vi_ || (!problem.empty() && problem != keeper_problem_vi_);
        keeper_vi_ = now;
        if (!problem.empty()) keeper_problem_vi_ = problem;
    }
    if (changed) request_redraw();
}

void GuiApp::start_party() {
    vitals_for_job();       // HP and MP at the run's pace before any key
    if (party_running_) return;
    if (!input_.window_valid()) {
        set_status("Pick the game window first", 6000);
        return;
    }
    // A loop that ended by itself leaves its thread to be joined. Assigning
    // over a thread that still wants joining ends the program.
    join_pumping(party_thread_);
    party_stop_ = false;
    party_running_ = true;
    party_accepted_ = 0;
    party_invited_ = 0;
    party_thread_ = std::thread([this] { party_loop(); });
}

void GuiApp::stop_party(bool wait) {
    party_stop_ = true;
    if (!wait) return;
    // Joined even when it is no longer running: a loop that stopped by itself
    // still has a thread, and destroying that unjoined ends the program.
    join_pumping(party_thread_);
    party_running_ = false;
}

/// Both halves of auto-party, on one thread.
///
/// Answering invitations and sending them share a capture and a set of rules,
/// and both are slow enough to stutter the window if they ran on the drawing
/// thread: an invitation costs several seconds of clicking and waiting for the
/// client to catch up.
void GuiApp::party_loop() {
    name_this_thread("party");
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
    capture.set_label("party");
    PartyInvite answering;
    answering.set_settings(accept_geometry);
    PartyChat chat;
    chat.set_settings(chat_rules);
    PlayerList list;
    list.set_settings(list_geometry);

    // F12, DỪNG, unticking: asked in every wait of an answer or an invite, and
    // before each of their clicks. Against the clock, as the player list's own
    // waits are: Windows rounds each sleep up to its tick, and short slices
    // added up made a wait a quarter longer.
    const auto stopped = [this] { return party_stop_.load(); };
    const auto nap = [&](int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (stopped()) return false;
            const auto left = until - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) return true;
            std::this_thread::sleep_for(
                std::min<std::chrono::steady_clock::duration>(left, std::chrono::milliseconds(50)));
        }
    };

    say_party("watching");
    std::string answered;
    std::string refused;            // the dialog last left alone, and why
    std::uint64_t rules_read_ms = now_ms();
    // One look this pass, taken afresh each pass; kept across them so that
    // its frame's 3 MB are grabbed into again rather than made anew at up to
    // 6.7 looks a second (take_party_look keeps the buffer).
    PartyLook look;

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
            nap(500);
            continue;
        }

        // One look this pass, and what was so of it as it was grabbed: whether
        // the game was all in view is asked right before the grab and right
        // after it, not when the look is read -- a second or more later, an
        // invitation answered meanwhile (simple_party.cpp, take_party_look).
        if (!input_.window_valid() ||
            !take_party_look(party_sight(capture, input_.target_window()), look)) {
            nap(500);
            continue;
        }
        const Frame& frame = look.frame;
        // "Tổ đội: …" on the tab, every few seconds (simple_party.cpp).
        party_hud_tick(look, store);

        // An invitation on screen, to answer. The footer picture is the two
        // buttons every dialog has: the trash bin's own dialogs, told apart by
        // bag::bin_asking, are no invitation.
        PartyInvite::Seen seen;
        bool bin = false;
        if (rules.auto_accept) {
            seen = answering.look(frame, store, have_font ? &font : nullptr);
            bin = seen.up && bag::bin_asking(frame, store);
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
                    if (!mouse.owns_lock() || trash_pass_ || stopped()) return false;
                    if (!input_.window_focused()) {
                        input_.focus_window();
                        if (!nap(250)) return false;
                    }
                    if (accept && !nap(accept_geometry.settle_ms)) return false;
                    Frame now;
                    if (!capture.grab(input_.target_window(), now) || !now.valid()) return false;
                    const auto again = answering.look(now, store, have_font ? &font : nullptr);
                    if (!again.up || again.who.empty() || bag::bin_asking(now, store) ||
                        std::abs(again.at.x - seen.at.x) > 3 || std::abs(again.at.y - seen.at.y) > 3)
                        return false;
                    // Still the same player's: the decision was made on seen.who,
                    // and another invitation can come up in the same place
                    // during the focus and the settle.
                    if (PartyInvite::fold(again.who) != PartyInvite::fold(seen.who)) return false;
                    if (stopped()) return false;
                    // Only with the game in front and the game itself under the
                    // button, both asked again as the button goes down, and a
                    // stop asked once more after the pointer's pause there: the
                    // frame comes from PrintWindow, which shows the game even
                    // when it is covered, and a click lands on whatever is on
                    // top there.
                    InputManager::GameClick how = InputManager::GameClick::Clicked;
                    const bool clicked =
                        accept ? input_.click_on_game(again.accept_x, again.accept_y, false, stopped, &how)
                               : input_.click_on_game(again.decline_x, again.decline_y, false, stopped, &how);
                    // A click refused leaves the invitation up, tried again on
                    // every look: why is said once for each dialog, or the
                    // player sees it sit there with nothing in the log.
                    if (!clicked && how != InputManager::GameClick::Stopped) {
                        const std::string line = std::string(accept ? "could not accept " : "could not decline ") +
                                                 seen.who + ": " + InputManager::game_click_name(how);
                        const std::string key = std::to_string(seen.at.x) + "," +
                                                std::to_string(seen.at.y) + line;
                        if (key != refused) say_party(line);
                        refused = key;
                    }
                    return clicked;
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
                        // This pass's look is from before it: nothing goes
                        // on it (read_party, PartyHud::stale).
                        party_events().changed(now_ms(), "an invitation was declined", party_text::kEventDeclined);
                        say_party("declined " + seen.who);
                        answered = seen.who;
                    }
                } else if (answer(true)) {
                    // A member of somebody else's party from now on, though
                    // this pass's look still shows the player alone: no
                    // invite goes on it (read_party, PartyHud::stale), and the
                    // next look shows the party as the game draws it.
                    party_events().changed(now_ms(), "an invitation was accepted", party_text::kEventAccepted);
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

            // The player's own lines, read back off the chat, are never
            // somebody asking, whatever their length (PartyChat::without_own):
            // said by the character named on Đánh quái, a line Tự chat said,
            // or the keyword GoBot itself typed -- for as long as that line
            // stays on the chat, by the speaker it was read with in its first
            // minute. A Tự chat line that was the keyword alone was taken for
            // the player asking on 2026-10-20, 193 s after it was typed; an
            // advert with the keyword in it would have the player try to
            // invite himself after every one. What was typed is this run's
            // and, read from SaidLately's file on every look, any other
            // process's: this GoBot before it was started again, gobot_cli
            // --chat (R8bR-1). The name through the detector's own lock, not
            // the Đánh quái tab's copy.
            lines = chat.without_own(lines, detector_.character_name(), said_lately(), now_ms());

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
                if (!list.ensure_open(input_, capture, store, &why, stopped)) {
                    if (stopped()) break;
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
                const bool sent = list.invite(input_, capture, store, *row, &why, stopped);
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

        nap(chat_rules.vote_gap_ms);
    }

    say_party("stopped");
    party_running_ = false;
}

// -------------------------------------------------------- keeping it alive

void GuiApp::start_keeper(bool oneshot) {
    vitals_for_job();       // HP and MP at the run's pace before any key
    // A keeper started for "Vào game ngay" alone becomes the run's own.
    if (keeper_running_) {
        if (!oneshot) keeper_oneshot_ = false;
        return;
    }
    join_pumping(keeper_thread_);
    keeper_ui_.reset();                         // the watchdog starts afresh
    keeper_oneshot_ = oneshot;
    if (!oneshot) keeper_login_now_ = false;    // a key's request the last keeper never took
    keeper_stop_ = false;
    keeper_running_ = true;
    keeper_restarts_ = 0;
    keeper_closed_counted_ = false;
    {
        // A new start, so whatever stopped the last one is old news.
        std::lock_guard<std::mutex> lock(worker_mutex_);
        keeper_vi_ = "Đang theo dõi game.";
        keeper_problem_vi_.clear();
    }
    keeper_thread_ = std::thread([this] { keeper_loop(); });
}

void GuiApp::stop_keeper(bool wait) {
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
    if (!wait) return;
    join_pumping(keeper_thread_);
    keeper_running_ = false;
    // A "Vào game ngay" asked for and not yet taken up goes with the keeper
    // that was to take it: left set, the next press of the key was read as
    // withdrawing it, and only a third press logged in.
    keeper_login_now_ = false;
}

/// Notice the client has fallen over, end it, and cross back in.
void GuiApp::keeper_loop() {
    name_this_thread("keeper");
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
            keeper_note("could not read " + login_path + " -- crossing with the pages that ship, "
                        "and leaving the file as it is");
        else if (login.save(login_path))
            keeper_note("wrote " + login_path + " with the pages that ship");
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
    // only if the file comes to name others; see run_crossing.
    TemplateStore store = keeper_pictures(login);

    CrashWatch watch(watch_settings);
    keeper_note("watching");
    bool seen_running = false;      // the client has been up since this started
    bool crossed_in = false;        // this keeper has logged in at least once
    bool said_found = false;        // the cold start's game was found up, and said so

    while (!keeper_stop_) {
        CrashWatch::Look seen = watch.look();
        // Stopped while it looked -- F12, DỪNG, unticking: nothing more. Not the
        // watchdog's verdict, and above all no client ended or crossed into
        // after the player asked for everything to stop.
        if (keeper_stop_) break;
        // The game seen up again: a closing counted earlier is behind it.
        if (!seen.bad()) keeper_closed_counted_ = false;
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
                    keeper_note("the game is already running -- following it and starting the "
                                "ticked jobs");
                    say_keeper_vi("Game đã mở sẵn. Đang theo dõi game.");
                    said_found = true;
                }
                keeper_reattach_ = true;
            }
            for (int i = 0; i < 30 && !keeper_stop_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        keeper_note(seen.detail);
        if (!seen.code.empty()) keeper_note("error code " + seen.code);

        // Never up since this started -- BẮT ĐẦU with the game closed. That is
        // a start, not a restart, the way GoC treats a missing window: there
        // is no client to end and no session for the server to see close, so
        // neither the shut-down nor the cool-down, and a game launcher already
        // open is left for the crossing to carry on from. It is one crossing
        // either way, and one that fails stops this.
        const bool cold = move == CrashWatch::Move::Start;
        // A client gone because a crossing ended it -- the login key's, then
        // cancelled by the key -- was counted as it was ended: the same
        // closing is not a second restart, nor a disconnect to tell of.
        const bool counted = !cold && keeper_closed_counted_ && seen.what == CrashWatch::Trouble::Gone;
        keeper_closed_counted_ = false;
        if (!cold && !counted)
            alert("disconnect", alert_text::disconnect(seen.what, seen.code,
                                                       keeper_restarts_ < watch_settings.max_restarts &&
                                                       elevated_ && login.unidentified().empty(),
                                                       seen.minutes));

        // This client gone is a restart, counted before anything is ended
        // (keeper_take_restart): at max_restarts it is said and alerted
        // there, and nothing is ended.
        if (!cold && !counted && !keeper_take_restart(watch_settings)) break;

        if (!cold) {
            // The last moment a stop keeps the client from being ended. One
            // already being shut down is not cut short: CrashWatch::shut_down
            // takes no stop yet.
            if (keeper_stop_) break;
            say_keeper_vi(CrashWatch::trouble_vi(seen) + " — GoBot đóng game rồi vào lại sau " +
                          std::to_string(watch_settings.cooldown_s) + " giây.");
            watch.shut_down(seen);
            for (int i = 0; i < watch_settings.cooldown_s * 10 && !keeper_stop_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (keeper_stop_) break;
        }

        // The crossing, the login key's too (run_crossing): login.ini read
        // afresh, the keeper's checks, and every client it ends a restart.
        if (run_crossing(cold ? Crossing::Cold : Crossing::Restart, watch, watch_settings, login, store) !=
            Crossed::In)
            break;      // stopped, or a person is needed: said and alerted there
        seen_running = true;
        crossed_in = true;
        // In by the screen's word. Beside it, what the packets had of the way
        // in -- the last map entry, and the link -- only written down (the
        // packets plan, item I).
        {
            const std::uint64_t now = now_ms();
            const std::uint64_t entry = packet_self_.link(now).last_entry_ms;
            char entered[48];
            if (entry == 0)
                std::snprintf(entered, sizeof entered, "none seen");
            else
                std::snprintf(entered, sizeof entered, "%.1f s before",
                              static_cast<double>(now > entry ? now - entry : 0) / 1000.0);
            GB_INFO("Keeper: in by the screen; the packets: map entry %s; %s", entered,
                    packet_link_text(now).c_str());
        }
        keeper_stall_restart();
    }

    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        keeper_state_ = "not watching";
    }
    say_keeper("stopped");
    keeper_running_ = false;
}

void GuiApp::keeper_note(const std::string& line) {
    GB_INFO("Keeper: %s", line.c_str());
    say_keeper(line);
}

void GuiApp::keeper_give_up(const std::string& english, const std::string& vi) {
    GB_WARN("Keeper: %s", english.c_str());
    say_keeper(english);
    say_keeper_vi(vi, vi);
}

bool GuiApp::keeper_take_restart(const CrashWatch::Settings& watch_settings) {
    if (keeper_restarts_ < watch_settings.max_restarts) {
        ++keeper_restarts_;
        return true;
    }
    const std::string n = std::to_string(keeper_restarts_.load());
    keeper_give_up("that is " + n + " restarts and the game needs another -- stopping, a person is needed",
                   "Đã phải vào lại game " + n + " lần — GoBot ngừng tự vào lại. Cần người kiểm tra.");
    alert("keeper_gave_up", alert_text::gave_up_restarts(keeper_restarts_));
    return false;
}

/// One crossing for the keeper, whichever way it began -- see Crossing.
GuiApp::Crossed GuiApp::run_crossing(Crossing how, CrashWatch& watch,
                                     const CrashWatch::Settings& watch_settings,
                                     AutoLogin::Settings& login, TemplateStore& store) {
    const bool key = how == Crossing::Key;

    // Read again for every crossing. A channel or a character picked on the
    // tab while the keeper runs is meant for the next one -- the tab says
    // GoBot goes to the one shown there -- and a file that will not read
    // just now leaves the last copy that did. Kept as the keeper's own, for
    // the watchdog and the crossings after this one.
    bool other_pictures = false;
    {
        AutoLogin::Settings fresh;
        if (fresh.load(exe_relative("configs/login.ini"))) {
            other_pictures = AutoLogin::picture_names(fresh) != AutoLogin::picture_names(login);
            login = fresh;
        }
    }
    if (!login.unidentified().empty()) {
        std::string pages;
        for (const AutoLogin::Page* page : login.unidentified())
            pages += (pages.empty() ? "" : ", ") + page->key;
        keeper_give_up("the crossing is not set up, so it cannot be made -- see configs/login.ini",
                       AutoLogin::advice(AutoLogin::Why::NotSetUp, pages));
        alert("keeper_gave_up", alert_text::gave_up_not_set_up());
        return Crossed::GaveUp;
    }
    // The game and VTCZone run as administrator, and Windows drops clicks
    // sent to them from a program that does not.
    if (!elevated_) {
        keeper_give_up("GoBot is not running as administrator, so its clicks cannot reach VTCZone or the game",
                       "GoBot không chạy bằng quyền Administrator nên không bấm được vào VTCZone/game.");
        alert("keeper_gave_up", alert_text::gave_up_not_elevated());
        return Crossed::GaveUp;
    }
    // A page given another picture since then is a picture not kept.
    if (other_pictures) {
        keeper_note("configs/login.ini names other pictures now -- loading them");
        store = keeper_pictures(login);
    }

    switch (how) {
        case Crossing::Cold:
            keeper_note("starting the game -- it was not running when this began");
            break;
        case Crossing::Restart:
            keeper_note("logging back in (" + std::to_string(keeper_restarts_.load()) + " of " +
                        std::to_string(watch_settings.max_restarts) + ")");
            break;
        case Crossing::Key:
            keeper_note("login key: crossing now");
            break;
    }

    AutoLogin flow;
    flow.set_settings(login);
    flow.set_templates(&store);
    flow.set_debug_dir(exe_relative("login_debug"));
    // A game that comes up part way and stays on a screen the crossing does
    // not know is ended the way a crashed one is, and the next attempt starts
    // from VTCZone: the game closed to be entered again, so a restart however
    // this crossing began. Out of them, nothing is ended and the crossing
    // stops for a person. Only by a keeper the player ticked "Tự vào lại
    // game" for: one started for the login key alone never ends the game --
    // the player asked for a crossing, not for the client to be closed -- and
    // a stuck attempt there simply fails.
    bool out_of_restarts = false;
    if (!keeper_oneshot_)
        flow.set_end_client(
            [this, &watch, &watch_settings, &flow, &out_of_restarts] {
                // Not after a stop: the crossing may be on its way out when F12
                // comes, and still judge the game stuck.
                if (keeper_stop_) return false;
                if (!keeper_take_restart(watch_settings)) {
                    out_of_restarts = true;
                    flow.cancel();
                    return false;
                }
                keeper_note("ending a game stuck part way in (" + std::to_string(keeper_restarts_.load()) +
                            " of " + std::to_string(watch_settings.max_restarts) + ")");
                const bool ended = watch.shut_down(CrashWatch::Look{});
                // Counted already: keeper_loop does not count it again if the
                // crossing is cancelled and the next look finds it gone.
                if (ended) keeper_closed_counted_ = true;
                return ended;
            },
            watch_settings.cooldown_s);
    const std::string doing = how == Crossing::Cold ? "Đang mở game và vào nhân vật: "
                            : key                   ? "Đang vào game (phím vào game ngay): "
                                                    : "Đang vào lại game: ";
    flow.on_step([this, doing, &flow](AutoLogin::Step s, const std::string& detail) {
        say_keeper(std::string(AutoLogin::step_name(s)) + ": " + detail);
        if (s == AutoLogin::Step::Blocked || s == AutoLogin::Step::InWorld) return;
        std::string vi = doing + AutoLogin::step_name_vi(s) + "…";
        // Waiting on a window in the way, not stuck: said while it waits,
        // so the player at the screen knows what to move.
        if (flow.holding())
            vi += " " + (flow.hiding() ? AutoLogin::hidden_advice(flow.held_by())
                                       : AutoLogin::advice(AutoLogin::Why::Covered, flow.held_by()));
        say_keeper_vi(vi);
    });
    {
        std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
        keeper_flow_ = &flow;
        // The login key pressed again in the moment between asking and this:
        // stopped. A crossing the keeper began is not the key's to stop.
        if (key && keeper_key_cancel_) flow.cancel();
    }
    const bool in = !keeper_stop_ && flow.run();
    {
        std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
        keeper_flow_ = nullptr;
    }
    if (keeper_stop_) return Crossed::Stopped;
    // Said and alerted by keeper_take_restart; the game is left as it is.
    if (out_of_restarts) return Crossed::GaveUp;
    if (key && keeper_key_cancel_.exchange(false)) {
        keeper_note("login key: cancelled");
        say_keeper_vi("Đã huỷ vào game.");
        return Crossed::Cancelled;
    }
    if (!in) {
        // Stop and ask: the crossing has already used its attempts, and the
        // next try is for a person to decide.
        keeper_give_up((how == Crossing::Restart ? "could not get back in: " : "could not get in: ") +
                           flow.trouble(),
                       flow.advice());
        alert("keeper_gave_up", alert_text::gave_up_crossing(flow.attempts_made(), flow.why()));
        return Crossed::GaveUp;
    }
    keeper_closed_counted_ = false;
    keeper_note(key ? "login key: in the world" : "back in the world");
    if (how == Crossing::Restart)
        alert("back_in", alert_text::back_in(keeper_restarts_, watch_settings.max_restarts));
    say_keeper_vi(how == Crossing::Restart ? "Đã vào lại game. Đang theo dõi game."
                  : keeper_oneshot_        ? "Đã vào game."
                                           : "Đã vào game. Đang theo dõi game.");
    keeper_reattach_ = true;
    crossed_in_ms_ = now_ms();      // the check-in board comes up now: pictured 15 s on
    return Crossed::In;
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
    if (!find_and_retarget(app_config_.window_title, app_config_.window_class,
                           app_config_.process_name)) {
        // Said once: a cold start waiting on the window asks every few seconds.
        if (!reattach_missed_)
            GB_WARN("The game is up, but its window could not be found -- looking again");
        reattach_missed_ = true;
        return false;
    }
    reattach_missed_ = false;
    GB_INFO("Following the game window the keeper found");
    set_status("Following the game window", 6000);
    return true;
}

// ----------------------------------------------------------------- reviving

void GuiApp::say_revive(const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        revive_log_.push_back(line);
        if (revive_log_.size() > 200) revive_log_.erase(revive_log_.begin());
    }
    request_redraw();       // shown at once, as say_party's
}

void GuiApp::start_revive() {
    vitals_for_job();       // HP and MP at the run's pace before any click
    if (revive_running_) return;
    join_pumping(revive_thread_);
    revive_stop_ = false;
    revive_running_ = true;
    revive_thread_ = std::thread([this] { revive_loop(); });
}

void GuiApp::stop_revive(bool wait) {
    revive_stop_ = true;
    if (!wait) return;
    join_pumping(revive_thread_);
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
    name_this_thread("revive");
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
    capture.set_label("revive");
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
    // A revive ends the minute after a death panel in which the HP/MP reader
    // keeps the run's pace whatever the packets say (death_seen_ms_).
    reviver.on_revived([this] {
        ++revive_count_;
        death_seen_ms_ = 0;
    });
    // The phone hears of a death once, when its panel is first seen, and of
    // the revive once the Reviver is sure the character is up. Three failed
    // tries are told by stop_after_revive alone, with why the bot stopped.
    // The panel also keeps the HP/MP reader at the run's pace for a minute,
    // and what the packets said of HP is written beside it: death stays the
    // screen's, and the log is how a later plan proves a packet HP of 0.
    reviver.on_death([this] {
        death_seen_ms_ = now_ms();
        GB_INFO("Vitals: the death panel is up -- %s", packet_vitals_text().c_str());
        alert("death", alert_text::death(true));
    });
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
    simple_cold_start_ = false;
    const bool bin_note = stop_jobs(true);
    simple_track_run(false);
    if (!bin_note) simple_note("Đã dừng.");
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
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        trash_log_.push_back(line);
        if (trash_log_.size() > 200) trash_log_.erase(trash_log_.begin());
    }
    request_redraw();       // shown at once, as say_party's
}

void GuiApp::start_trash() {
    vitals_for_job();       // HP and MP at the run's pace before any key
    if (trash_running_) {
        trash_oneshot_ = false;          // a one-off pass in progress becomes a run
        return;
    }
    join_pumping(trash_thread_);
    trash_stop_ = false;
    trash_oneshot_ = false;
    trash_running_ = true;
    trash_thread_ = std::thread([this] { trash_loop(); });
}

void GuiApp::request_trash(bool look_only) {
    vitals_for_job();       // a one-shot pass watches HP too: at the run's pace before any key
    trash_request_ = look_only ? 2 : 1;
    if (trash_running_) return;
    join_pumping(trash_thread_);
    trash_stop_ = false;
    trash_oneshot_ = true;
    trash_running_ = true;
    trash_thread_ = std::thread([this] { trash_loop(); });
}

void GuiApp::request_charms() {
    vitals_for_job();       // a one-shot pass watches HP too: at the run's pace before any key
    trash_request_ = 3;
    if (trash_running_) return;
    join_pumping(trash_thread_);
    trash_stop_ = false;
    trash_oneshot_ = true;
    trash_running_ = true;
    trash_thread_ = std::thread([this] { trash_loop(); });
}

void GuiApp::request_shop(bool dry) {
    vitals_for_job();       // a one-shot pass watches HP too: at the run's pace before any key
    trash_request_ = dry ? 5 : 4;
    if (trash_running_) return;
    join_pumping(trash_thread_);
    trash_stop_ = false;
    trash_oneshot_ = true;
    trash_running_ = true;
    trash_thread_ = std::thread([this] { trash_loop(); });
}

bool GuiApp::stop_trash(bool wait) {
    // A bag job's key window shut first, and forgotten: no key of the macro's
    // goes through it from here, and the job cannot open it again
    // (2026.11.10).
    coord_.disarm_pass_keys();
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
    // Told, and not waited for: stop_jobs tells every job first.
    if (!wait) return false;
    join_pumping(trash_thread_);
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
    // The executor presses only while the packet bot runs, and stop_bot()
    // stops it with the bot. It is paused as well, before the hold ends below,
    // until the bin is seen with nothing waiting -- looked for every couple of
    // seconds, by watch_left_held when no trash worker runs: see
    // bin_seen_clear.
    if (trash_hold_.exchange(false)) {
        stop_hunt();
        stop_macro();
        stop_bot();
        stop_calibration();
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
    capture.set_label("bag");
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
    // A protocol calibration walks the character itself for its twenty
    // seconds. Anything started meanwhile -- the hunter, a walk, the packet
    // bot -- lets go of the arrow it holds and spoils what it measures.
    if (calibrator_ && calibrator_->running())
        return refuse("Đang chạy Calibrate (dò giao thức): chờ nó xong, hoặc bấm Abort, rồi bấm lại.");
    // A Tự chat worker told to stop with a line under way looks at the line
    // it leaves, and may yet hand it over as maybe open: what started
    // meanwhile would type into it.
    if (chat_running_ && chat_quit_)
        return refuse("Đang dừng Tự chat. Chờ một chút rồi bấm lại.");
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
    // Without one, nothing else would look until BẮT ĐẦU, and whatever waits
    // on the doubt would wait all session for a player who answered the bin's
    // dialog and went on by hand: a quest round, the popup closer.
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
        GB_INFO("Trash: the game shows nothing waiting in the bin any more; the executor's pause "
                "for it ends");
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
    if (!coord_.critical_active() || coord_.critical_reason() != reason) return;
    // A hold let go of while everything is being stopped passes to the stop,
    // with no moment free in between. A quest round, a popup's close or a
    // revive told to stop ends its own within a fraction of a second, and the
    // hunter and the macro, waited for only later, would otherwise be free to
    // press skills and potions after F12.
    if (stopping_ && reason != kStopHold) {
        GB_INFO("Hold '%s' let go of during a stop: kept on as the stop's", reason.c_str());
        coord_.engage_critical(kStopHold);
    } else {
        coord_.disengage_critical();
    }
}

bool GuiApp::stop_jobs(bool with_bot) {
    // Everything that presses keys through the coordinator -- the hunter, the
    // macro, a walk to a place, the packet bot's executor, the jump key's
    // "Thử" -- refused from this moment to the end. The hunter and the macro
    // cannot be told to stop without being waited for, and while the others
    // were waited for they went on swinging and drinking. Not over a hold that
    // is somebody else's, which refuses them just the same -- and which, let
    // go of by its owner as it winds down, is the stop's from then on
    // (end_hold). Before anything, a bag job's key window shut and forgotten:
    // under the job's hold take_hold(kStopHold) below does nothing, and the
    // macro's letter keys would go on through it (2026.11.10).
    coord_.disarm_pass_keys();
    {
        std::lock_guard<std::mutex> lock(hold_mutex_);
        stopping_ = true;
    }
    take_hold(kStopHold);

    // ---- every one told, and nothing waited for ----
    if (with_bot && navigator_) navigator_->request_stop();
    if (calibrator_) calibrator_->request_stop();
    stop_chat(true, false);
    stop_party(false);
    stop_revive(false);
    stop_trash(false);
    stop_quest(false);
    stop_popups(false);
    stop_keeper(false);
    simple_style_stop();
    // The packet bot's executor: its queue and its route dropped, and nothing
    // more pressed. The bot itself is stopped below, with the rest.
    if (with_bot && executor_) executor_->halt();
    input_.release_all();

    // ---- then each one waited for: the engines first ----
    if (with_bot && navigator_) navigator_->stop();
    stop_hunt();
    stop_macro();
    if (with_bot) stop_bot();
    stop_calibration();
    stop_party();
    stop_revive();
    const bool bin_note = stop_trash();
    stop_quest();
    stop_popups();
    stop_keeper();

    // What went down while they wound down -- a press under way when they
    // were told -- let go of as well.
    input_.release_all();
    // The stop's hold ends. One its owner keeps beyond the stop is left on: the
    // revive's that ran out of tries, which stop_after_revive ends, and a chat
    // line in doubt handed over to tick().
    {
        std::lock_guard<std::mutex> lock(hold_mutex_);
        stopping_ = false;
    }
    end_hold(kStopHold);
    return bin_note;
}

/// The timed passes, and the ones asked for.
///
/// Every other part of the bot is held off for the length of a pass: the
/// hunter walking off with the bag open drags it out of reach, and a potion key
/// pressed while a quantity box has the keyboard types a number into it. But
/// for the macro's keys on a letter or Space, which go on through the hold's
/// key window between a pass's or a shop round's own clicks while nothing is
/// up (2026.11.10) -- never a charm run's.
void GuiApp::trash_loop() {
    name_this_thread("trash");
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
    capture.set_label("trash");
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
    // The hold kept by a potion purchase that left something up: let go only
    // once the shop, its message and its question are gone too, not only the
    // bin's dialogs -- see bag::shop_left_up -- and not while the check-in
    // board is up, which may hide them (run_pass's end check holds for that).
    bool shop_held = false;
    std::uint64_t held_looked_ms = 0;
    // The same from the start when the game already shows the bin asking, or
    // something waiting in it -- left by a pass a stop cut short, or by hand:
    // held off now, not from the first pass fifteen seconds on.
    if (!held) {
        Frame f;
        if (grab(f) && f.valid()) {
            const BagView v = bag::find(f, store, settings.layout);
            if ((bag::bin_asking(f, store) || (v.found && !v.bin_empty && v.bin_on_frame)) &&
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
        // Nor under the check-in board: a quantity box taken to be up when it
        // came over it -- 100 typed, not seen gone -- is out of sight of both
        // looks above. Held until the board is gone and a look shows nothing.
        if (shop_held && (bag::shop_left_up(f, store) || checkin_board_title(f, store))) return;
        const bool was_held = held;
        if (held) {
            held = false;
            shop_held = false;
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
    // Tự chat's line before any timed job here -- the player's order: chat,
    // then the bag (the pass, then the charms), then the potions; a shop due
    // as a pass ends rides it, before the charms (2026.11.11). None starts
    // while a line is due, its attempt included, or waits only for this
    // worker's last job. Not while this worker holds the game for something
    // left in the bin (the line waits on that, and only a pass ends it), not
    // over a line in doubt (the jobs refuse that anyway), and never past
    // kChatFirstMaxMs: a line that cannot go holds nothing up for long.
    const auto chat_first = [&] {
        if (held || trash_left_held_ || chat_doubt_) return false;
        const std::uint64_t now = now_ms();
        const std::uint64_t waits = chat_waits_trash_ms_.load();
        if (waits != 0 && now < waits + kChatFirstMaxMs) return true;
        const std::uint64_t due = chat_next_ms_.load();
        return due != 0 && now >= due && now < due + kChatFirstMaxMs;
    };
    // ---- the macro's letter and Space keys during a pass and a shop round ----
    // (2026.11.10, the owner: "Khi dọn rác thì cần ra các skill thuộc phím chữ
    // cái", "cả khi mua HP và MP cũng cần ra skill", "có tính phím space", "lúc
    // dùng bùa thì không cần ra skill"; 2026.11.11: "Home End PgUp PgDn Ins
    // Del", "Không cho phím số"): the hold's key window, through which the
    // macro's keys on a letter, Space or one of those six -- skills, buffs and
    // potions; never a digit -- go between the job's own clicks and keys
    // (BagCleaner::KeyWindow, pass_key_allowed). Built from
    // the macro's keys in memory (MacroEngine::pass_keys); never the bag's key,
    // the quest window's -- quest.ini read here, on this thread -- or GoBot's
    // own switches; none with the macro off. Never for a charm run, nor for a
    // job begun with something maybe left in the bin: those keep the whole hold.
    // The keys no bag job lets through nor presses of its own (2026.11.11b:
    // the job's own potion presses go by the same).
    const auto job_never = [&] {
        QuestSettings quest;
        quest.load(exe_relative("configs/quest.ini"));
        std::vector<std::uint32_t> never{ settings.bag_key, quest.quest_key, own_trash_key_.load(),
                                          own_login_key_.load() };
        if (hunter_) never.push_back(hunter_->config().toggle_key);
        return never;
    };
    const auto pass_keys_now = [&] {
        if (!macro_ || !macro_->running()) return PassKeys{};
        return macro_->pass_keys(job_never());
    };
    // ---- the HP and MP potions a bag job presses itself ----
    // (2026.11.11b, the owner: "làm cho có thể mua bình ở bất cứ mức máu nào và
    // vừa mua vừa bơm máu", "GoBot tự bấm bình", "Giữ chặn phím số", "Cả dọn
    // rác"): with the macro not running -- the bot stopped and "Mua ngay" or
    // "Dọn ngay" pressed, or the macro off -- nothing else presses them, so a
    // pass or a shop round does, by the macro's own potion lines (the tier for
    // the reading, each key's cooldown), on a key the key window would let
    // through: never a digit, which is said once a job. The cooldown is kept
    // here, across the jobs, as the macro keeps its own.
    std::map<std::uint32_t, std::uint64_t> own_potion_ms;
    const auto own_potions = [&] {
        BagCleaner::OwnPotions o;
        auto refused_said = std::make_shared<bool>(false);
        o.due = [this, never = job_never(), refused_said, &own_potion_ms](std::string& line) -> std::uint32_t {
            if (!macro_ || macro_->running()) return 0;
            const MacroEngine::OwnPotion p = macro_->own_potion_due(never);
            if (p.refused != 0 && !*refused_said) {
                *refused_said = true;
                GB_INFO("Trash: a potion due on key %s is not pressed by a bag job -- letters, Space and "
                        "Home/End/PgUp/PgDn/Insert/Delete only, never a digit",
                        InputManager::key_name(p.refused).c_str());
            }
            if (p.vk == 0) return 0;
            const auto at = own_potion_ms.find(p.vk);
            if (at != own_potion_ms.end() && now_ms() < at->second + p.cooldown_ms) return 0;
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s on %s, %s %.0f%% at or under its mark %d%%", p.hp ? "HP" : "MP",
                          InputManager::key_name(p.vk).c_str(), p.hp ? "HP" : "MP", static_cast<double>(p.pct),
                          p.mark);
            line = buf;
            return p.vk;
        };
        o.pressed = [&own_potion_ms](std::uint32_t vk) { own_potion_ms[vk] = now_ms(); };
        return o;
    };
    const auto key_window = [this] {
        BagCleaner::KeyWindow w;
        w.open = [this] { return coord_.open_pass_keys(); };
        w.shut = [this](int ms) { return coord_.close_pass_keys(static_cast<std::uint32_t>(std::max(0, ms))); };
        w.potion_ago_ms = [this] {
            const std::uint64_t at = coord_.pass_potion_ms();
            const std::uint64_t now = now_ms();
            return at == 0 ? -1 : static_cast<int>(std::min<std::uint64_t>(now > at ? now - at : 0, 3'600'000));
        };
        // An HP or MP potion due whose key this hold's window lets through --
        // never the pet's -- for the job's potion breaks (2026.11.11).
        w.potion_due = [this] {
            return macro_ && macro_->vital_potion_due([this](std::uint32_t vk) { return coord_.pass_key_armed(vk); });
        };
        // Home, End, PgUp, PgDn, Insert or Delete let through and come up
        // (2026.11.11): the job's input of its own waits a moment after it.
        w.nav_ago_ms = [this] {
            const std::uint64_t at = coord_.pass_nav_ms();
            const std::uint64_t now = now_ms();
            return at == 0 ? -1 : static_cast<int>(std::min<std::uint64_t>(now > at ? now - at : 0, 3'600'000));
        };
        return w;
    };
    // ---- charms (Bùa) ----
    // A job of its own beside the timed passes and the ones asked for: the
    // charms on the Dùng list that are due -- one of each kind an hour, by
    // CharmClock -- used in a quick run that opens the bag on Tiêu Dùng and
    // nothing else, with everything that presses keys held off for the few
    // seconds it takes (the macro's skills, potions and buffs, the hunter's
    // attack). Never inside the slow pass that clears the bag. `forced` is the
    // button "Dùng bùa ngay": the clock is not asked, and what stops it is
    // said instead of waited out.
    CharmClock charm_clock;
    charm_clock.load();
    // The charms' and the shop's clocks as the last worker left them: a MACRO
    // OFF and ON in between starts a new worker, and each start used to bring
    // the charm check 8 s on and the shop's look 45 s on whatever was due --
    // the ten minutes after a tab not read, the shop's wait after a spoiled
    // quiet second. Kept, but never sooner than a start's own wait; and from
    // that wait after its box is ticked on again, the worker gone meanwhile.
    TrashClocks kept;
    {
        std::lock_guard<std::mutex> lock(trash_clocks_mutex_);
        kept = trash_clocks_;
    }
    unsigned charm_ticks = charm_ticked_.load(), shop_ticks = shop_ticked_.load();
    if (kept.charm_ticks_seen != charm_ticks) kept.charm_check_ms = 0;
    if (kept.shop_ticks_seen != shop_ticks) {
        kept.shop_due_ms = 0;
        kept.shop_quiet_level = 0;
        // Ticked on again: nothing owed, the plan's time from the file.
        kept.owed_hp = kept.owed_mp = 0;
        kept.plan_due_ms = 0;
        kept.shop_troubles = 0;
    }
    std::uint64_t charm_check_ms = resumed_clock(kept.charm_check_ms, now_ms(), kCharmFirstMs);
    // A message of the game's after a charm's Xác Nhận that the last run left
    // up, or may have (BagCleaner::Result::notice_left): its question's pair,
    // and when it was left -- 0 for none. The next run closes it, for two
    // minutes; after that a message there is anyone's, and left alone.
    ScreenRect charm_notice_place;
    std::uint64_t charm_notice_ms = 0;
    std::vector<std::string> charm_listed;       // clock ids of the charms on the list, as last built
    std::uint64_t charm_listed_ms = 0;
    bool charm_listed_built = false;
    const auto wall_s = [] {
        return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch()).count());
    };
    // ---- the check-in board ----
    // A bag job that found the "Sự kiện và điểm danh" board up pressed nothing
    // (BagCleaner::Result::board_up) and is looked at again kBoardWaitMs on --
    // not trouble. Said once a sighting, until a job runs with no board; a job
    // asked for with a button says it each time. With "Tự đóng thông báo" off
    // nothing closes the board, and the line says so: app.ini read here, on
    // this worker's thread.
    bool board_said = false;
    const auto board_put_off = [&](const char* job, bool asked) {
        if (board_said && !asked) return;
        const bool told_before = board_said;
        board_said = true;
        IniFile app;
        const bool closes = app.load(exe_relative("configs/app.ini")) && app.get_bool("Simple", "close_popups", false);
        if (!told_before)
            GB_INFO("Trash: the check-in board is up -- %s put off %d s%s", job, BagCleaner::kBoardWaitMs / 1000,
                    closes ? "" : " (close_popups is off: nothing closes it)");
        tell("Đang có bảng Sự kiện và điểm danh: việc túi đồ chờ " + std::to_string(BagCleaner::kBoardWaitMs / 60'000) +
             " phút." + (closes ? "" : " Bật \"Tự đóng thông báo\" (thẻ Đánh quái) để GoBot tự tắt bảng điểm danh."));
    };
    const auto run_charms = [&](bool forced) {
        // The file again: the tab writes it as boxes are ticked.
        for (int i = 0; i < 5; ++i) {
            CleanSettings fresh;
            if (fresh.load(config)) { settings = fresh; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        read_ms = now_ms();
        const auto not_now = [&](const std::string& why) {
            if (forced) tell(why);
            charm_check_ms = now_ms() + (forced ? 0 : 5'000);
        };
        if (!settings.use_charms) {
            if (forced) tell("Dùng bùa đang tắt. Đánh dấu \"Dùng bùa\" ở đầu thẻ này.");
            return;
        }
        // Which charms are on the list, asked of the files only now and then:
        // building the list reads every picture and weighs it against the
        // catalog, too much to do every five seconds to learn nothing is due.
        // The whole list is built below, when a charm is.
        TrashList list;
        std::vector<std::string>& listed = charm_listed;
        if (forced || now_ms() >= charm_listed_ms + 30'000) {
            list.build(exe_relative("configs/trash"), settings, catalog);
            listed.clear();
            for (const TrashList::Item& it : list.items)
                if (it.action == TrashList::Action::Use && it.charm() &&
                    std::find(listed.begin(), listed.end(), it.clock_id()) == listed.end())
                    listed.push_back(it.clock_id());
            charm_listed_ms = now_ms();
            charm_listed_built = true;
        } else {
            charm_listed_built = false;
        }
        if (listed.empty()) {
            if (forced)
                tell("Chưa chọn bùa nào trong danh sách Dùng. Bấm vào một bùa ở phần \"Chọn đồ cần bỏ\", rồi "
                     "chọn \"Dùng bùa\".");
            return;
        }
        charm_clock.load();      // another GoBot may have used one since
        // A message left by the last run, two minutes at most: a reason to run
        // with nothing due -- such a run closes it and opens nothing.
        if (charm_notice_ms != 0 && now_ms() >= charm_notice_ms + 120'000) charm_notice_ms = 0;
        const bool notice_kept = charm_notice_ms != 0;
        bool any_due = forced || notice_kept;
        int due_kinds = forced ? static_cast<int>(listed.size()) : 0;
        for (const std::string& id : listed) {
            const bool due = charm_clock.due(id, wall_s());
            any_due = any_due || due;
            if (!forced && due) ++due_kinds;
        }
        if (!any_due) return;
        // What the run cannot be done without, before it takes the hold: a
        // run that stops for want of it would hold the bot off every few
        // seconds for nothing.
        if (!bag::can_check_charm(store)) {
            charm_check_ms = now_ms() + 60'000;
            if (forced) tell("Thiếu hình của game để dùng bùa (xem thẻ Dọn rác).");
            return;
        }
        if (!input_.window_valid()) return not_now("Chưa thấy cửa sổ game.");
        if (chat_doubt_) return not_now(kTrashWaitsForChat);
        if (coord_.critical_active() && coord_.critical_reason() != "trash cleaning")
            return not_now("Chờ: bot đang tạm dừng (" + coord_.critical_reason() + "), dùng bùa sau.");
        // Only with the character in the world: nothing is read of its HP at
        // the login or the character screen, and the bag key pressed there is
        // a letter in a form.
        if (keeper_cold_start_) return not_now("Chờ: GoBot đang vào lại game, dùng bùa sau.");
        {
            std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
            if (keeper_flow_ != nullptr) return not_now("Chờ: GoBot đang qua màn hình khác, dùng bùa sau.");
        }
        // Not while HP is near what the HP potion drinks at: the potion is
        // held off with everything else for the length of the run.
        {
            const float hp = hp_percent_now();
            if (hp < 0.0f) return not_now("Chưa đọc được máu của nhân vật, dùng bùa sau.");
            if (hp < static_cast<float>(chat_hp_floor_.load() + 10)) return not_now("Máu đang thấp, dùng bùa sau.");
        }
        // The clock has to be writable: a charm used and not written down is a
        // charm used again after a restart, and the next charm after it. Asked
        // now, before anything is right-clicked.
        charm_clock.load();
        if (!charm_clock.save()) {
            charm_check_ms = now_ms() + 300'000;
            tell("Không ghi được " + CharmClock::default_path() + ", nên không dùng bùa (một bùa đã dùng mà không ghi "
                 "lại sẽ bị dùng thêm lần nữa).");
            return;
        }
        if (!charm_listed_built) list.build(exe_relative("configs/trash"), settings, catalog);

        BagCleaner cleaner(input_, store, grab);
        cleaner.set_settings(settings);
        cleaner.set_trash(&list);
        cleaner.set_counts(&counts);
        cleaner.set_debug_dir(exe_relative("trash_debug"));
        cleaner.set_mode(BagCleaner::Mode::CharmsOnly);
        cleaner.set_charm_due([&](const std::string& id) { return forced || charm_clock.due(id, wall_s()); });
        // Everything the bot presses is held off while this runs; if that
        // hold is taken from it, it stops there.
        // And HP not fallen under the potion's mark, which is held off as well:
        // the run stops there and gives the keys back.
        cleaner.set_guard([this] {
            if (!(coord_.critical_active() && coord_.critical_reason() == "trash cleaning")) return false;
            const float hp = hp_percent_now();
            return !(hp >= 0.0f && hp < static_cast<float>(chat_hp_floor_.load()));
        });
        // Each charm written down as used the moment before it is right-clicked
        // -- the time it had is kept, to give back if nothing came of it -- so
        // that a GoBot closed or killed in the middle has not forgotten it.
        std::map<std::string, std::int64_t> claimed_before;
        cleaner.set_charm_claim([&](const std::string& id) {
            claimed_before[id] = charm_clock.next(id);
            if (!charm_clock.set_next(id, CharmClock::next_after(CharmClock::How::MaybeUsed,
                                                                 settings.charm_every_minutes, wall_s())))
                cleaner.cancel();       // not written down: nothing more is right-clicked
        });
        // The packets' bag on each charm's slot before its right-click, read
        // only: a slot it names another item in is left alone, and so is a
        // stack of one it says holds another count -- it never makes a
        // right-click happen (packets_bag.cpp).
        cleaner.set_charm_packets([this](const std::string& colour_id, int slot, int count, std::string& said) {
            return bag_charm_slot(colour_id, slot, count, said);
        });
        // The last run's message, closed with its own Xác Nhận before anything
        // else -- the bag key included.
        if (notice_kept) cleaner.set_own_notice(charm_notice_place);
        cleaner.on_report([this](const std::string& line) { say_trash(line); });
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = &cleaner;
        }
        if (trash_stop_) cleaner.cancel();

        BagCleaner::Result r;
        bool passed = false;
        const std::uint64_t asked_ms = now_ms();
        std::uint64_t held_ms = 0;
        {
            // The mouse, as a pass takes it, waited for a little at a time:
            // a charm job is quick and not wanted late.
            std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::defer_lock);
            const std::uint64_t give_up = now_ms() + 5'000;
            while (!trash_stop_ && now_ms() < give_up && !mouse.try_lock())
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (mouse.owns_lock() && !trash_stop_ && !chat_doubt_ && take_hold("trash cleaning")) {
                passed = true;
                trash_pass_ = true;
                held_ms = now_ms();
                tell("Dùng bùa...");
                // What is mid-press when the hold comes is let finish, and
                // whatever is still down let go of.
                for (int i = 0; i < 30 && coord_.held(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                input_.release_all();
                if (!input_.window_focused()) {
                    input_.focus_window();
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
                // About 3 s a kind due, and 20 at least for the bag, its tab
                // and its pages: 20 to 40 s.
                cleaner.set_deadline_ms(BagCleaner::charm_deadline_ms(due_kinds));
                r = cleaner.run();
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
            if (!trash_stop_) not_now("Chờ: bot đang bận (" + (coord_.critical_active() ? coord_.critical_reason()
                                                                                      : std::string("chuột đang dùng")) +
                                      "), dùng bùa sau.");
            return;
        }
        // The check-in board up as the run began, on its first look: nothing
        // pressed, claimed or used, and a message handed over kept for the
        // next run -- which comes in a minute, not in five seconds.
        if (r.board_up && !r.ran && claimed_before.empty()) {
            charm_check_ms = now_ms() + BagCleaner::kBoardWaitMs;
            board_put_off("charms", forced);
            return;
        }
        if (r.ran) board_said = false;
        // The message this run left up, or may have, for the next: its two
        // minutes from the end of the run that left it, not renewed by a run
        // that only found it still there.
        if (!r.notice_left) {
            charm_notice_ms = 0;
        } else if (!r.notice_carried || charm_notice_ms == 0) {
            charm_notice_place = r.notice_place;
            charm_notice_ms = now_ms();
        }

        // The clock: every charm right-clicked, by how it came out -- a charm
        // perhaps used counts as used, for one used twice is spent early --
        // and every due charm the bag was searched for and not found, to be
        // looked for again in ten minutes rather than every few seconds.
        const std::int64_t done_s = wall_s();
        std::set<std::string> tried;
        bool unsaved = false;
        for (const BagCleaner::Result::CharmTry& t : r.charm_tries) {
            tried.insert(t.id);
            std::int64_t at = CharmClock::next_after(t.how, settings.charm_every_minutes, done_s);
            // Not used: tried again soon -- but never sooner than the time it
            // had before this run claimed it.
            if (t.how == CharmClock::How::NotUsed) at = std::max(at, claimed_before[t.id]);
            unsaved = !charm_clock.set_next(t.id, at) || unsaved;
            // The packets' bag beside it, for the log only -- the clock goes by
            // the screen, as above: whether the charm's stack came out one less
            // since the hold was taken (packets_bag.cpp).
            const std::string by_bag = bag_charm_text(t.id, held_ms);
            if (!by_bag.empty()) {
                GB_INFO("Trash: charm %s %s %s", t.id.c_str(),
                        t.how == CharmClock::How::Used        ? "seen used"
                        : t.how == CharmClock::How::MaybeUsed ? "taken for used"
                                                              : "not used",
                        by_bag.c_str());
                if (by_bag.find("one less") != std::string::npos) tell("Bùa " + t.id + " (theo gói tin: đã dùng).");
            }
        }
        // Claimed and never right-clicked (the run stopped first): as it was.
        for (const auto& c : claimed_before)
            if (tried.count(c.first) == 0) unsaved = !charm_clock.set_next(c.first, c.second) || unsaved;
        if (r.ran) {
            // Due and not found in the bag: looked for again in ten minutes,
            // not every few seconds -- and never earlier than a time it has.
            // Not a run stopped on the strip not reading as Tiêu Dùng: nothing
            // was looked for, and the run waits as long below.
            for (const std::string& id : listed)
                if (tried.count(id) == 0 && claimed_before.count(id) == 0 && (forced || charm_clock.due(id, done_s)) &&
                    r.trouble.empty() && !r.tab_unsure)
                    unsaved = !charm_clock.set_next(id, std::max(charm_clock.next(id),
                                                                 CharmClock::next_after(CharmClock::How::NotUsed, 0,
                                                                                        done_s))) || unsaved;
        }
        // A run that stopped with something wrong and tried no charm, or never
        // found the bag: not again every few seconds, which would hold the bot
        // off each time. A run only to close the last one's message, the bag
        // left shut, is neither. Nor is a run cut short by DỪNG / F10: its
        // clock as it was, which the next worker resumes from its start's 8 s.
        const bool only_message = !forced && due_kinds == 0;
        if (!forced && !trash_stop_ && ((!r.ran && (!only_message || !r.trouble.empty())) ||
                                        (!r.trouble.empty() && r.charm_tries.empty())))
            charm_check_ms = now_ms() + 60'000;
        // The strip not read as Tiêu Dùng -- at the tab's click or in the
        // sweep: ten minutes, not one.
        if (!forced && r.tab_unsure) charm_check_ms = now_ms() + kCharmTabRetryMs;
        if (unsaved)
            tell("Không ghi được " + CharmClock::default_path() + ": giờ dùng bùa chỉ nhớ đến khi tắt GoBot.");

        // Slots drawn like a charm chosen and left alone -- another colour, a
        // colour not sure, the packets' bag naming something else there, no
        // count read -- said when nothing was used: else they read as no
        // charm at all. Slots of a kind due with no count read, said whatever
        // else the run did: they are charms chosen and not used.
        std::string left_alone;
        const auto left = [&](int n, const char* what) {
            if (n > 0) left_alone += (left_alone.empty() ? "" : "; ") + std::to_string(n) + what;
        };
        left(r.charms_other_colour, " ô là bùa cùng hình khác màu (để nguyên)");
        left(r.charms_colour_doubt, " ô không chắc màu, không dùng");
        left(r.charms_vetoed, " ô bị gói tin chặn");
        left(r.charms_unread, " ô bùa không đọc được số lượng, chưa dùng");
        // The sweep stopped on the strip not reading as Tiêu Dùng (the tab's
        // click says its own trouble): said as it is -- no charm was looked
        // for, which is not a bag with none in it.
        const bool tab_stop = r.tab_unsure && r.trouble.empty();
        const std::string tab_line = std::string("không chắc túi đồ đang ở ngăn Tiêu Dùng, nên dừng dùng bùa") +
                                     (forced ? "." : "; thử lại sau 10 phút.");
        std::string line;
        if (r.charms_used > 0) {
            line = "Đã dùng " + std::to_string(r.charms_used) + " bùa." + (tab_stop ? " Dừng: " + tab_line : "");
        } else if (only_message) {
            line = r.charm_notices > 0 ? "Đã đóng thông báo của game sau lần dùng bùa trước." : "";
        } else if (tab_stop) {
            line = "Chưa dùng bùa nào: " + tab_line;
        } else if (r.charms_seen == r.charms_vetoed + r.charms_unread && r.trouble.empty()) {
            line = left_alone.empty() ? std::string("Không thấy bùa nào trong ngăn Tiêu Dùng của túi đồ.")
                   : r.charms_unread > 0 ? "Không dùng được bùa nào; " + left_alone + "."
                                         : "Không thấy loại bùa đã chọn; " + left_alone + ".";
        } else if (r.charm_tries.empty() && r.trouble.empty()) {
            line = std::string("Bùa chưa tới giờ dùng lại") + (r.charms_unread > 0 ? "; " + left_alone : "") + ".";
        }
        // The unread slots, where the line above has not said them already.
        if (r.charms_unread > 0 && line.find(" ô bùa không đọc được số lượng") == std::string::npos)
            line += (line.empty() ? "" : " ") + std::to_string(r.charms_unread) +
                    " ô bùa không đọc được số lượng, chưa dùng.";
        if (!r.trouble.empty()) line += (line.empty() ? "" : " ") + std::string("Dừng: ") + r.trouble + ".";
        if (!line.empty()) tell(line);
        GB_INFO("Trash: charm job: asked at %llu ms, hold taken after %llu ms, took %llu ms of %d, %d used, %d seen, %d "
                "not due, %d kinds due, %d messages closed, %d other colour, %d colour doubt, %d vetoed, %d count "
                "unreadable%s%s",
                static_cast<unsigned long long>(asked_ms), static_cast<unsigned long long>(held_ms - asked_ms),
                static_cast<unsigned long long>(now_ms() - held_ms), BagCleaner::charm_deadline_ms(due_kinds),
                r.charms_used, r.charms_seen, r.charms_waiting, r.charms_due, r.charm_notices, r.charms_other_colour,
                r.charms_colour_doubt, r.charms_vetoed, r.charms_unread,
                r.notice_left ? ", a message left for the next run" : "",
                r.tab_unsure ? ", stopped on the bag's strip not read as Tiêu Dùng" : "");
        if (held) {
            held_looked_ms = now_ms();
            tell("Món đồ có thể vẫn đang ở trong một hộp thoại. Bấm Hủy Bỏ trong game; đánh quái, bình và buff "
                 "tạm dừng đến khi không còn gì chờ.");
        }
    };

    // ---- the potion shop ----
    // Every buy_every_minutes a plan: buy_hp_slots purchases of 100 of Thịt bò
    // khô and buy_mp_slots of Rượu lá tre, taking turns, from the shop the
    // Người Giữ Sân item opens -- BagCleaner::Mode::ShopBuy. Nothing in the bag
    // is counted (the owner, 2026-11-09): what is still owed of the plan is
    // kept here, and the game's message that the bag is full ends it -- the
    // next plan buys again. A timed round makes kShopBuysPerHold purchases at
    // most, the rest kShopPartGapMs later; "Mua ngay" a fresh plan, up to
    // kShopMaxBuys of it in its one hold and the rest as the timed rounds buy
    // it -- the numbers have no cap of their own since 2026.11.10 (the owner:
    // "Không giới hạn"). Everything that presses keys is held off for as long
    // as a round takes -- the skills too, which would otherwise press into the
    // quantity box and the shop -- but for the macro's keys the round's key
    // window lets through between purchases (2026.11.10). An HP potion among
    // them drinks there, an HP or MP potion due given a moment before each
    // double-click, and the round starts at the HP potion's mark and stops
    // only under half of it (2026.11.11, bag::hp_gates); one whose key waits
    // starts with HP well over the mark and stops when it falls under it.
    // `forced` is the button "Mua ngay" (the time is not asked), `dry` the
    // button "Thử mua": everything up to the quantity box, answered Hủy Bỏ.
    // `after_pass`: a timed round that rides the pass just ended (2026.11.11,
    // the shop right after the pass, below) -- the hold and the game's mouse
    // still the pass's, carried in, and neither the HP start gate nor the
    // quiet second asked -- HP not read, it waits as one on its own does;
    // whoever called it lets the hold go after it.
    //
    // When the next plan is due is written to shop_clock.txt as a plan begins,
    // before anything is clicked: a worker started with no clock of the last
    // one's waits what is left of it, 45 s at least and one interval at most
    // (bag::shop_first_wait_s) -- a GoBot started again never buys early. The
    // file is read and written on this thread only.
    CharmClock shop_clock(shop_clock_path());
    shop_clock.load();
    const auto shop_first_due = [&] {
        return now_ms() + 1000ull * static_cast<std::uint64_t>(bag::shop_first_wait_s(
                                        shop_clock.next(kShopClockId), wall_s(), settings.buy_every_minutes));
    };
    std::uint64_t shop_due_ms =
        kept.shop_due_ms != 0 ? resumed_clock(kept.shop_due_ms, now_ms(), kShopFirstMs) : shop_first_due();
    // The plan under way, as the last worker left it: what is owed, when the
    // next begins, and its rounds in a row that ended in trouble.
    bag::ShopOwed owed;
    owed.hp = kept.owed_hp;
    owed.mp = kept.owed_mp;
    owed.troubles = kept.shop_troubles;
    std::uint64_t plan_due_ms = kept.plan_due_ms;
    bool shop_was_on = settings.buy_potions;
    // How long a timed look waits after its quiet second was spoiled: longer
    // each time, from 30 s again after one that passed.
    QuietBackOff shop_quiet;
    shop_quiet.set_level(kept.shop_quiet_level);
    // "23:15": when `at_ms`, on now_ms()'s clock, comes by the PC's own.
    const auto clock_at = [&](std::uint64_t at_ms) {
        const std::uint64_t now = now_ms();
        const std::time_t t =
            static_cast<std::time_t>(wall_s() + static_cast<std::int64_t>(at_ms > now ? (at_ms - now) / 1000 : 0));
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
        return std::string(buf);
    };
    // The card's line (simple_shop.cpp), from atomics alone: the next look and
    // the next plan in wall-clock seconds, and what is owed -- set again only
    // when one of them moved. A worker here for one button's job does no timed
    // round: no times then.
    std::uint64_t shown_due = 0, shown_plan = 0;
    int shown_owed = -1;
    bool shown_timed = false;
    const auto show_shop = [&] {
        const int left = owed.hp + owed.mp;
        const bool timed_worker = !trash_oneshot_.load();
        if (shown_due == shop_due_ms && shown_plan == plan_due_ms && shown_owed == left && shown_timed == timed_worker)
            return;
        shown_due = shop_due_ms;
        shown_plan = plan_due_ms;
        shown_owed = left;
        shown_timed = timed_worker;
        const std::uint64_t now = now_ms();
        const auto wall_of = [&](std::uint64_t ms) {
            return wall_s() + static_cast<std::int64_t>(ms > now ? (ms - now + 999) / 1000 : 0);
        };
        shop_owed_ = left;
        shop_look_at_s_ = timed_worker ? wall_of(shop_due_ms) : std::int64_t{ 0 };
        // Nothing owed: the next look begins the next plan.
        shop_plan_at_s_ = !timed_worker ? std::int64_t{ 0 }
                          : left > 0 && plan_due_ms != 0 ? wall_of(plan_due_ms)
                                                         : wall_of(shop_due_ms);
    };
    // ---- the shop right after the pass (2026.11.11) ----
    // The owner: "Tới giờ mua: dọn rồi mua" (bag::ShopRide). A timed look of
    // the shop come due -- a plan, or a part still owed -- with Tự dọn rác on
    // pulls the pass forward to now, and the round rides it: run_shop with
    // `after_pass` right after the pass, the hold and the game's mouse carried
    // across with nothing let go in between, no quiet second -- the pass has
    // none, and clicked through the bag a moment before -- and no HP start gate
    // of its own; a look come due while a timed pass ran rides that one the
    // same way. No pass pulled when one ended less than bag::kRideFreshMs ago:
    // the shop goes on its own at once then. No ride after a pass that did not
    // run, or did not end clean: the shop goes on its own, by its own rules.
    // Never for a button, nor in a worker here for one: a Dọn ngay never buys.
    bag::ShopRide ride = bag::ShopRide::None;
    std::uint64_t last_pass_end_ms = 0;
    // No pass pulled before this time: one pulled found nothing it could do
    // -- the list empty, no tab ticked -- which its own clock, an interval
    // on, looks at again, as it did before 2026.11.11 (the review's R2).
    std::uint64_t no_pull_before_ms = 0;
    // Since the last pass ended, for the log: "312 s ago", or "none yet".
    const auto since_pass = [&] {
        return last_pass_end_ms == 0 ? std::string("none yet")
                                     : std::to_string((now_ms() - last_pass_end_ms) / 1000) + " s ago";
    };
    // Why a timed pass or shop look was last put off -- HP with its value and
    // the gate, another job's hold, the quiet second, the board -- said in the
    // log once, and again only when the reason changes: a look every 15 s says
    // nothing new each time. Forgotten once a job runs. The shop's pull of a
    // pass likewise (`pull_said`): said once until a job runs or a new reason
    // puts a look off (the review's R4), not again for each look the same
    // reason puts off.
    std::string put_off_why;
    bool pull_said = false;
    const auto put_off = [&](const char* job, const std::string& why, const std::string& said) {
        if (why == put_off_why) return;
        put_off_why = why;
        pull_said = false;
        GB_INFO("Trash: timed %s put off -- %s", job, said.c_str());
    };
    // HP and a job of the bag (2026.11.11, the owner: "Mua luôn, bơm xen"),
    // by the job's own key window (bag::hp_gates): every HP potion the macro
    // may choose from the mark down to half of it on a key the window lets
    // through (PassKeys::hp_all_through, the review's HP-1) -- the macro on,
    // the job not begun with something left in the bin -- and a timed pass or
    // shop look starts at the potion's own mark, the potion drinking between
    // the job's clicks, and the guard stops it only under half that mark;
    // otherwise -- one of them on a digit, say -- it starts over the window's
    // floor by a margin (95% at most: a mark at 91% and over would never let
    // one start) and stops under the floor, as before. chat_hp_floor_ stays
    // what chat and the other jobs go by.
    const auto hp_gates_of = [this](const PassKeys& keys) {
        return bag::hp_gates(keys.hp_all_through, keys.hp_mark, chat_hp_floor_.load());
    };
    const auto hp_said = [](float hp, int level, const char* what) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "HP %.0f%% under %s %d%%", static_cast<double>(hp), what, level);
        return std::string(buf);
    };
    // What a bag job's guard stopped it for, for the log: HP under the level,
    // as the guard saw them -- set by the guard, forgotten before each run. A
    // stop with none of it seen and the hold somebody else's was the hold's.
    struct HpCut {
        float hp = -1.0f;
        int   level = 0;
    };
    HpCut hp_cut;
    // The level the job under way stops at (bag::HpGates::stop), for the
    // line of a stop by the hold.
    int job_level = 0;
    const auto say_stopped = [&](const char* job, const HpCut& seen, const std::string& trouble) {
        if (trouble.empty() || trash_stop_) return;
        if (seen.level > 0) {
            GB_INFO("Trash: %s stopped by HP -- %s", job, hp_said(seen.hp, seen.level, "the level").c_str());
        } else if (!(coord_.critical_active() && coord_.critical_reason() == "trash cleaning")) {
            const std::string whose = coord_.critical_active() ? coord_.critical_reason() : std::string("nobody");
            GB_INFO("Trash: %s stopped by the hold -- %s's now; HP %.0f%%, the level %d%%", job, whose.c_str(),
                    static_cast<double>(hp_percent_now()), job_level);
        }
    };
    const auto run_shop = [&](bool forced, bool dry, bool after_pass) {
        const std::uint64_t due_before = shop_due_ms;
        // "Mua ngay" and "Thử mua": somebody at GoBot wants the shop now, and
        // the waits for a spoiled quiet second start again from the first.
        if (forced || dry) shop_quiet.reset();
        for (int i = 0; i < 5; ++i) {
            CleanSettings fresh;
            if (fresh.load(config)) { settings = fresh; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        read_ms = now_ms();
        const auto minutes = [&](int m) { return now_ms() + static_cast<std::uint64_t>(m) * 60'000ull; };
        const auto not_now = [&](const std::string& why, std::uint64_t again_ms = 5'000) {
            if (forced || dry) tell(why);
            shop_due_ms = forced || dry ? due_before : now_ms() + again_ms;
        };
        if (!settings.buy_potions && !dry) {
            if (forced) tell("Tự mua bình đang tắt. Đánh dấu \"Tự mua bình\" ở thẻ Dọn rác.");
            shop_due_ms = minutes(1);
            return;
        }
        if (!bag::can_check_shop(store) || !counts.loaded()) {
            shop_due_ms = minutes(5);
            tell("Thiếu hình hoặc bộ chữ số của game để mua bình (xem thẻ Dọn rác).");
            return;
        }
        // Why a timed look is put off, for the log (put_off); a button's says
        // it on the card instead.
        const auto timed_put_off = [&](const std::string& why, const std::string& said) {
            if (!forced && !dry) put_off("shop", why, said);
        };
        if (!input_.window_valid()) return not_now("Chưa thấy cửa sổ game.");
        if (!client_on_screen(input_.target_window())) return not_now(kGameOffScreen, 60'000);
        if (chat_doubt_) return not_now(kTrashWaitsForChat);
        if (coord_.critical_active() && coord_.critical_reason() != "trash cleaning") {
            timed_put_off("hold " + coord_.critical_reason(),
                          "another job holds the game (" + coord_.critical_reason() + ")");
            return not_now("Chờ: bot đang tạm dừng (" + coord_.critical_reason() + "), mua bình sau.",
                           coord_.critical_reason() == kChatHold ? kAfterChatLineMs : 15'000);
        }
        if (keeper_cold_start_) return not_now("Chờ: GoBot đang vào lại game, mua bình sau.");
        {
            std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
            if (keeper_flow_ != nullptr) return not_now("Chờ: GoBot đang qua màn hình khác, mua bình sau.");
        }
        // The macro's letter and Space keys between purchases (2026.11.10):
        // the window shut from just before each double-click until the
        // question is seen gone and the full-bag message has had its time.
        // Whether the HP potion is among them sets the round's HP gates
        // (2026.11.11).
        const PassKeys pass_keys = held || trash_left_held_ ? PassKeys{} : pass_keys_now();
        const bag::HpGates hp_gate = hp_gates_of(pass_keys);
        job_level = hp_gate.stop;
        // HP not read -- not in the world: a map loading, the death panel, the
        // character screen -- is asked of every round, one that rides a pass
        // too (the review's R3): the pass's own gate lets HP unread through.
        // The start gate is not asked of a round that rides a pass: the pass
        // passed its own a moment ago, and the guard watches HP through the
        // round all the same.
        const float hp = hp_percent_now();
        if (hp < 0.0f) return not_now("Chưa đọc được máu của nhân vật, mua bình sau.", 15'000);
        if (!after_pass) {
            if (hp < static_cast<float>(hp_gate.start)) {
                timed_put_off("hp",
                              hp_said(hp, hp_gate.start, hp_gate.drinks ? "the HP potion's mark" : "the start gate"));
                return not_now("Máu đang thấp, mua bình sau.", bag::kHpLookAgainMs);
            }
        }
        // The two potions and the Người Giữ Sân item, as catalog icons -- the
        // live client's bag draws the item as its icon at 1.000, so no picture
        // of the player's is asked for -- and the rest of the catalog to tell
        // them from: nothing here is ever deleted.
        const std::vector<std::string> icons{ CleanSettings::kBuyHpId, CleanSettings::kBuyMpId, settings.buy_opener };
        for (const std::string& id : icons)
            if (catalog.find(id) == nullptr) {
                shop_due_ms = minutes(5);
                tell("Không có hình \"" + id + "\" trong bộ hình món đồ của game, nên không mua bình được.");
                return;
            }
        TrashList list;
        list.add_catalog(catalog, icons, TrashList::Action::Delete, settings.catalog_score);
        list.add_rivals(catalog);
        if (list.items.size() < 3) {
            shop_due_ms = minutes(5);
            tell("Không có hình món đồ để tìm món Người Giữ Sân (" + settings.buy_opener +
                 ") trong bộ hình của game.");
            return;
        }

        // A "Mua ngay" that does not get to run -- the hold not taken, the
        // quiet second spoiled every time, a stop -- leaves the plan as it was:
        // nothing owed by it and no plan time moved, so a press said "not now"
        // is not bought later, just before the next plan. The clock it wrote
        // only errs towards waiting, and the next plan writes it again.
        const bag::ShopOwed owed_before = owed;
        const std::uint64_t plan_before = plan_due_ms;
        // The plan, once the round can run: the gates above leave it as it
        // was. The owner's numbers as they are now bound what is still owed:
        // one lowered, or set to 0, buys no more than it says. A new plan when
        // its time has come, or with "Mua ngay" -- owing the numbers set,
        // afresh, whatever was left -- and its time written down before
        // anything is clicked. Nothing owed before then -- a number set under
        // what was left -- waits for the plan's time and never begins one
        // early. A trial touches none of it.
        if (!dry) {
            owed.hp = std::min(owed.hp, settings.buy_hp_slots);
            owed.mp = std::min(owed.mp, settings.buy_mp_slots);
            if (forced || now_ms() >= plan_due_ms) {
                const int every = settings.buy_every_minutes;
                if (settings.buy_hp_slots + settings.buy_mp_slots <= 0) {
                    owed = bag::ShopOwed{};
                    if (forced) {
                        tell("Cả hai số ô đều là 0: không có gì để mua.");
                        shop_due_ms = due_before;
                        return;
                    }
                    plan_due_ms = minutes(every);
                    shop_due_ms = plan_due_ms;
                    GB_INFO("Trash: shop -- both slot numbers 0, nothing to buy; next plan %s",
                            clock_at(plan_due_ms).c_str());
                    tell("Cả hai số ô đều là 0: không mua gì.");
                    return;
                }
                // Not written down, nothing bought: a GoBot started again would
                // buy the plan early -- as a charm is not used unwritten. A
                // "Mua ngay" in the middle of a plan leaves that plan as it was.
                if (!shop_clock.set_next(kShopClockId, wall_s() + static_cast<std::int64_t>(every) * 60)) {
                    if (!forced) owed = bag::ShopOwed{};
                    shop_due_ms = forced ? due_before : minutes(5);
                    GB_WARN("Trash: shop -- %s could not be written; nothing bought", shop_clock_path().c_str());
                    tell("Không ghi được " + shop_clock_path() + ", nên không mua bình (giờ mua không ghi lại được "
                         "thì mở lại GoBot sẽ mua sớm).");
                    return;
                }
                owed = bag::shop_new_plan(settings.buy_hp_slots, settings.buy_mp_slots);
                plan_due_ms = minutes(every);
                GB_INFO("Trash: shop plan HP %d + MP %d (owed %d+%d, at most %d a hold%s); clock saved, next plan %s",
                        settings.buy_hp_slots, settings.buy_mp_slots, owed.hp, owed.mp,
                        forced ? BagCleaner::kShopMaxBuys : BagCleaner::kShopBuysPerHold, forced ? ", Mua ngay" : "",
                        clock_at(plan_due_ms).c_str());
            }
            if (owed.hp + owed.mp == 0) {
                shop_due_ms = plan_due_ms;
                GB_INFO("Trash: shop -- nothing left of this plan (a number lowered); next plan %s",
                        clock_at(plan_due_ms).c_str());
                return;
            }
        }

        // Its own capture, off the screen only: the game is in front for every
        // click of it anyway, and PrintWindow was being tried grab after grab
        // on the live client just as the quantity box came up -- see
        // ScreenCapture::set_screen_only.
        ScreenCapture shop_capture;
        shop_capture.set_screen_only(true);
        shop_capture.set_label("shop");
        const auto shop_grab = [&](Frame& f) { return shop_capture.grab(input_.target_window(), f); };
        BagCleaner cleaner(input_, store, shop_grab);
        cleaner.set_settings(settings);
        cleaner.set_trash(&list);
        cleaner.set_counts(&counts);
        cleaner.set_debug_dir(exe_relative("trash_debug"));
        cleaner.set_mode(BagCleaner::Mode::ShopBuy);
        cleaner.set_dry_run(dry);
        // This round's part of what is owed, in turns: a timed round
        // kShopBuysPerHold at most -- the rest kShopPartGapMs later, the skills
        // and potions not held off a minute and more with nobody watching --
        // and "Mua ngay" kShopMaxBuys at most, the rest in timed parts. A trial
        // tries one of each, whatever this says.
        const std::vector<bag::ShopPotion> part =
            bag::shop_order(owed.hp, owed.mp, forced ? BagCleaner::kShopMaxBuys : BagCleaner::kShopBuysPerHold);
        const int part_hp = static_cast<int>(std::count(part.begin(), part.end(), bag::ShopPotion::Hp));
        if (!dry) cleaner.set_shop_plan(part_hp, static_cast<int>(part.size()) - part_hp);
        // The hold must stay ours, and HP not fall under the round's level --
        // half the HP potion's mark when the potion drinks between purchases,
        // else the mark, the potion held off as well (2026.11.11): the run
        // stops there and gives the keys back. The wait for a quiet second
        // watches the same. What HP stopped it at goes to the log, and makes
        // the round put off, never trouble (hp_cut).
        const auto still_ours = [this, &hp_cut, stop_level = hp_gate.stop] {
            if (!(coord_.critical_active() && coord_.critical_reason() == "trash cleaning")) return false;
            const float hp = hp_percent_now();
            const int level = stop_level;
            if (hp >= 0.0f && hp < static_cast<float>(level)) {
                hp_cut = HpCut{ hp, level };
                return false;
            }
            return true;
        };
        const auto hp_under = [this, stop_level = hp_gate.stop] {
            const float hp = hp_percent_now();
            return hp >= 0.0f && hp < static_cast<float>(stop_level);
        };
        cleaner.set_guard(still_ours);
        // The game's full-bag message closed whatever HP is, never under
        // another job's hold (2026.11.11).
        cleaner.set_hold_check(
            [this] { return coord_.critical_active() && coord_.critical_reason() == "trash cleaning"; });
        cleaner.on_report([this](const std::string& line) { say_trash(line); });
        if (pass_keys.any()) cleaner.set_key_window(key_window());
        else if (!held && !trash_left_held_) cleaner.set_own_potions(own_potions());
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = &cleaner;
        }
        if (trash_stop_) cleaner.cancel();

        // Whether anybody has used the mouse or keyboard since a moment: asked
        // only under the hold, after the keys are let go, as the quick quest
        // asks it -- GoBot presses nothing then: no key window is armed until
        // the round runs, and GoBot's own input in that second is never taken
        // for the player's (quiet_said).
        const auto last_input = [] {
            LASTINPUTINFO info{};
            info.cbSize = sizeof info;
            return GetLastInputInfo(&info) ? info.dwTime : 0;
        };
        BagCleaner::Result r;
        bool passed = false, touched = false, lost = false, own = false;
        // The run ended with the hold no longer this job's (`cut`), or its
        // guard stopped it for HP under the round's level (`hp_stop`): put
        // off, not trouble -- HP's looked at again kHpStoppedAgainMs on.
        bool cut = false, hp_stop = false;
        const std::uint64_t asked_ms = now_ms();
        std::uint64_t held_ms = 0;
        // The wait a spoiled quiet second brings: a timed look's, further off
        // each time; a button's run does not move it.
        std::uint64_t touched_ms = 0;
        {
            // A round that rides a pass has the game's mouse already, in the
            // pass's hand: not asked again -- the lock is not one a thread may
            // take twice -- nor let go, which a revive or an invite would take.
            // Its hold is the pass's too, which take_hold leaves as it is.
            std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::defer_lock);
            const std::uint64_t give_up = now_ms() + 5'000;
            while (!after_pass && !trash_stop_ && now_ms() < give_up && !mouse.try_lock())
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if ((after_pass || mouse.owns_lock()) && !trash_stop_ && !chat_doubt_ && take_hold("trash cleaning")) {
                trash_pass_ = true;
                held_ms = now_ms();
                // No key window from the last job under this hold: none of the
                // macro's keys from here to the round (2026.11.10).
                coord_.disarm_pass_keys();
                for (int i = 0; i < 30 && coord_.held(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                input_.release_all();
                // Nobody at the mouse or keyboard for a moment: a pointer moved
                // over the bag's tabs spoils which one reads as chosen, and the
                // player keeps the mouse they are using. Not now, and asked
                // again in a while, the hold let go as it was. A button's run
                // looks again a few times first, the hold and HP watched. The
                // pointer, its buttons and the window in front are glanced at
                // either side, for the log to say what spoiled it (read only).
                // A second in which GoBot itself sent anything -- a key let go
                // of, another job's -- is measured again, kShopOwnRelooks times
                // at most: whose the input was cannot be told (2026.11.10).
                // None for a round that rides a pass (2026.11.11, the owner):
                // the pass has none, and has just clicked through the bag under
                // this hold; the hold and HP looked at once instead.
                const InputGlancer glancer = InputGlancer::real();
                InputGlance seen_before;
                const int tries = forced || dry ? kShopQuietTries : after_pass ? 0 : 1;
                int own_seen = 0;
                if (after_pass) lost = !still_ours();
                // The key-ups just sent in Windows' tick before the second
                // starts, should it stamp them a moment after SendInput.
                if (tries > 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
                for (int t = 0; t < tries && !trash_stop_ && !lost; ++t) {
                    seen_before = glancer.glance();
                    // GoBot's count read before the tick at the start and after
                    // it at the end: an event of GoBot's whose tick lands in the
                    // second moves the count between them (sent_count).
                    const std::uint64_t own_from = InputManager::sent_count();
                    const DWORD quiet = last_input();
                    for (int slept = 0; slept < kShopQuietMs && !trash_stop_ && !lost; slept += 50) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        lost = !still_ours();
                    }
                    const DWORD after = last_input();
                    const std::uint64_t own_to = InputManager::sent_count();
                    const QuietSaid said = quiet_said(quiet, after, own_from, own_to);
                    own = !trash_stop_ && !lost && said == QuietSaid::Own;
                    if (own && own_seen++ < kShopOwnRelooks) {
                        --t;
                        continue;
                    }
                    touched = !trash_stop_ && !lost && said == QuietSaid::Touched;
                    if (!touched && !own) break;
                }
                if (own)
                    GB_INFO("Shop: not now -- GoBot itself sent input in each quiet second (held %llu ms)",
                            static_cast<unsigned long long>(now_ms() - held_ms));
                if (touched || own || trash_stop_ || lost) {
                    if (touched) {
                        const std::string seen = quiet_spoiled_text(seen_before, glancer.glance());
                        if (forced || dry) {
                            GB_INFO("Shop: not now -- someone is using the mouse or keyboard (held %llu ms): %s",
                                    static_cast<unsigned long long>(now_ms() - held_ms), seen.c_str());
                        } else {
                            touched_ms = shop_quiet.next_ms(static_cast<std::uint64_t>(settings.buy_every_minutes) *
                                                            60'000ull);
                            GB_INFO("Shop: not now -- someone is using the mouse or keyboard (held %llu ms): %s; "
                                    "the next look in %llu s",
                                    static_cast<unsigned long long>(now_ms() - held_ms), seen.c_str(),
                                    static_cast<unsigned long long>(touched_ms / 1000));
                        }
                    }
                    if (!held) end_hold("trash cleaning");
                    trash_pass_ = false;
                } else {
                    passed = true;
                    put_off_why.clear();
                    pull_said = false;
                    // A quiet second: the next spoiled one waits 30 s again. A
                    // round that rides a pass had none, and leaves the wait as
                    // it was.
                    if (!after_pass) shop_quiet.reset();
                    tell(dry ? "Thử mua bình (không mua)..." : "Mua bình...");
                    if (!input_.window_focused()) {
                        input_.focus_window();
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    }
                    // 30 s and 6 s a purchase of this round, 300 s at most.
                    cleaner.set_deadline_ms(BagCleaner::shop_deadline_ms(dry ? 2 : static_cast<int>(part.size())));
                    // The packets' bag either side of a real round, for the log
                    // only: it gates nothing (packets_bag.cpp).
                    int bag_used = -1;
                    if (!dry) {
                        const std::string bag_now = bag_shop_text(bag_used);
                        if (!bag_now.empty()) GB_INFO("Bag (packets): before the shop %s", bag_now.c_str());
                    }
                    {
                        // No PrintWindow anywhere in GoBot meanwhile, the vitals
                        // reader's included: see ScreenCapture::set_screen_only.
                        const PrintWindowPause no_print;
                        // The key window armed for this round's hold alone, and
                        // shut -- its keys seen up -- and forgotten however the
                        // round ends, before the hold is kept or let go of
                        // below (2026.11.10).
                        const PassKeysScope keys(coord_, pass_keys, "shop");
                        hp_cut = HpCut{};
                        r = cleaner.run();
                    }
                    say_stopped("shop round", hp_cut, r.trouble);
                    // What stopped it, as its guard saw it (2026.11.11): HP
                    // under the round's level -- whatever HP reads by now, a
                    // potion let through may have lifted it since -- and,
                    // looked at now, the hold no longer this job's.
                    hp_stop = !trash_stop_ && hp_cut.level > 0;
                    cut = !trash_stop_ && !(coord_.critical_active() && coord_.critical_reason() == "trash cleaning");
                    if (!dry) {
                        const std::string bag_now = bag_shop_text(bag_used);
                        if (!bag_now.empty()) GB_INFO("Bag (packets): after the shop %s", bag_now.c_str());
                        if (r.shop_full && bag_used >= 0)
                            GB_INFO("Bag (packets): the game said full with %d Tiêu Dùng slots used", bag_used);
                    }
                    held = r.keeps_hold(held);
                    shop_held = held && (shop_held || r.stuck);
                    trash_hold_ = held;
                    if (!held) end_hold("trash cleaning");
                    trash_pass_ = false;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = nullptr;
        }
        if (!passed) {
            // A press that did not get to run leaves the plan as it was -- a
            // DỪNG / F10 while it waited too, which says nothing here. A timed
            // look's plan was due, and stays begun.
            if (forced) {
                owed = owed_before;
                plan_due_ms = plan_before;
            }
            const std::string busy =
                coord_.critical_active() ? coord_.critical_reason() : std::string("chuột đang dùng");
            if (touched) {
                timed_put_off("touched", "someone is using the mouse or keyboard (the quiet second)");
                not_now("Chờ: bạn đang dùng chuột hoặc bàn phím, mua bình sau.", touched_ms);
            } else if (own) {
                timed_put_off("own input", "GoBot's own input in each quiet second");
                not_now("Chờ: GoBot vừa tự bấm phím hay chuột trong lúc chờ yên, mua bình sau.", 15'000);
            } else if (lost && hp_under()) {
                timed_put_off("hp", hp_said(hp_percent_now(), hp_gate.stop, "the level"));
                not_now("Máu đang thấp, mua bình sau.", bag::kHpStoppedAgainMs);
            } else if (!trash_stop_) {
                timed_put_off("hold " + busy, "another job holds the game (" + busy + ")");
                not_now("Chờ: bot đang bận (" + busy + "), mua bình sau.",
                        coord_.critical_reason() == kChatHold ? kAfterChatLineMs : 15'000);
            }
            return;
        }

        GB_INFO("Trash: shop job: asked at %llu ms, hold taken after %llu ms, took %llu ms, %d bought (HP %d, MP %d), "
                "%d unsure, full %s; %s, the last pass ended %s",
                static_cast<unsigned long long>(asked_ms), static_cast<unsigned long long>(held_ms - asked_ms),
                static_cast<unsigned long long>(now_ms() - held_ms), r.shop_bought, r.shop_bought_hp,
                r.shop_bought_mp, r.shop_unsure, r.shop_full ? "yes" : "no",
                after_pass ? "right after the pass" : forced ? "Mua ngay" : dry ? "Thử mua" : "on its own",
                since_pass().c_str());
        if (!r.board_up) board_said = false;
        // What is owed after the round (bag::shop_owed_after), and the next
        // look. A trial moves none of it: not what is owed, nor the troubles,
        // nor either clock. Nothing owed, the next look is the next plan's
        // time, however the round ended -- never sooner, or a plan bought in
        // full would be bought again. Otherwise a round cut short by DỪNG / F10
        // keeps its clock as it was, which the next worker resumes from its
        // start's 45 s, owing the rest. The board, the hold lost, HP its guard
        // stopped it for, or a key of the macro's not seen up in time put the
        // rest off -- the board kBoardWaitMs, HP kHpStoppedAgainMs, the others
        // a part's gap, kShopPartGapMs -- and are not trouble (bag::shop_end);
        // a full bag, or a message in the quantity box's place, ends the plan;
        // a trouble waits ten minutes -- not every few seconds, which would
        // hold the bot off each time -- and the second in a row drops what is
        // left of the plan.
        bool dropped = false;
        if (dry) {
            shop_due_ms = due_before;
        } else {
            bag::ShopRoundSeen seen;
            seen.full = r.shop_full;
            seen.message = r.shop_message;
            seen.board_up = r.board_up;
            seen.stopped = trash_stop_;
            seen.hp_cut = hp_stop;
            seen.hold_lost = cut;
            seen.keys_late = r.keys_late;
            seen.trouble = !r.trouble.empty() || !r.ran;
            const bag::ShopEnd end = bag::shop_end(seen);
            owed = bag::shop_owed_after(owed, r.shop_bought_hp, r.shop_bought_mp, r.shop_unsure_hp, r.shop_unsure_mp,
                                        end);
            dropped = owed.dropped;
            const bool next_part = owed.hp + owed.mp > 0;
            if (!next_part) shop_due_ms = plan_due_ms;
            else if (trash_stop_) shop_due_ms = due_before;
            else if (r.board_up) shop_due_ms = now_ms() + BagCleaner::kBoardWaitMs;
            else if (hp_stop) shop_due_ms = now_ms() + bag::kHpStoppedAgainMs;
            else if (end == bag::ShopEnd::Paused) shop_due_ms = now_ms() + BagCleaner::kShopPartGapMs;
            else if (end == bag::ShopEnd::Trouble) shop_due_ms = minutes(10);
            else shop_due_ms = now_ms() + BagCleaner::kShopPartGapMs;
            const std::string when =
                next_part ? std::to_string((shop_due_ms > now_ms() ? shop_due_ms - now_ms() : 0) / 1000) + " s on"
                          : clock_at(plan_due_ms);
            GB_INFO("Trash: shop done -- bought %d+%d, unsure %d, full %s%s; owed %d+%d; %s %s",
                    r.shop_bought_hp, r.shop_bought_mp, r.shop_unsure, r.shop_full ? "yes" : "no",
                    r.shop_message ? ", a message instead of the box" : r.board_up ? ", the check-in board" : "",
                    owed.hp, owed.mp, next_part ? "next part" : "next plan", when.c_str());
            if (dropped)
                GB_INFO("Trash: shop -- %d troubles in a row: owed dropped until the next plan %s",
                        bag::kShopTroublesToDrop, clock_at(plan_due_ms).c_str());
        }
        if (r.board_up) {
            timed_put_off("board", "the check-in board is up");
            board_put_off("shop", forced || dry);
        }
        std::string line;
        if (r.shop_bought > 0)
            line = "Đã mua " + std::to_string(r.shop_bought) + " lần " + std::to_string(CleanSettings::kBuyQuantity) +
                   ": " + std::to_string(r.shop_bought_hp) + " ô Thịt bò khô, " + std::to_string(r.shop_bought_mp) +
                   " ô Rượu lá tre.";
        const auto add = [&line](const std::string& more) { line += (line.empty() ? "" : " ") + more; };
        // A full bag: the end of this plan, said with how far it got. Whatever
        // would not close is said by the trouble below.
        if (r.shop_full)
            add(dry ? std::string("Thử mua: game báo túi đồ [Tiêu thụ] đã đầy.")
                    : "Game báo túi đồ [Tiêu thụ] đã đầy" +
                          (r.shop_bought > 0 ? " sau " + std::to_string(r.shop_bought) + " lần mua" : std::string()) +
                          ": " + (r.trouble.empty() ? "đã đóng thông báo và cửa hàng, " : "") +
                          "dừng lượt này. Tới giờ GoBot mua lại.");
        else if (r.shop_message)
            add(std::string("Game hiện một thông báo khác thay cho hộp số lượng (có thể là túi đầy): ") +
                (r.trouble.empty() ? "đã bấm Xác Nhận của nó và dừng lượt này." : "dừng lượt này.") +
                " Ảnh ở trash_debug (shop_message).");
        if (!r.trouble.empty()) line += (line.empty() ? "" : " ") + std::string("Dừng: ") + r.trouble + ".";
        // What is left of the plan, and when it is bought: by this worker
        // later, or -- one here for the button alone -- once the bot runs.
        const int left = owed.hp + owed.mp;
        if (dropped) {
            add("Hai lần mua liền bị lỗi: GoBot bỏ phần còn lại của lượt này, tới giờ sau mua lại. Ảnh ở trash_debug.");
        } else if (!dry && left > 0 && (!r.board_up || trash_oneshot_) && !trash_stop_) {
            const std::uint64_t in_ms = shop_due_ms > now_ms() ? shop_due_ms - now_ms() : 0;
            add("Còn " + std::to_string(left) + " lần" +
                (trash_oneshot_ ? std::string(" của lượt này: GoBot mua tiếp khi bot chạy (BẮT ĐẦU).")
                                : ", mua tiếp sau " + std::to_string(std::max<std::uint64_t>(1, (in_ms + 59'999) / 60'000)) +
                                      " phút."));
        }
        if (!line.empty()) tell(line);
        if (held) {
            held_looked_ms = now_ms();
            tell("Còn một hộp thoại hoặc cửa hàng đang mở trong game. Đóng nó (Hủy Bỏ, rồi ESC); đánh quái, bình "
                 "và buff tạm dừng đến khi không còn hộp thoại nào.");
        }
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
        // The potion shop, asked for with a button (4: buy now, 5: a trial) or
        // due by its time: a job of its own, like the charms'.
        // Switched off and on again: nothing owed, the next look as a worker's
        // start has it -- what is left of the plan time saved, 45 s at least --
        // and a spoiled quiet second's wait from its first step. Not 45 s
        // whatever was saved: unticked and ticked again, a plan just bought is
        // not bought again ("Mua ngay" is for that).
        if (settings.buy_potions != shop_was_on) {
            shop_was_on = settings.buy_potions;
            if (shop_was_on) {
                owed = bag::ShopOwed{};
                plan_due_ms = 0;
                shop_due_ms = shop_first_due();
                shop_quiet.reset();
            }
        }
        // The same for a tick on the window, which the file may not have shown
        // off in between -- and the charms' clock, after a tab not read or a
        // run that stopped on something, from a start's wait.
        if (shop_ticked_.load() != shop_ticks) {
            shop_ticks = shop_ticked_.load();
            owed = bag::ShopOwed{};
            plan_due_ms = 0;
            shop_due_ms = shop_first_due();
            shop_quiet.reset();
        }
        if (charm_ticked_.load() != charm_ticks) {
            charm_ticks = charm_ticked_.load();
            charm_check_ms = now_ms() + kCharmFirstMs;
        }
        // What is due by its own clock, and in the player's order when several
        // are: Tự chat's line, then the pass, then the charms, then the shop --
        // but a shop come due with Tự dọn rác on pulls the pass to now and
        // rides it, right after it and before the charms (2026.11.11, the
        // owner). A button (requests 1 to 5) is now, and waits for nothing.
        const bool timed = request == 0 && !trash_oneshot_;
        const bool shop_due = timed && !held && settings.buy_potions && now_ms() >= shop_due_ms;
        // The ride is the look due now's (bag::ride_turn): none with no look
        // due -- Tự mua bình off, a button, a worker here for one, the bin
        // held -- or Tự dọn rác off. Decided once for each look, as it comes
        // due: a pass put off by its gates is not pulled again until the
        // shop's own wait is over. The pass pulled to now -- but none where
        // the shop's own cheap gates would put the look off, which the pass
        // does not all ask (the review's R1): the chat line in doubt, no game
        // window or the game off the screen, HP not read -- the login or
        // character screen, a map loading, the death panel -- a keeper's
        // crossing or cold start; nor for an interval after one pulled found
        // nothing it could do (R2). The look goes on its own then, and waits
        // by the shop's rules; the first look after pulls one.
        {
            const bag::ShopRide was = ride;
            if (!settings.enabled) no_pull_before_ms = 0;
            const auto no_pull = [&] {
                if (chat_doubt_ || keeper_cold_start_ || now_ms() < no_pull_before_ms || !input_.window_valid() ||
                    !client_on_screen(input_.target_window()) || hp_percent_now() < 0.0f)
                    return true;
                std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
                return keeper_flow_ != nullptr;
            };
            // Asked only as a look is decided, not every turn.
            const bool cannot_pull = shop_due && settings.enabled && was == bag::ShopRide::None && no_pull();
            ride = bag::ride_turn(was, shop_due, settings.enabled, cannot_pull, now_ms(), last_pass_end_ms);
            if (ride == bag::ShopRide::Pending && was != bag::ShopRide::Pending) {
                next = std::min(next, now_ms());
                // Once until a job runs, not again for each look the same
                // reason puts off.
                if (!pull_said) {
                    pull_said = true;
                    GB_INFO("Trash: shop due -- a pass first, then the round under the same hold (the last pass "
                            "ended %s)", since_pass().c_str());
                }
            }
        }
        const bool pass_due = timed && settings.enabled && now_ms() >= next;
        const bool charm_due = timed && !held && settings.use_charms && now_ms() >= charm_check_ms;
        show_shop();
        if ((pass_due || charm_due || shop_due) && chat_first()) continue;
        if (request == 4 || request == 5 || (shop_due && ride != bag::ShopRide::Pending && !pass_due && !charm_due)) {
            if (request == 0) shop_due_ms = now_ms() + 5'000;
            ride = bag::ShopRide::None;
            run_shop(request == 4, request == 5, false);
            if (trash_oneshot_ && !held) break;
            continue;
        }
        // The charms, asked for with a button or due by their clock: a job of
        // their own, and the timed pass is looked at again after it.
        if (request == 3 || (charm_due && !pass_due)) {
            charm_check_ms = now_ms() + 5'000;
            run_charms(request == 3);
            if (trash_oneshot_ && !held) break;
            continue;
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
        // The pass a due shop pulled (2026.11.11): from here on that shop goes
        // on its own -- its own gates, its quiet second -- by every way out
        // below but the end of a timed pass that ran clean, which its round
        // rides, as it does a shop come due while the pass ran.
        const bool pulled = ride == bag::ShopRide::Pending && request == 0;
        if (pulled) ride = bag::ShopRide::Alone;

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
            if (request == 0)
                put_off("pass", "hold " + coord_.critical_reason(),
                        "another job holds the game (" + coord_.critical_reason() + ")");
            if (trash_oneshot_ && !held) break;
            // Right after a Tự chat line: the bag is next in the player's order.
            next = now_ms() + (coord_.critical_reason() == kChatHold ? kAfterChatLineMs : 30'000);
            continue;
        }
        next = now_ms() + static_cast<std::uint64_t>(settings.interval_minutes) * 60'000ull;

        const bool look_only = request == 2;
        // An item may be left in the bin, which a pass is what gives back.
        const bool left_held = held || trash_left_held_;
        // The macro's letter and Space keys between the pass's own clicks
        // (2026.11.10) -- not in one begun to give back what may be in the bin;
        // whether the HP potion is among them sets the pass's HP gates
        // (2026.11.11, bag::hp_gates).
        const PassKeys pass_keys = left_held ? PassKeys{} : pass_keys_now();
        const bag::HpGates hp_gate = hp_gates_of(pass_keys);
        job_level = hp_gate.stop;

        // Not with HP under the start gate: the HP potion's mark when it is
        // drunk between items, else over it by a margin, the potion held off
        // for the length of the pass. A timed pass waits a few seconds, said
        // on the card once until it goes; one asked for with a button goes,
        // and stops between items should HP fall under its level (the guard).
        // Asked before the list is built (the review's PERF-PASS-HP-5S):
        // building it reads every picture and weighs it against the catalog,
        // too much to do every few seconds only to wait again.
        if (request == 0 && !held) {
            const float hp = hp_percent_now();
            if (hp >= 0.0f && hp < static_cast<float>(hp_gate.start)) {
                if (put_off_why != "hp") tell("Máu đang thấp, dọn rác sau.");
                put_off("pass", "hp",
                        hp_said(hp, hp_gate.start, hp_gate.drinks ? "the HP potion's mark" : "the start gate"));
                next = now_ms() + bag::kHpLookAgainMs;
                continue;
            }
        }

        // A timed pass that finds nothing it could do -- the list empty, no
        // tab ticked -- is looked at again by its own clock, an interval on:
        // no shop's look pulls one before then (no_pull_before_ms, the
        // review's R2), each to build the list and say the same again.
        TrashList list;
        list.build(exe_relative("configs/trash"), settings, catalog);
        if (list.items.empty()) {
            tell("Danh sách đồ bỏ đi đang trống, nên không có gì để dọn.");
            if (request == 0) no_pull_before_ms = next;
            if (trash_oneshot_ && !held) break;
            continue;
        }
        // No tab to look through -- none ticked, or every item one of Tiêu
        // Dùng's with that tab not ticked (bag::pass_tabs): no hold, no bag
        // key, and said as the card says it (2026.11.10). Unless an item may
        // be left in the bin (left_held).
        const bag::PassTabs pass_tabs = bag::pass_tabs(settings, list);
        if (pass_tabs.count == 0 && !left_held) {
            tell(std::string(pass_tabs.none_said()) + ".");
            if (request == 0) no_pull_before_ms = next;
            if (trash_oneshot_) break;
            continue;
        }
        if (!input_.window_valid()) {
            tell("Chưa thấy cửa sổ game.");
            if (trash_oneshot_ && !held) break;
            continue;
        }
        if (!client_on_screen(input_.target_window())) {
            tell(kGameOffScreen);
            if (trash_oneshot_ && !held) break;
            next = now_ms() + 60'000;
            continue;
        }

        // Its own capture, off the screen only, as the potion shop's: every
        // click of a pass needs the game in front and on top where it lands
        // anyway, and PrintWindow is not asked anywhere in GoBot while it runs
        // (ScreenCapture::set_screen_only, PrintWindowPause below).
        ScreenCapture pass_capture;
        pass_capture.set_screen_only(true);
        pass_capture.set_label("bag");
        const auto pass_grab = [&](Frame& f) { return pass_capture.grab(input_.target_window(), f); };
        BagCleaner cleaner(input_, store, pass_grab);
        cleaner.set_settings(settings);
        cleaner.set_trash(&list);
        cleaner.set_counts(&counts);
        cleaner.set_debug_dir(exe_relative("trash_debug"));
        cleaner.set_dry_run(look_only);
        cleaner.set_left_held(left_held);
        if (pass_keys.any()) cleaner.set_key_window(key_window());
        else if (!left_held && !look_only) cleaner.set_own_potions(own_potions());
        cleaner.on_report([this](const std::string& line) { say_trash(line); });
        // The hold must stay this pass's, and HP not fall under its level --
        // half the HP potion's mark when the potion drinks between items, else
        // the mark (2026.11.11): the pass stops between items and gives the
        // keys back. What HP stopped it at goes to the log (hp_cut), and the
        // pass is tried again kHpStoppedAgainMs on.
        cleaner.set_guard([this, &hp_cut, stop_level = hp_gate.stop] {
            if (!(coord_.critical_active() && coord_.critical_reason() == "trash cleaning")) return false;
            const float hp = hp_percent_now();
            const int level = stop_level;
            if (hp >= 0.0f && hp < static_cast<float>(level)) {
                hp_cut = HpCut{ hp, level };
                return false;
            }
            return true;
        });
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = &cleaner;
        }
        // A stop that came while all of the above was being done found no
        // cleaner to cancel; it is passed on now instead.
        if (trash_stop_) cleaner.cancel();

        BagCleaner::Result r;
        bool passed = false;
        // The shop's round rides this pass (2026.11.11): the hold and the
        // game's mouse kept for it, from the pass's end to the round's.
        bool rides = false;
        // Not while the revive sequence or the party worker is clicking, nor
        // they while this is. The hold is taken only now, with the mouse: a
        // revive that began while this waited for it keeps its own, and the
        // pass waits instead. The chat line is asked about again with the
        // mouse in hand: a line left in doubt since the look above was under
        // Tự chat's hold until then. Waited for a little at a time, as the
        // revive waits for a pass, and given up on a stop: an invite holding
        // the mouse when F12 came handed it to this pass the moment it ended.
        // Let go of as the pass ends -- or, the shop riding it, as its round
        // does, below.
        std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::defer_lock);
        {
            while (!trash_stop_ && !mouse.try_lock())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (mouse.owns_lock() && !trash_stop_ && !chat_doubt_ && take_hold("trash cleaning")) {
                passed = true;
                trash_pass_ = true;
                put_off_why.clear();
                pull_said = false;
                tell(look_only ? "Thử tìm trong túi đồ..." : "Bắt đầu dọn rác...");
                const std::uint64_t pass_from = now_ms();
                // A key half pressed by a skill let finish first, as the shop's
                // and the charms' do; then the game in front, looked at every
                // 20 ms rather than waited a fixed 350 for.
                for (int i = 0; i < 30 && coord_.held(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                input_.release_all();
                if (!input_.window_focused()) {
                    input_.focus_window();
                    for (int i = 0; i < 18 && !input_.window_focused(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    std::this_thread::sleep_for(std::chrono::milliseconds(60));
                }
                {
                    const PrintWindowPause no_print;
                    // Armed for this pass's hold alone, and shut -- its keys
                    // seen up -- and forgotten however the pass ends, before
                    // the hold is kept or let go of below (2026.11.10).
                    const PassKeysScope keys(coord_, pass_keys, "pass");
                    hp_cut = HpCut{};
                    r = cleaner.run();
                }
                if (!look_only) last_pass_end_ms = now_ms();
                GB_INFO("Trash: pass took %llu ms: %d deleted, %d failed%s",
                        static_cast<unsigned long long>(now_ms() - pass_from), r.deleted, r.failed,
                        r.trouble.empty() ? "" : " (stopped)");
                say_stopped("pass", hp_cut, r.trouble);
                // Stopped by its guard for HP (2026.11.11): tried again in a
                // few seconds, not the whole interval on -- unless the hold is
                // kept, below, which looks again in a minute.
                if (hp_cut.level > 0 && !trash_stop_) next = now_ms() + bag::kHpStoppedAgainMs;
                // Kept while the bin may still hold the item, and by a pass
                // that never got to a clean screen -- a look-only one, or one
                // that met a dialog it would not answer -- since it saw
                // nothing of an earlier item coming back. Otherwise ended, if
                // it is still this pass's: one taken over during the pass is
                // somebody else's to end.
                held = r.keeps_hold(held);
                trash_hold_ = held;
                // The shop due -- the look that pulled this pass, or one come
                // due while it ran -- rides a timed pass that ran clean: the
                // hold and the mouse kept for its round (bag::ride_refused).
                // A Dọn ngay never buys.
                bag::RidePass ridden;
                ridden.ran = true;
                ridden.timed = request == 0 && !trash_oneshot_;
                ridden.stopped = trash_stop_;
                ridden.look_only = look_only;
                ridden.left_held = left_held;
                ridden.keeps_hold = held;
                ridden.bag_ran = r.ran;
                ridden.trouble = !r.trouble.empty();
                ridden.board_up = r.board_up;
                ridden.hold_ours = coord_.critical_active() && coord_.critical_reason() == "trash cleaning";
                ridden.shop_due = settings.buy_potions && now_ms() >= shop_due_ms;
                const char* no_ride = bag::ride_refused(ridden);
                rides = no_ride == nullptr;
                if (pulled && !rides)
                    GB_INFO("Trash: shop -- no ride on the pass it pulled (%s): it goes on its own", no_ride);
                if (!held && !rides) end_hold("trash cleaning");
                if (!rides) trash_pass_ = false;
            }
            if (!rides && mouse.owns_lock()) mouse.unlock();
        }
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = nullptr;
        }
        // Stopped while it waited for the mouse: nothing was touched, and there
        // is nothing to say or to try again.
        if (!passed && trash_stop_) break;
        if (!passed) {
            const bool chat_line = !chat_doubt_ && coord_.critical_reason() == kChatHold;
            if (chat_doubt_) tell(kTrashWaitsForChat);
            else tell("Chờ: bot đang tạm dừng (" + coord_.critical_reason() + "), dọn rác sau.");
            if (!chat_doubt_ && request == 0)
                put_off("pass", "hold " + coord_.critical_reason(),
                        "another job holds the game (" + coord_.critical_reason() + ")");
            if (trash_oneshot_ && !held) break;
            next = now_ms() + (chat_line ? kAfterChatLineMs : 30'000);
            continue;
        }
        // The check-in board up on the pass's first look: nothing pressed, and
        // looked at again in a minute rather than the interval.
        if (r.board_up && !r.ran) {
            next = now_ms() + BagCleaner::kBoardWaitMs;
            if (request == 0) put_off("pass", "board", "the check-in board is up");
            board_put_off(look_only ? "the look" : "the pass", request != 0);
            if (trash_oneshot_ && !held) break;
            continue;
        }
        board_said = false;

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
        // ---- the shop's round right after it, under the same hold ----
        // (2026.11.11) Nothing let go in between: the hold and the game's
        // mouse the pass had, carried into run_shop (`after_pass`) -- no
        // quiet second, no HP start gate -- and let go after the round:
        // its own end lets the hold go, unless it keeps it for something
        // left up, and an early way out of it leaves that to here.
        if (rides) {
            GB_INFO("Trash: shop rides the pass -- the same hold, no quiet second (%s)",
                    pulled ? "the look that pulled it" : "come due while it ran");
            ride = bag::ShopRide::None;
            run_shop(false, false, true);
            if (!held) end_hold("trash cleaning");
            trash_pass_ = false;
            if (mouse.owns_lock()) mouse.unlock();
        }
        if (trash_oneshot_ && !held) break;
    }
    // The charms' and the shop's clocks for the next worker: a MACRO OFF and
    // ON keeps them (see where they were read) -- and the shop's plan, what is
    // owed of it said on the card while no worker buys it.
    {
        std::lock_guard<std::mutex> lock(trash_clocks_mutex_);
        trash_clocks_.charm_check_ms = charm_check_ms;
        trash_clocks_.shop_due_ms = shop_due_ms;
        trash_clocks_.shop_quiet_level = shop_quiet.level();
        trash_clocks_.charm_ticks_seen = charm_ticks;
        trash_clocks_.shop_ticks_seen = shop_ticks;
        trash_clocks_.owed_hp = owed.hp;
        trash_clocks_.owed_mp = owed.mp;
        trash_clocks_.plan_due_ms = plan_due_ms;
        trash_clocks_.shop_troubles = owed.troubles;
    }
    shop_look_at_s_ = 0;
    shop_plan_at_s_ = 0;
    shop_owed_ = owed.hp + owed.mp;
    // A button pressed as the worker was stopping asked for nothing the next
    // worker should do: a "Dùng bùa ngay" left over would use charms the
    // clock has not let go.
    trash_request_ = 0;
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
const char* const kChatLowHp = "Máu thấp — chat sau.";

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
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        chat_log_.push_back(line);
        if (chat_log_.size() > 200) chat_log_.erase(chat_log_.begin());
    }
    request_redraw();       // shown at once, as say_party's
}

void GuiApp::start_chat() {
    vitals_for_job();       // HP and MP at the run's pace before any key
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
    join_pumping(chat_thread_);
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
    vitals_for_job();       // "Gửi thử" watches HP too: at the run's pace before any key
    {
        std::lock_guard<std::mutex> lock(chat_life_mutex_);
        if (chat_running_ && !chat_quit_) {
            chat_request_ = send_one ? 2 : 1;
            return;
        }
    }
    join_pumping(chat_thread_);
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
        // A line under way stops at its next key and is left in the chat
        // line as it is (ChatSender::cancel): nothing more is pressed into
        // it, so the game is not brought forward for it either.
        if (chat_sender_) chat_sender_->cancel();
    }
    chat_next_ms_ = 0;
    chat_waits_trash_ms_ = 0;
    if (!wait) return;
    join_pumping(chat_thread_);
    chat_running_ = false;
    // Asked for while it ran and never answered: not for the next run.
    chat_request_ = 0;
}

void GuiApp::remember_said(const std::string& typed) {
    const std::uint64_t now = now_ms();
    {
        std::lock_guard<std::mutex> lock(chat_said_mutex_);
        chat_said_.emplace_back(typed, now);
        while (!chat_said_.empty() &&
               (chat_said_.size() > SaidLately::kKeep || now - chat_said_.front().second > SaidLately::kKeepMs))
            chat_said_.erase(chat_said_.begin());
    }
    // And in the file every party watcher reads, with the same time: a GoBot
    // started again while this line is on the chat, or a gobot_cli
    // --autoparty beside this one, knows it for the player's (R8bR-1). An
    // append, quick, with the sending Enter waiting on it.
    if (!SaidLately(SaidLately::default_path()).add(typed, now, wall_ms()))
        GB_WARN("Chat: could not keep the line in %s -- a party watcher in another process will not know it",
                SaidLately::default_path().c_str());
}

std::vector<std::pair<std::string, std::uint64_t>> GuiApp::said_lately() {
    const std::uint64_t now = now_ms();
    // The file's -- this run's lines, and those of any other process: a run
    // of GoBot before this one, gobot_cli --chat -- and this run's from
    // memory, for a file that could not be written. A line in both is there
    // twice, with the same time, which changes nothing for without_own.
    std::vector<std::pair<std::string, std::uint64_t>> out =
        SaidLately(SaidLately::default_path()).read(now, wall_ms());
    std::lock_guard<std::mutex> lock(chat_said_mutex_);
    for (const auto& said : chat_said_)
        if (now >= said.second && now - said.second <= SaidLately::kKeepMs &&
            std::find(out.begin(), out.end(), said) == out.end())
            out.push_back(said);
    return out;
}

std::uint64_t GuiApp::last_said_ms() {
    // Another process's lines too: two lines are never closer together than
    // the shortest interval, whoever typed the first -- this GoBot before it
    // was started again, or gobot_cli --chat.
    std::uint64_t last = 0;
    for (const auto& said : said_lately()) last = std::max(last, said.second);
    return last;
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
    capture.set_label("chat");
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
    // open -- nothing would look until BẮT ĐẦU, and whatever waits on the
    // doubt would wait all session for a player who shut the line by hand and
    // went on: a trash pass, a quest round, the popup closer.
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
        GB_INFO("Chat: the game shows the chat line shut; the executor's pause for it ends");
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
    name_this_thread("chat");
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
        chat_waits_trash_ms_ = 0;
        chat_running_ = false;
        return false;
    };
    const auto finish = [&](const std::string& why) {
        tell(why);
        std::lock_guard<std::mutex> lock(chat_life_mutex_);
        chat_next_ms_ = 0;
        chat_waits_trash_ms_ = 0;
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

    // Off the screen only, as the bag jobs: a line is typed only with the game
    // in front (chat_gate), and PrintWindow -- asked now and then, and by the
    // other workers -- is not asked anywhere in GoBot while one is typed
    // (PrintWindowPause below): the shop's quantity box took no digits while
    // it was. A look while the game is behind something reads the bar as not
    // seen, which puts a line off and keeps a doubt, never the other way.
    ScreenCapture capture;
    capture.set_screen_only(true);
    capture.set_label("chat");
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

    // HP under the potion's mark. A line under way -- its hold taken, the
    // potions held off -- ends at once on it, wherever it is: in the pause
    // before it, waiting for a key to finish, between its keys, in any of its
    // waits (R8R-2).
    const auto hp_low = [this] {
        const float hp = hp_percent_now();
        return hp >= 0.0f && hp < static_cast<float>(chat_hp_floor_.load());
    };

    // Under the hold: a key going down as the hold began finishes first --
    // nothing else gets the coordinator's token while the hold lasts -- and
    // nothing is left held down. For a line, not while HP is under the
    // potion's mark: the line waits, not the potion.
    const auto settle = [&](bool for_doubt) {
        const auto stopped = [&] { return chat_quit_ || (!for_doubt && (chat_stop_ || hp_low())); };
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
            // DỪNG, F12, a close: not one key more, as for a line under way.
            // Unticking leaves it at work, waiting for nobody at the keys.
            closer->set_stop([this] { return chat_quit_.load(); });
        }
        doubt.note(r, r.typed + settings.clear_backspaces + 2, now_ms());
    };

    // Whether the last attempt was put off only for a job of the trash worker:
    // asked again in a second, and that worker starts nothing new meanwhile.
    bool for_trash = false;
    // One line: what can be told without a look, then the mouse and the hold,
    // then a look, then the line. Nothing has been pressed when the result
    // says not_now. A line that may have been left open keeps the hold.
    const auto attempt = [&](const ChatText& line, bool asked) {
        for_trash = false;
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
            in.hp_pct = hp_percent_now();
            // Started only with some way to go before the potion's mark, where
            // the guard below ends a line: HP hovering at the mark in a fight
            // would otherwise start a line, end it, and start it again,
            // holding the potion off each time.
            const int floor = chat_hp_floor_.load();
            in.hp_floor = std::max(floor, std::min(kChatHpTop, floor + kChatHpMargin));
        };
        fill();
        ChatGate gate = chat_gate(in);
        if (!gate.send) {
            // A trash job under way, nothing of it stuck: asked again every
            // second, and the trash worker starts nothing new until this line
            // has gone.
            for_trash = in.window && !in.keeper && trash_pass_ && !trash_hold_ && !trash_left_held_;
            return wait(for_trash ? kChatAfterTrashMs : gate.wait_ms, gate.why);
        }

        // Not while the trash cleaner, the revive sequence or the party worker
        // is clicking, nor they while this types: a click can shut the line,
        // and a trash pass has the mouse for the whole of it.
        std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::try_to_lock);
        if (!mouse.owns_lock() || trash_pass_) {
            for_trash = trash_running_ && !trash_hold_ && !trash_left_held_;
            return wait(for_trash ? kChatAfterTrashMs : 5000,
                        trash_pass_ ? "Đang dọn rác — chat sau." : "Bot đang bấm chuột trong game — chat sau.");
        }
        if (!take_hold(kChatHold))
            return wait(10000, "Bot đang tạm dừng (" + coord_.critical_reason() + ") — chat sau.");
        // How long everything else waits for this line, which the line's
        // report counts from: the potions are held off from here.
        const std::uint64_t held_at = now_ms();
        bool keep_hold = false;
        OnExit hold{ [&] { if (!keep_hold) end_hold(kChatHold); } };
        if (!settle(false)) {
            if (chat_stop_ || chat_quit_) return wait(5000, kStopped);
            if (hp_low()) return wait(2000, kChatLowHp);
            return wait(5000, "Bot đang bấm phím — chat sau.");
        }
        // GoBot presses nothing from here until the line, so any input
        // Windows counts from now on is a person's -- maybe typing in the
        // chat. Taken at once, so that the whole of the pause below counts,
        // and asked again just before the first Enter. The pause is
        // chat.ini's quiet_before_ms, 150 as shipped and up to 5 s, with the
        // potions held off: HP under the potion's mark, or the hold taken
        // over, ends it at once, and the hold with it (OnExit above).
        const DWORD quiet = last_input();
        const std::string quiet_why = chat_quiet_pause(
            settings.quiet_before_ms,
            [&]() -> std::string {
                if (chat_stop_ || chat_quit_) return kStopped;
                if (hp_low()) return kChatLowHp;
                const std::string reason = coord_.critical_reason();
                if (reason != kChatHold) return "Bot đang tạm dừng (" + reason + ") — chat sau.";
                return {};
            },
            [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
        if (quiet_why == kStopped) return wait(0, kStopped);
        if (!quiet_why.empty()) return wait(quiet_why == kChatLowHp ? 2000 : 10000, quiet_why);

        Frame frame;
        if (!grab(frame) || !frame.valid()) return wait(5000, "Chưa chụp được màn hình game.");
        fill();
        in.looked = true;
        in.hud = SessionManager::hud_present(frame);
        // No dialog look here: the line's own first look, a moment later on a
        // fresher frame, refuses on a dialog with the same wait (send_line).
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
        // The line is remembered for the party worker (PartyChat::without_own)
        // just before its sending Enter goes down, not by the result: a game
        // that acts on the key-down shows it while the key is held, one that
        // takes that Enter a second or two late sends the line after the looks
        // have given up on it (r.sent false), and the worker can read a line
        // off the chat before send() returns. One remembered per Enter, sent
        // or not; Gửi thử and the timed lines alike -- in memory, and in the
        // file a party watcher of another process reads (remember_said). A
        // line the game dropped has keyword-only lines passed over a while
        // longer: until a later keyword line of GoBot's shows on the chat, 30
        // minutes at most, when nothing was learned from it (DOC-TRUOC.txt).
        sender.on_send_pressed([this, &line] { remember_said(line.typed); });
        sender.set_ready_check([&]() -> std::string {
            return touched_since(quiet) ? kUserActive : std::string();
        });
        // Between keys and through every wait of the line: HP falling under
        // the potion's mark ends the line, so the potion is not held off for
        // the rest of it; so does the hold being anything but this line's
        // own, whoever took it over.
        sender.set_guard([this, hp_low]() -> std::string {
            if (hp_low()) return "Máu thấp — dừng câu đang gõ.";
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

        ChatSender::Result r;
        {
            const PrintWindowPause no_print;
            r = sender.send(line);
        }
        // What it pressed and saw, whenever it pressed anything: each Enter's
        // hold and the bar after it, the ink as it was typed -- for telling a
        // key the game dropped from one it took late. With a line the game
        // showed nothing of, what on this machine can swallow typed letters.
        if (!r.trace.enters.empty()) {
            if (r.trace.outcome == "no_text") r.trace.machine = chat_machine_note(input_.target_window());
            GB_INFO("%s", chat_trace_line(r.trace).c_str());
        }

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
        // What on this machine can swallow typed letters, once a run.
        GB_INFO("Chat: %s", chat_machine_note(input_.target_window()).c_str());
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
                chat_waits_trash_ms_ = 0;
                doubt.step(*closer, now_ms());
                if (doubt.active()) continue;   // nothing else while the line may be open
            }
            if (chat_stop_ || stopped_self || chat_oneshot_) break;
            if (!settings.enabled || settings.lines.empty()) {
                chat_next_ms_ = 0;
                chat_waits_trash_ms_ = 0;
                if (settings.enabled) tell_now_and_then("Danh sách câu đang trống.");
                continue;
            }
            chat_next_ms_ = schedule.due_at();
            const std::uint64_t now = now_ms();
            if (!schedule.due(now)) continue;
            chat_waits_trash_ms_ = 0;   // due: chat_next_ms_ speaks for the line now

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
            if (r.not_now && for_trash) chat_waits_trash_ms_ = end;
            note_doubt(r);
            if (r.not_now) {
                schedule.postpone(end, r.wait_ms);
                tell_now_and_then(r.trouble);
            } else if (r.sent) {
                schedule.sent(end);
                ++chat_sent_;
                put_off_in_row = 0;
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
                // Unticked part way: the worker stays, and closes the line
                // itself once nobody is at the mouse or keyboard -- not what
                // kLeftAsIs says, which is DỪNG's and F12's.
                if (r.left_open && chat_stop_ && !chat_quit_)
                    tell_now_and_then("Câu " + std::to_string(index + 1) +
                                      ": đã dừng giữa chừng, ô chat có thể còn mở. Đóng ô chat trong game (xoá "
                                      "chữ rồi bấm Enter); GoBot cũng tự xoá và đóng khi không ai dùng chuột hay "
                                      "bàn phím.");
                else
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

    // And nothing at all where it cannot be seen. Dọn rác's catalog lays out
    // every icon of every group -- a thousand-odd rects each -- and plotted
    // all of them on every frame, scrolled into view or not. Only the
    // plotting is skipped: the button below is made all the same, with the
    // same id and size, so the layout, the clicks and the tooltips are what
    // they were, and so is what this returns.
    const bool visible = ImGui::IsRectVisible(ImVec2(w * scale, h * scale));

    for (int y = 0; visible && y < h; y += step) {
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
                retarget(w.hwnd);
                app_config_.window_title = w.title;
                app_config_.process_name = w.process;
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
    // And the pet's, which has no figures printed on it.
    const struct {
        const char* name; float pct; ImVec4 col; int value; int max;
    } kShow[] = {
        { "HP", hp, ImVec4(0.85f, 0.25f, 0.25f, 1.0f),
          vals.have_hp() ? vals.hp : -1, vals.hp_max },
        { "MP", mp, ImVec4(0.25f, 0.45f, 0.90f, 1.0f),
          vals.have_mp() ? vals.mp : -1, vals.mp_max },
        { "Pet", vitals_.pet_percent(), ImVec4(0.90f, 0.50f, 0.30f, 1.0f), -1, -1 },
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
    if (vitals_running_) {
        // Where the pet's bar is: found on the HUD by itself, not set here.
        const BarSpec pet = vitals_.pet_bar();
        if (pet.valid())
            text_dim("pet's bar at %d,%d, %d wide -- found by itself%s", pet.x, pet.y, pet.width,
                     vitals_.pet_percent() < 0.0f ? ", not there now" : "");
        else
            text_dim("pet's bar not found: summon the pet -- its HP is the red bar in its slot under HP and MP");
    }
    // What the game's packets say of HP and MP, checked against these bars
    // before anything goes by them (packets_vitals.cpp).
    text_dim("%s", packet_vitals_text().c_str());

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
    //
    // Searched on a frame read after the click (full_find_bars_tick), not on
    // this one: up to a second old while nothing runs (vitals_idle_ms), maybe
    // grabbed while this window still covered the game. The time first.
    ImGui::BeginDisabled(!have_frame || full_find_bars_);
    if (ImGui::Button(full_find_bars_ ? "FINDING..." : "FIND THE BARS", ImVec2(200, 32))) {
        full_find_bars_since_ms_ = now_ms();
        vitals_for_job();
        full_find_bars_ = true;
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

    // The least GoBot's window is made is in its own layout's pixels, and
    // Windows shows that window larger above 100% display scale: everything
    // else here is in real ones.
    const WindowScaling own = window_scaling(self);
    const auto shown = [&](int layout) { return own.known ? layout * own.percent / 100 : layout; };
    if (best_room >= shown(360)) {
        const int want_w = std::min(w, best_room - shown(8));
        const int want_h = std::min(h, screen_h - best_y - shown(8));
        SetWindowPos(self, nullptr, best_x, best_y,
                     std::max(shown(360), want_w), std::max(shown(320), want_h),
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
    // The camera tracker only while a walk or the full screen's Radar and
    // movement panels use it (camera_wanted): its search over every frame is
    // most of the detector's CPU, and nothing on the simple screen reads it.
    // tick() feeds it from the moment one does; reset below either way.
    camera_fed_ = camera_wanted();
    detector_.set_camera(camera_fed_ ? &camera_ : nullptr);

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
    // Open: the camera tracker is fed, and the detector kept (camera_wanted).
    camera_panel_frame_ = ImGui::GetFrameCount();
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

    // These numbers start wherever the view was when tracking began, and
    // start again at every map change or restart: places saved by them hold
    // for this session only. The lasting kind is to be by the game's own
    // coordinates from its packets (M27); the edge anchor never pinned them.
    text_dim("These numbers start from where tracking began, and start again at every map change "
             "or when watching starts again: places saved here hold only until then.");

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
    camera_panel_frame_ = ImGui::GetFrameCount();     // open (camera_panels_open)
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
    // Marked stopped first. The hunt's end below lets the detector go by
    // itself (release_detector), and finds it stopped already: it is stopped
    // once, here, and never through a call of this from in there.
    detector_running_ = false;
    detector_by_player_ = false;
    stop_hunt();                  // nothing to hunt with the eyes shut
    if (navigator_) navigator_->stop();   // nor anywhere to walk to
    detector_.stop();
}

void GuiApp::release_detector() {
    if (!detector_running_ || detector_wanted()) return;
    GB_INFO("Mob detector: nothing looks through it now -- stopped until the next hunt or look");
    stop_detector();
}

bool GuiApp::detector_wanted() const {
    return hunting_ || detector_by_player_ || monster_look_running() || jump_test_running() ||
           (navigator_ && navigator_->running()) || camera_panels_open();
}

bool GuiApp::camera_wanted() const {
    return (navigator_ && navigator_->running()) || camera_panels_open();
}

bool GuiApp::camera_panels_open() const {
    return ImGui::GetCurrentContext() != nullptr && camera_panel_frame_ + 1 >= ImGui::GetFrameCount();
}

void GuiApp::start_hunt() {
    vitals_for_job();       // HP and MP at the run's pace before any key
    if (hunting_) return;
    spot_refused_vi_.clear();
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
    // "Chỗ đánh quái" is where the game's packets put the character. With no
    // packets to be had -- no Npcap, or the positions switched off -- there is
    // no spot to take or walk back to, and the hunter would only ever stand:
    // refused, and said why, as the missing name is. The rest that was ticked
    // starts all the same, and the card under BẮT ĐẦU says it in red.
    if (const char* why = spot_refusal(spot_refused_vi_)) {
        simple_note("Chưa bật Tự đánh quái: " + spot_refused_vi_, 15000);
        if (!simple_view_)
            set_status("The hunting spot (spot=true) needs the game's packets: Npcap installed and running, and "
                       "positions from packets not Off",
                       12000);
        GB_WARN("Hunting not started: the hunting spot (spot=true) needs the game's packets, and %s", why);
        return;
    }
    if (!keys_may_start()) return;
    if (!detector_running_) start_detector();
    if (!detector_running_) return;

    hunter_->set_config(hunt_config_);
    // A ↓ or Ctrl a drop left down in Windows let go of, as at GoBot's start;
    // and what the drop said when it stopped the last hunt, gone.
    if (hunt_config_.drop_down) input_.release_stray_drop_keys();
    // A high jump's ↑ and Ctrl the same way, with "Nhảy lên bục trên" on.
    if (hunt_config_.jump_up && !hunt_config_.drop_down) input_.release_stray_drop_keys();
    drop_stopped_vi_.clear();

    // Same reason as the macro: the hunter refuses to send anything to an
    // unfocused window, and clicking the button is what unfocused it.
    //
    // Done after the detector is up, so the first frames it captures are of
    // the game in front rather than of this window on top of it.
    if (hunt_config_.focus_on_start) focus_game_to_start("hunting");

    // Every hunt starts on the screen: the packets earn their two seconds of
    // health afresh. Before the start, which reads the eyes' frame of
    // reference as its own.
    if (packet_eyes_) packet_eyes_->restart();
    hunter_->start();
    hunting_ = true;

    // The capture the positions come from, unless they are switched off: it
    // runs all session, and is asked for again here after a hand's stop. A
    // capture that will not start leaves the hunter on the screen, said once.
    position_capture_off_ = false;
    position_start_failed_ = false;
    position_backoff_ms_ = GameLink::kReattachEveryMs;
    position_idle_since_ms_ = 0;
    packet_looks_.clear();
    packet_target_gaps_.clear();
    packet_watch_ms_ = 0;
    if (packet_eyes_) packet_eyes_seen_ = packet_eyes_->stats();
    // The Positions line counts from here; the Load line's frames say what
    // came before the hunt.
    hunt_world_seen_ = packet_world_.stats();
    frames_line_ms_ = 0;
    if (hunt_config_.packet_positions != PacketPositions::Off) start_position_capture();
}

void GuiApp::stop_hunt() {
    if (!hunting_) return;
    hunter_->stop();
    hunting_ = false;
    // The capture for positions goes on: it runs all session, and F11, DỪNG
    // and F12 join no thread of it here. The Load line's frames say what
    // came during the hunt.
    frames_line_ms_ = 0;
    // And its eyes, unless something else still looks through them: the
    // detector's 7 grabs a second, and its CPU, went on to the end of the
    // session after the first F11. Quick: its wait is cut short as it stops.
    release_detector();
}

void GuiApp::toggle_hunt() {
    if (hunting_) stop_hunt(); else start_hunt();
}

// ------------------------------------------- the hunter's drop (Ctrl + ↓)

const char* GuiApp::drop_veto_now() const {
    // On the hunter's thread: tick()'s code, and HP read live.
    switch (drop_veto_.load()) {
        case kDropVetoMacro:  return "the macro is held: the character is not in the world";
        case kDropVetoWalk:   return "a navigator walk is under way";
        case kDropVetoKeeper: return "the keeper is crossing";
        case kDropVetoHotkey: return "one of GoBot's own keys is Ctrl or Down, or Up with jumps up on";
        default:              break;
    }
    // HP under the chat's floor, as the chat line and the quest round wait
    // for: a fall is no time to need a potion. Not known while a HP potion
    // key is on is as low.
    const float hp = hp_percent_now();
    if (hp >= 0.0f && hp < static_cast<float>(chat_hp_floor_.load())) return "HP is low";
    if (hp < 0.0f && drop_hp_watch_.load()) return "HP is not known";
    return nullptr;
}

void GuiApp::drop_tick() {
    // What only this thread may read, for the hunter's veto: worked out with
    // the drop on, none with it off.
    int veto = kDropVetoNone;
    bool hp_watch = false;
    // The jumps up (2026.11.05b) go by the same veto: HP, the macro held, a
    // walk, the keeper crossing.
    if (hunt_config_.drop_down || hunt_config_.jump_up) {
        // A key of GoBot's own on Ctrl or ↓ -- or ↑, with the jumps up on: its
        // own reading of the keys (GetAsyncKeyState) sees the drop's press, or
        // a jump's, as the player's.
        const bool up = hunt_config_.jump_up;
        const auto ctrl_or_down = [up](std::uint32_t vk) {
            const std::uint32_t low = vk & 0xFFu;
            return vk != 0 && (low == 0x11u || low == 0xA2u || low == 0xA3u || low == 0x28u || (up && low == 0x26u));
        };
        if (macro_ && macro_running_ && macro_->suspended())
            veto = kDropVetoMacro;
        else if (navigator_ && navigator_->running())
            veto = kDropVetoWalk;
        else if (keeper_crossing())
            veto = kDropVetoKeeper;
        else if (ctrl_or_down(macro_config_.toggle_key) || ctrl_or_down(macro_config_.panic_key) ||
                 ctrl_or_down(hunt_config_.toggle_key) || ctrl_or_down(hotkeys_.trash_now) ||
                 ctrl_or_down(hotkeys_.login_now))
            veto = kDropVetoHotkey;
        if (macro_running_)
            for (const MacroKey& k : macro_config_.keys)
                if (k.enabled && k.keycode != 0 && macro_key_kind(k) == MacroKeyKind::HpPotion) hp_watch = true;
    }
    drop_veto_.store(veto);
    drop_hp_watch_.store(hp_watch);

    // A hunt the drop wants stopped. The map changed after a press -- the
    // player's answer 3: the hunt stops, "Nhảy xuống bậc dưới" is unticked
    // and saved, and said in red. A key-up stuck: the hunt stops.
    if (!hunting_ || !hunter_) return;
    const Hunter::DropStop stop = hunter_->drop_stop();
    if (stop == Hunter::DropStop::None) return;
    stop_hunt();
    if (stop == Hunter::DropStop::MapChanged) {
        hunt_config_.drop_down = false;
        hunter_->set_config(hunt_config_);
        const bool saved = hunt_config_.save(exe_relative("configs/hunt.ini"));
        SYSTEMTIME at{};
        GetLocalTime(&at);
        char clock[16];
        std::snprintf(clock, sizeof clock, "%02u:%02u:%02u", static_cast<unsigned>(at.wHour),
                      static_cast<unsigned>(at.wMinute), static_cast<unsigned>(at.wSecond));
        drop_stopped_vi_ = std::string("Lần nhảy lúc ") + clock +
                           " làm đổi bản đồ — GoBot đã dừng đánh quái và bỏ dấu Nhảy xuống.";
        GB_WARN("Hunter: Nhảy xuống unticked%s -- a drop's Ctrl+Down changed the map, hunting stopped",
                saved ? " and saved" : " (hunt.ini could not be written)");
        if (!simple_view_)
            set_status("Hunting stopped: a drop (Ctrl+Down) changed the map -- drop_down is off now", 30000);
    } else if (stop == Hunter::DropStop::JumpMapChanged) {
        // A jump up's key changed the map (2026.11.05b): the same, for
        // "Nhảy lên bục trên" -- the hunt stops, the tick is taken off and
        // saved, and said in red.
        hunt_config_.jump_up = false;
        hunter_->set_config(hunt_config_);
        const bool saved = hunt_config_.save(exe_relative("configs/hunt.ini"));
        SYSTEMTIME at{};
        GetLocalTime(&at);
        char clock[16];
        std::snprintf(clock, sizeof clock, "%02u:%02u:%02u", static_cast<unsigned>(at.wHour),
                      static_cast<unsigned>(at.wMinute), static_cast<unsigned>(at.wSecond));
        drop_stopped_vi_ = std::string("Lần nhảy lên lúc ") + clock +
                           " làm đổi bản đồ — GoBot đã dừng đánh quái và bỏ dấu Nhảy lên.";
        GB_WARN("Hunter: Nhảy lên unticked%s -- a jump up's key changed the map, hunting stopped",
                saved ? " and saved" : " (hunt.ini could not be written)");
        if (!simple_view_)
            set_status("Hunting stopped: a jump up changed the map -- jump_up is off now", 30000);
    } else if (stop == Hunter::DropStop::BothMapChanged) {
        // A drop and a jump up were both under watch as the map changed
        // (2026.11.05b): either may have done it -- both ticks taken off and
        // saved, and said in red.
        hunt_config_.drop_down = false;
        hunt_config_.jump_up = false;
        hunter_->set_config(hunt_config_);
        const bool saved = hunt_config_.save(exe_relative("configs/hunt.ini"));
        SYSTEMTIME at{};
        GetLocalTime(&at);
        char clock[16];
        std::snprintf(clock, sizeof clock, "%02u:%02u:%02u", static_cast<unsigned>(at.wHour),
                      static_cast<unsigned>(at.wMinute), static_cast<unsigned>(at.wSecond));
        drop_stopped_vi_ = std::string("Lần nhảy lúc ") + clock +
                           " làm đổi bản đồ — GoBot đã dừng đánh quái và bỏ dấu Nhảy xuống và Nhảy lên.";
        GB_WARN("Hunter: Nhảy xuống and Nhảy lên unticked%s -- a drop and a jump up were both under watch when the "
                "map changed, hunting stopped",
                saved ? " and saved" : " (hunt.ini could not be written)");
        if (!simple_view_)
            set_status("Hunting stopped: the map changed after a drop and a jump up -- drop_down and jump_up are off "
                       "now",
                       30000);
    } else if (stop == Hunter::DropStop::JumpKeysStuck) {
        // A high jump's ↑ or Ctrl up stayed owed (2026.11.05b): ↑ named, not
        // ↓ -- pressing ↓ would not let go of ↑.
        drop_stopped_vi_ = "GoBot đã dừng đánh quái: phím ↑ hoặc Ctrl có thể vẫn đang bị giữ — nhấn rồi nhả ↑ và "
                           "Ctrl trên bàn phím.";
        GB_WARN("Hunter: Nhảy lên: a high jump's Up or Ctrl up stayed owed -- hunting stopped");
        if (!simple_view_) set_status("Hunting stopped: Up or Ctrl may still be held -- press and let go of both",
                                      30000);
    } else {
        drop_stopped_vi_ = "GoBot đã dừng đánh quái: phím ↓ hoặc Ctrl có thể vẫn đang bị giữ — nhấn rồi nhả ↓ và "
                           "Ctrl trên bàn phím.";
        GB_WARN("Hunter: Nhảy xuống: a drop's Down or Ctrl up stayed owed -- hunting stopped");
        if (!simple_view_) set_status("Hunting stopped: Down or Ctrl may still be held -- press and let go of both",
                                      30000);
    }
    simple_note(drop_stopped_vi_, 15000);
}

// ------------------------------------------------- positions from the packets

void GuiApp::position_say(const std::string& what) {
    if (what == position_said_) return;
    position_said_ = what;
    GB_WARN("Positions: %s", what.c_str());
}

bool GuiApp::capture_shared() {
    bool recording = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recording = recording_;
    }
    const bool autopilot = auto_stage_ != Auto::Idle && auto_stage_ != Auto::Done && auto_stage_ != Auto::Failed;
    return bot_running_ || autopilot || recording || (calibrator_ && calibrator_->running());
}

bool GuiApp::packets_wanted() const {
    // All session, hunt or no hunt and light mode or not, unless the
    // positions are switched off or the capture was stopped by hand.
    return hunt_config_.packet_positions != PacketPositions::Off && !position_capture_off_;
}

bool GuiApp::position_attaching(std::uint64_t now) {
    // The keeper bringing the game back: the new game is followed within a
    // second, so that its map entry is not missed.
    if (keeper_crossing()) return true;
    // None runs: found as soon as the game is -- for its first two minutes
    // with nothing to capture, the same game or none (a game at its login
    // with no server, a keeper that gave up), and not while a start the
    // capture refused waits out its backoff. Then every five seconds, which
    // still sees a new connection within five: each look reads the socket
    // tables, on the worker.
    if (!capture_.running()) {
        const std::uint64_t tried = now > position_tried_ms_ ? now - position_tried_ms_ : 0;
        if (position_start_failed_ && tried < position_backoff_ms_) return false;
        const std::uint64_t idle = now > position_idle_since_ms_ ? now - position_idle_since_ms_ : 0;
        return position_idle_since_ms_ == 0 || idle < kPositionIdleLooksMs;
    }
    // One that has had no report of yours since it started -- a game not yet
    // in the world, a socket opened later -- for its first five minutes.
    const std::uint64_t since = now > position_tried_ms_ ? now - position_tried_ms_ : 0;
    return packet_world_.stats().last_own_ms == 0 && since < 300'000;
}

GuiApp::PositionJobIn GuiApp::position_job_in(std::uint64_t now) {
    PositionJobIn in;
    in.window = input_.target_window();
    // While the keeper crosses, the window followed may still be the old
    // game's, or none: the new game is found by its process's name.
    if (keeper_crossing())
        in.process = app_config_.process_name.empty() ? std::string("Game.exe") : app_config_.process_name;
    in.before = position_link_;
    in.since_try = now > position_tried_ms_ ? now - position_tried_ms_ : 0;
    in.running = capture_.running();
    return in;
}

std::optional<GuiApp::PositionJobOut> GuiApp::take_position_out() {
    std::lock_guard<std::mutex> lock(position_out_mutex_);
    std::optional<PositionJobOut> out = std::move(position_out_);
    position_out_.reset();
    return out;
}

void GuiApp::start_position_capture() {
    position_tried_ms_ = now_ms();
    // A capture already running is used as it is -- the Protocol tab's,
    // Autopilot's, the packet bot's, or this one's from before -- and never
    // stopped or started again from here. Only its wrappers are opened now.
    if (capture_.running()) {
        push_framings();
        return;
    }
    // One start at a time: a job under way says how it went, and the next
    // look tries again if it must.
    if (capture_worker_.busy()) return;
    // Every frame unpacked and let through, in short batches: a frame
    // reaches the hunter within about 40 ms of arriving, some 15 px at
    // walking speed.
    push_framings();
    capture_.set_opcode_filter({});
    capture_.set_batch_interval_ms(20);
    // The game's sockets, Npcap and the adapter are looked at, and the
    // capture started, on the worker: never on this thread, which reads F12.
    capture_worker_.post([this, in = position_job_in(position_tried_ms_)] { position_job(in); });
}

void GuiApp::position_job(const PositionJobIn& in) {
    const std::uint64_t from = now_ms();
    PositionJobOut out;
    const auto done = [this, &out, from] {
        out.took_ms = now_ms() - from;
        std::lock_guard<std::mutex> lock(position_out_mutex_);
        position_out_ = out;
    };

    // The game by its window; while the keeper crosses, by its process's
    // name: the first such process with a connection of its own.
    std::uint32_t pid = pid_for_window(in.window);
    if (pid == 0 && !in.process.empty()) {
        SocketOwners owners = SocketOwners::here(in.process);
        for (const ProcessConnection& c : tcp_connections()) {
            if (c.pid != 0 && owners.takes(c.pid)) {
                pid = c.pid;
                break;
            }
        }
    }
    // No capture, and Npcap known to be missing: nothing to look at the
    // sockets for. Looked at again a minute on.
    if (!in.running && pid != 0 && now_ms() < npcap_absent_until_ms_) {
        out.link.pid = pid;
        out.tried = true;
        out.npcap = false;
        return done();
    }
    // Its traffic by its process: its connections to the server, and its own
    // UDP sockets, which its reports of where your character is go out of,
    // to wherever they go.
    std::vector<ProcessConnection> conns;
    out.link = game_link_of(pid, conns);
    out.conns = conns.size();

    if (in.running && !in.stop_first) {
        // A capture runs: started again only for a reason -- a new process,
        // connection or UDP port, or its TCP gone quiet (should_reattach).
        const char* why = nullptr;
        if (!should_reattach(in.before, out.link, in.silent, in.since_try, &why)) return done();
        out.why = why != nullptr ? why : "?";
    } else if (in.failed && in.backoff != 0 && !should_reattach(in.before, out.link, 0, in.since_try)) {
        // The capture itself refused the last start, and the same sockets
        // would be refused the same way: at once only for a new process,
        // connection or UDP port, otherwise once the backoff has gone by.
        return done();
    }
    out.tried = true;
    if (pid == 0 || conns.empty()) return done();      // said by position_started

    // Asked only with a game to capture: it is a look at Npcap's service.
    if (!PacketCapture::npcap_available()) {
        npcap_absent_until_ms_ = now_ms() + 60'000;
        out.npcap = false;
        return done();
    }

    CaptureConfig cfg;
    // The adapter the game's own socket is bound to: no default route
    // guessed, and each address looked up once. Only an adapter found by
    // that address is kept: the default, when none was, is looked for again
    // at the next start, and so is one the capture refused (below).
    const std::string local_ip = ip_to_string(conns.front().local.ip);
    std::string iface;
    if (const auto known = iface_by_ip_.find(local_ip); known != iface_by_ip_.end()) {
        iface = known->second;
    } else {
        iface = PacketCapture::find_interface_by_ip(local_ip);
        if (!iface.empty()) iface_by_ip_[local_ip] = iface;
    }
    if (iface.empty()) iface = PacketCapture::default_interface();
    cfg.interface_name = iface;
    out.iface = cfg.interface_name;
    // This game's traffic and no other's: a second client on this PC talks
    // to the same server, and its map entries and monsters would be taken
    // for this one's. A socket the game opens later is let in by the
    // re-attach (position_reattach).
    cfg.bpf_filter = build_own_filter_for(conns, out.link.udp_ports);
    // Read straight out of the game's socket table: direction labelled
    // exactly rather than guessed.
    for (const ProcessConnection& c : conns) cfg.known_servers.push_back(c.remote);
    cfg.read_timeout_ms = 20;

    // The same game session -- the same process, with no connection the
    // capture was not built with, the capture running until now -- keeps the
    // place it knows: the map entry was seen, and no other came while it ran.
    // A connection that has closed since hides no map entry: what it carried
    // was captured. A new process or a new connection starts the world
    // afresh.
    const auto all_in = [](const std::vector<Endpoint>& a, const std::vector<Endpoint>& b) {
        for (const Endpoint& e : a)
            if (std::find(b.begin(), b.end(), e) == b.end()) return false;
        return true;
    };
    bool same = false;
    if (pid == in.before.pid && in.running) same = all_in(out.link.servers, in.before.servers);
    // Stopped before the world is touched: no frame of the old capture comes
    // after it under the new reckoning.
    if (capture_.running()) capture_.stop();
    if (same) {
        packet_world_.relink(now_ms());
        packet_self_.relink();
    } else {
        packet_world_.reset();
        packet_self_.reset();
    }
    out.kept_world = same;
    // Every frame in short batches: one reaches the hunter within about 40
    // ms of arriving. Set here, with the old capture's thread joined and no
    // other start under way, for every start this job makes.
    capture_.set_batch_interval_ms(20);
    out.started = capture_.start(cfg);
    out.refused = !out.started;
    // An adapter the capture would not open is looked up afresh next time.
    if (out.refused) iface_by_ip_.erase(local_ip);
    done();
}

void GuiApp::position_started(const PositionJobOut& out) {
    // Nothing tried: the capture as it was.
    if (!out.tried) return;
    // Nothing to capture -- no Npcap, no game, no connection: since when, for
    // the same game or none (position_attaching). A start made, or another
    // game, and the game is looked for every second again.
    const bool idle = !out.npcap || out.link.pid == 0 || out.conns == 0;
    if (!idle) {
        position_idle_since_ms_ = 0;
    } else if (position_idle_since_ms_ == 0 || out.link.pid != position_idle_pid_) {
        position_idle_since_ms_ = now_ms();
        position_idle_pid_ = out.link.pid;
    }
    // What the start found of Npcap, for the reason PacketEyes gives. Only a
    // start that got as far as asking says it is there.
    if (!out.npcap) {
        position_npcap_ok_ = false;
        position_say("no capture: Npcap is not installed, or its service is not running -- the hunter goes "
                     "by the screen");
        return;
    }
    if (out.link.pid == 0) {
        position_say("no capture yet: no game window -- the hunter goes by the screen");
        return;
    }
    if (out.conns == 0) {
        position_say("no capture yet: the game has no connection to its server open -- the hunter goes by the "
                     "screen, and it is tried again");
        return;
    }
    position_npcap_ok_ = true;
    // A start was made: the capture before it, if one ran, is stopped.
    position_tried_ms_ = now_ms();
    position_link_ = out.link;
    if (!out.why.empty()) {
        GB_INFO("Positions: starting the capture again -- %s", out.why.c_str());
        // Once per quiet spell: the next silence restart waits for TCP heard
        // again (position_reattach).
        if (out.why.find("no game TCP") != std::string::npos) silence_restart_armed_ = false;
    }
    if (!out.started) {
        // Refused by the capture itself -- an adapter it cannot use, no
        // rights. The same sockets would be refused the same way, the
        // capture's own lines in the log again: tried again at once only for
        // a new process, connection or UDP port, and otherwise after twice as
        // long each time (position_reattach).
        capture_running_ = false;
        position_capture_ = false;
        position_start_failed_ = true;
        position_backoff_ms_ = std::min<std::uint64_t>(position_backoff_ms_ * 2, kPositionBackoffMaxMs);
        position_say("the capture for positions would not start (the lines above say why) -- the hunter goes "
                     "by the screen");
        return;
    }
    position_start_failed_ = false;
    position_backoff_ms_ = GameLink::kReattachEveryMs;
    capture_running_ = true;
    position_capture_ = true;
    position_said_.clear();
    std::string ports;
    for (const std::uint16_t p : position_link_.udp_ports) ports += (ports.empty() ? "" : ",") + std::to_string(p);
    GB_INFO("Positions: capturing the game's own traffic for positions -- pid %u, %zu connection(s), UDP ports %s, "
            "on %s -- %s; started in %llu ms",
            static_cast<unsigned>(out.link.pid), out.conns, ports.empty() ? "none" : ports.c_str(),
            out.iface.c_str(),
            out.kept_world ? "the world kept (same game process, no new connection)"
                           : "a new game process or connection: the world starts afresh",
            static_cast<unsigned long long>(out.took_ms));
}

void GuiApp::stop_position_capture() {
    if (!position_capture_) return;
    if (!capture_.running()) {
        position_capture_ = false;
        capture_running_ = false;
        return;
    }
    // Another part may be reading it now: it stays, and positions_tick stops
    // it once they are done.
    if (capture_shared()) return;
    // On the worker: a stop joins the capture's threads. Full, it is asked
    // for again at the next tick.
    if (!capture_worker_.post([this] { capture_.stop(); })) return;
    capture_running_ = false;
    position_capture_ = false;
    GB_INFO("Positions: the capture for positions stopped -- the positions are switched off");
}

void GuiApp::positions_tick() {
    const std::uint64_t now = now_ms();
    // packet_positions=off as it stands now -- the full screen's combo
    // changes it -- for the bag's and the vitals' uses on any thread: off is
    // nothing from the packets at all, whoever captures.
    packets_off_.store(hunt_config_.packet_positions == PacketPositions::Off);
    // A job on the worker that has finished: what it started, or why not.
    if (auto out = take_position_out()) position_started(*out);
    if (!packets_wanted()) {
        // The positions switched off, or the capture stopped by hand: the
        // capture started for them goes, once nothing else is reading it --
        // and a line owed since logging back in with it.
        if (position_capture_) stop_position_capture();
        where_pending_ms_ = 0;
        return;
    }
    if (now - packet_sample_ms_ >= 1000) {
        packet_sample_ms_ = now;
        // From GoBot's start, hunt or no hunt: every door, login and
        // re-login has its map entry said -- with your last report before
        // it, as this sample keeps it after.
        note_map_entries();
        {
            const PacketWorld::View v = packet_world_.view(now);
            if (v.me.known && v.me.report_ms != 0) {
                own_seen_.set = true;
                own_seen_.x = v.me.report_x;
                own_seen_.y = v.me.report_y;
                own_seen_.report_ms = v.me.report_ms;
                own_seen_.epoch = v.map_epoch;
                own_seen_.map = v.map;
                own_seen_.area = v.area;
            }
        }
        note_capture_gaps(now);
        // The line owed since logging back in: once the capture is on the
        // game's process of now and has the character -- the world starts
        // afresh with each new game, so nothing of the old session is in
        // it -- or a minute on, with whatever it has.
        if (where_pending_ms_ != 0) {
            const bool on_game = capture_.running() && position_capture_ && position_link_.pid != 0 &&
                                 position_link_.pid == pid_for_window(input_.target_window());
            const bool placed = on_game && packet_world_.view(now).me.known;
            if (placed || now - where_pending_ms_ >= kWherePendingMs) {
                note_where(placed    ? "logged in"
                           : on_game ? "a minute after logging in, the character not placed yet"
                                     : "a minute after logging in, the capture not on the new game");
                where_pending_ms_ = 0;
            }
        }
        more_tick(now);
        if (hunting_) packet_watch_sample(now);
    }
    // The Positions line, hunt-only: the owner reads it there.
    if (!hunting_) {
        packet_watch_ms_ = 0;
    } else if (packet_watch_ms_ == 0) {
        packet_watch_ms_ = now;
    } else if (now - packet_watch_ms_ >= 10'000) {
        packet_watch_ms_ = now;
        packet_watch_tick();
    }
    if (packets_minute_ms_ == 0) {
        packets_minute_ms_ = now;
    } else if (now - packets_minute_ms_ >= 60'000) {
        packets_minute_ms_ = now;
        packets_minute(now);
    }
    // Kept attached: every second while attaching, else every five -- asked
    // only once a second has gone by. It posts a job and never waits for one.
    const std::uint64_t since_look = now - position_checked_ms_;
    if (since_look >= 5'000 || (since_look >= 1'000 && position_attaching(now))) {
        position_checked_ms_ = now;
        position_reattach(now);
    }
}

void GuiApp::packet_watch_sample(std::uint64_t now) {
    if (!capture_.running() || packet_looks_.size() >= 64) return;
    // Your plate seen in this very frame, or nothing is compared: one only
    // remembered is where you were.
    Nameplate me;
    bool fresh = false;
    ViewComparison c;
    if (detector_.player(me, &fresh) && fresh) {
        const std::vector<Nameplate> plates = detector_.mobs();
        int w = 0, h = 0;
        input_.window_size(w, h);
        // A walk followed as PacketEyes follows it: 3 s while the hunter holds it.
        const std::uint64_t follow = hunter_ && now <= hunter_->walk_held_until_ms() ? PacketWorld::kOwnReckonMs
                                                                                       : PacketWorld::kOwnStepReckonMs;
        c = compare_views(packet_world_.view(now, follow), now, me, plates, detect_tuning_.resolved(w, h).area);
    }
    // While the packets steer: how far the target's dx, as the hunter goes by
    // it, is from its dx on the screen. The target is found among the pairs
    // by where the packets put it for the hunter.
    const Hunter::Status hs = hunter_ ? hunter_->status() : Hunter::Status{};
    if (c.valid && hs.has_target && packet_eyes_ && packet_eyes_->source() == PacketEyes::Source::Packets) {
        const double scale = hunt_config_.packet_scale > 0.0f ? static_cast<double>(hunt_config_.packet_scale) : 1.0;
        const double you_x = me.x() + static_cast<double>(c.offset_x);
        const double you_y = me.y() + static_cast<double>(c.offset_y);
        int gap = -1, best = 0;
        for (const ViewComparison::Pair& p : c.pairs) {
            const long x = std::lround((you_x + p.world_dx) / scale);
            const long y = std::lround((you_y + p.world_dy) / scale) + hunt_config_.packet_dy;
            const int d = static_cast<int>(std::labs(x - hs.target.x()) + std::labs(y - hs.target.y()));
            if (d > 40 || (gap >= 0 && d >= best)) continue;
            best = d;
            gap = static_cast<int>(std::labs(std::lround(p.world_dx / scale) - p.screen_dx));
        }
        if (gap >= 0) packet_target_gaps_.push_back(gap);
    }
    packet_looks_.push_back(std::move(c));
}

void GuiApp::note_map_entries() {
    // By its time, not the count: a capture started again counts from 0.
    const PacketWorld::Stats ws = packet_world_.stats();
    if (ws.entry_ms == 0 || ws.entry_ms == packet_entry_said_ms_) return;
    packet_entry_said_ms_ = ws.entry_ms;
    // Put somewhere off the file the place is taken for: the two numbers are
    // not its theme and stage, or not always.
    const int on = on_place(ws.entry_map, ws.entry_area, ws.entry_x, ws.entry_y);
    // Where it came from: the place said before this one, so that a door is
    // its two ends -- what lasting spots are to be keyed by.
    char from[48] = "";
    if (entry_from_map_ != 0 || entry_from_area_ != 0)
        std::snprintf(from, sizeof from, " (from t%u_s%u)", static_cast<unsigned>(entry_from_map_),
                      static_cast<unsigned>(entry_from_area_));
    // Who took the character there (2026.11.04d): your last report before
    // it, by the 1-s sample -- from an earlier map entry than this one --
    // GoBot's own last key or click, and the PC's last input. One newer than
    // GoBot's own was not GoBot's: a hand on the keys. Key names only.
    std::string before = "; before it: ";
    const std::uint32_t epoch = packet_world_.place().epoch;
    if (own_seen_.set && own_seen_.epoch < epoch) {
        char seen[160];
        std::snprintf(seen, sizeof seen, "you last placed at %ld,%ld on t%u_s%u, %.1f s before",
                      std::lround(own_seen_.x), std::lround(own_seen_.y), static_cast<unsigned>(own_seen_.map),
                      static_cast<unsigned>(own_seen_.area),
                      ws.entry_ms >= own_seen_.report_ms
                          ? static_cast<double>(ws.entry_ms - own_seen_.report_ms) / 1000.0 : 0.0);
        before += seen;
    } else {
        before += "no report of yours since the entry before";
    }
    before += "; GoBot's last key or click: " + InputManager::last_input_text(InputManager::last_input(), ws.entry_ms);
    LASTINPUTINFO last{};
    last.cbSize = sizeof last;
    if (GetLastInputInfo(&last)) {
        const std::uint64_t age = GetTickCount() - last.dwTime;
        const std::uint64_t now = now_ms();
        const std::uint64_t at = now > age ? now - age : 0;
        char pc[64];
        std::snprintf(pc, sizeof pc, "; the PC's last input %.1f s %s",
                      static_cast<double>(at <= ws.entry_ms ? ws.entry_ms - at : at - ws.entry_ms) / 1000.0,
                      at <= ws.entry_ms ? "before" : "after");
        before += pc;
    }
    GB_INFO("Positions: map entry %u/%u = %s -- you at %ld,%ld%s%s (%llu map entries since the world started "
            "afresh)%s",
            static_cast<unsigned>(ws.entry_map), static_cast<unsigned>(ws.entry_area),
            place_text(ws.entry_map, ws.entry_area).c_str(), std::lround(ws.entry_x), std::lround(ws.entry_y), from,
            on == 0 ? ", OFF that map's file" : "", static_cast<unsigned long long>(ws.map_entries), before.c_str());
    entry_from_map_ = ws.entry_map;
    entry_from_area_ = ws.entry_area;
}

std::string GuiApp::place_text(std::uint16_t map, std::uint16_t area) {
    if (!map_table_tried_) {
        map_table_tried_ = true;
        const std::string path = MapTable::default_path();
        if (path.empty() || !map_table_.load(path))
            GB_INFO("Positions: the game's list of places (zVietnam\\Chart\\tb_Map.tbl) was not found -- a map "
                    "is said by its numbers only");
    }
    char stem[32];
    std::snprintf(stem, sizeof stem, "t%u_s%u", static_cast<unsigned>(map), static_cast<unsigned>(area));
    std::string out = stem;
    for (const MapTable::Entry& e : map_table_.entries())
        if (e.theme == map && e.stage == area) {
            out += " \"" + e.name + "\"";
            break;
        }
    on_place(map, area, 0.0f, 0.0f);        // its size, looked up once
    const auto size = place_sizes_.find(stem);
    if (size != place_sizes_.end() && size->second.first > 0)
        out += " " + std::to_string(size->second.first) + "x" + std::to_string(size->second.second) + " px";
    return out;
}

int GuiApp::on_place(std::uint16_t map, std::uint16_t area, float x, float y) {
    char stem[32];
    std::snprintf(stem, sizeof stem, "t%u_s%u", static_cast<unsigned>(map), static_cast<unsigned>(area));
    auto it = place_sizes_.find(stem);
    if (it == place_sizes_.end()) {
        // Read once, the whole file for its size: a few hundred KB, at a map
        // entry or the 10-s line.
        std::pair<int, int> size{ 0, 0 };
        const std::string dir = MapFile::default_map_dir();
        const std::string path = dir.empty() ? std::string() : MapFile::find(dir, stem);
        MapFile file;
        if (!path.empty() && file.load(path)) size = { file.width_px(), file.height_px() };
        it = place_sizes_.emplace(stem, size).first;
    }
    const auto [w, h] = it->second;
    if (w <= 0 || h <= 0) return -1;
    constexpr float kSlack = 64.0f;     // as MapLocator allows
    return x >= -kSlack && y >= -kSlack && x <= w + kSlack && y <= h + kSlack ? 1 : 0;
}

void GuiApp::note_where(const char* when) {
    if (!capture_.running()) {
        GB_INFO("Positions: %s -- no capture", when);
        return;
    }
    const std::uint64_t now = now_ms();
    const PacketWorld::View v = packet_world_.view(now);
    std::ostringstream line;
    line << "Positions: " << when << " -- you " << (!v.me.known ? "not known" : v.me.uncertain ? "uncertain" : "known");
    if (v.me.known) line << " at " << std::lround(v.me.x) << "," << std::lround(v.me.y);
    line << ", map ";
    if (v.map_since_ms == 0)
        line << "not known (no map entry since the capture began)";
    else
        line << v.map << "/" << v.area << " = " << place_text(v.map, v.area) << " (entered "
             << decimals(static_cast<double>(now > v.map_since_ms ? now - v.map_since_ms : 0) / 1000.0, 1)
             << " s ago)";
    GB_INFO("%s", line.str().c_str());
}

void GuiApp::packet_watch_tick() {
    const std::uint64_t now = now_ms();
    const auto ago = [now](std::uint64_t at) {
        return decimals(static_cast<double>(now > at ? now - at : 0) / 1000.0, 1) + " s ago";
    };
    // Counted from HUNT ON: the capture runs all session, and its counts with it.
    const PacketWorld::Stats ws = packet_world_.stats().since(hunt_world_seen_);
    const PacketWorld::View v = packet_world_.view(
        now, hunter_ && now <= hunter_->walk_held_until_ms() ? PacketWorld::kOwnReckonMs : PacketWorld::kOwnStepReckonMs);
    const ViewSummary sum = summarize_views(packet_looks_);
    const PacketEyes::Stats es = packet_eyes_ ? packet_eyes_->stats() : PacketEyes::Stats{};
    const PacketEyes::Stats& was = packet_eyes_seen_;

    std::ostringstream line;
    line << "Positions (" << packet_positions_name(hunt_config_.packet_positions) << "): ";

    // The capture.
    if (!capture_.running()) {
        line << "no capture";
    } else {
        const PacketCapture::Stats cs = capture_.stats();
        line << "capture " << (position_capture_ ? "for positions" : "shared") << ", game TCP "
             << (cs.last_tcp_ms == 0 ? std::string("none yet") : ago(cs.last_tcp_ms)) << ", UDP "
             << cs.udp_datagrams << " datagrams, wrappers " << cs.unwrap.unpacked << " unpacked "
             << cs.unwrap.unpack_failed << " not";
    }

    // You.
    line << "; you: " << (!v.me.known ? "not known" : v.me.uncertain ? "uncertain" : "known");
    if (v.me.known) line << " at " << std::lround(v.me.x) << "," << std::lround(v.me.y);
    if (v.me.airborne) line << " (in the air)";
    // The jumps by kind, and the casts made in the air: what a jump attack or
    // a double jump would show. The walk-offs off an edge are among the jumps,
    // and said apart from them (2026.11.05b).
    line << ", reports 0027:" << ws.own(OwnReport::Move) << " 002A:" << ws.own(OwnReport::Air) << " (jumps "
         << ws.air_jumps << " (walk-offs " << ws.air_walkoffs << "), second " << ws.air_second << ", high "
         << ws.air_high << ", landed " << ws.air_landed
         << ", other " << ws.air_other << ")"
         << " 0029:" << ws.own(OwnReport::Other) << " 0028:" << ws.own(OwnReport::Here)
         << " 0031:" << ws.own(OwnReport::Cast) << " (" << ws.casts_in_air << " in the air), foreign " << ws.foreign
         << ", walks " << ws.own_walks
         << " (" << ws.own_walks_stopped << " stopped within " << PacketWorld::kStopWithinMs / 1000 << " s, "
         << ws.own_walks_unstopped << " not)";
    if (ws.last_own_ms != 0) line << ", last " << ago(ws.last_own_ms);

    // The map and its monsters.
    std::size_t alive = 0;
    for (const PacketWorld::Mob& m : v.mobs)
        if (!m.dead) ++alive;
    std::uint64_t wrong_size = 0;
    for (const PacketWorld::Stats::Frames& f : ws.frames) wrong_size += f.wrong_size;
    line << "; map " << v.map << "/" << v.area;
    // Which place, and whether you stand on its file: a check, hunt after
    // hunt, that the numbers are the game's theme and stage.
    if (v.map_since_ms != 0) {
        line << " = " << place_text(v.map, v.area);
        if (v.me.known) {
            const int on = on_place(v.map, v.area, v.me.x, v.me.y);
            if (on >= 0) line << (on ? " (you on it)" : " (you OFF it)");
        }
    }
    // The kills by whose they were, and the hurts of states 6 and 7: chained
    // near all of them proves the HP and the damage, and yours among them
    // the hitter at 48 (the packets plan, item B).
    line << ", monsters " << alive << " alive of " << v.mobs.size()
         << (v.table_seen ? ", table seen" : ", no table") << ", events: stops " << ws.stops << " walks "
         << ws.walks << " hits " << ws.hits << " kills " << ws.kills << " (yours " << ws.kills_yours
         << ", others' " << ws.kills_others << ", before your id " << ws.kills_before_id << ") hurts " << ws.hurts
         << " (chained " << ws.hurts_chained << ", rose " << ws.hurts_rose << ", far " << ws.hurts_far
         << ", new slot " << ws.hurts_new_slot << ", on dead " << ws.hurts_on_dead << ", yours " << ws.hurts_yours
         << ", others' " << ws.hurts_others << ") contacts " << ws.contacts << " (yours " << ws.contacts_yours
         << ", others' " << ws.contacts_others << ") spawns " << ws.spawns << " tables " << ws.tables
         << " map entries " << ws.map_entries << ", unknown states " << ws.unknown_states;
    // Which, and how many of them put their monster where they said.
    if (ws.unknown_states != 0) {
        line << " (";
        const char* sep = "";
        for (const PacketWorld::Stats::Kind& k : ws.unknown_kinds) {
            if (k.count == 0) continue;
            line << sep << "state " << static_cast<unsigned>(k.state) << ": " << k.count << ", " << k.placed
                 << " placed";
            // How far off where the monster was worked out to be: steady and
            // small, those bytes are its place.
            if (k.judged != 0)
                line << ", " << decimals(k.off_px / static_cast<double>(k.judged), 0) << " px off on average";
            sep = "; ";
        }
        line << ")";
    }
    // The frames far from where their monster was worked out to be -- placed
    // all the same (a chained hurt, a contact) or refused -- and the walks
    // followed faster than a monster walks: what leaves a monster 1,000 px
    // and more from its own frames. Each far one is said in the log as well
    // (PacketWorld::note_far).
    line << ", far frames placed " << ws.far_corrections << " refused " << ws.far_refused << ", walks faster than "
         << decimals(PacketWorld::kPlausibleWalkSpeed, 0) << " px/step " << ws.walks_fast;
    line << ", wrong size " << wrong_size;
    // Your id, which the hitters are weighed against, and the first few
    // hitters met since HUNT ON; and the minimap's map changes the packets
    // overruled since GoBot started (tick()).
    line << "; your id ";
    if (v.me.id != 0) line << v.me.id << (v.actor_checked ? " (proven the hitter's)" : "");
    else              line << "not known";
    line << ", hitters";
    bool any_hitter = false;
    for (const PacketWorld::Stats::Hitter& h : ws.hitters) {
        if (h.hits == 0) continue;
        line << ' ' << h.id << "×" << h.hits;
        any_hitter = true;
    }
    if (!any_hitter) line << " none";
    line << ", minimap changes overruled " << minimap_overruled_;

    // What the packets say beside what the screen shows -- and, next to what
    // was measured, what hunt.ini has, so the two can be set from this line.
    line << "; vs screen: " << sum.compared << " of " << sum.looks << " looks compared, " << sum.pairs << " pairs";
    if (sum.median_x && sum.median_y)
        line << ", residual x " << decimals(*sum.median_x, 0) << " y " << decimals(*sum.median_y, 0)
             << " px (y spread " << decimals(sum.spread_y, 0) << "), packet_dy that fits "
             << decimals(-*sum.median_y, 0) << " (packet_dy=" << hunt_config_.packet_dy << ")";
    line << ", scale " << (sum.scale ? decimals(*sum.scale, 2) : std::string("-")) << " (packet_scale="
         << decimals(hunt_config_.packet_scale, 2) << "), screen only " << sum.screen_only << ", packets only "
         << sum.packets_only;
    if (!packet_target_gaps_.empty()) {
        std::vector<int> gaps = packet_target_gaps_;
        std::sort(gaps.begin(), gaps.end());
        line << ", target |dx| packets vs screen: median " << gaps[gaps.size() / 2] << " px over " << gaps.size();
    }

    // Where the hunter's positions came from.
    const std::uint64_t spent = (es.packets_ms - was.packets_ms) + (es.screen_ms - was.screen_ms);
    const auto share = [spent](std::uint64_t ms) {
        return spent == 0 ? 0ull : static_cast<unsigned long long>((100 * ms + spent / 2) / spent);
    };
    line << "; source: packets " << share(es.packets_ms - was.packets_ms) << "% of the time, healthy "
         << share(es.healthy_ms - was.healthy_ms) << "%, switches " << (es.switches - was.switches) << ", now "
         << (packet_eyes_ ? packet_why_name(packet_eyes_->why()) : "?");
    // The hunter's plates of the packets' monsters (2026.11.05b): how many of
    // the map's living ones are in view, and how they were placed on floors.
    if (packet_eyes_ && packet_eyes_->source() == PacketEyes::Source::Packets) {
        const PacketEyes::PlateCounts pc = packet_eyes_->plates();
        line << "; the hunter's plates: " << pc.in_view << " of " << pc.alive << " living monsters in view (±"
             << pc.half_w << " x ±" << pc.half_h << ")";
        if (pc.floors)
            line << "; placed on floors: " << pc.walking << " walking (" << pc.projected << " projected past "
                 << PacketWorld::kMobWalkMs / 1000 << " s), " << pc.set_down << " set down";
        else
            line << "; not placed on floors (no map file in use)";
    }

    // The hunting spot ("Chỗ đánh quái"), when it is switched on: where it
    // is, how far off the character is, the walks back (simple_spot.cpp).
    if (hunt_config_.spot) line << "; " << spot_positions_text();

    GB_INFO("%s", line.str().c_str());
    packet_looks_.clear();
    packet_target_gaps_.clear();
    packet_eyes_seen_ = es;
}

void GuiApp::position_reattach(std::uint64_t now) {
    // Stopped by hand: it stays stopped until the next hunt. Somebody else's
    // capture is theirs, as it is; and one another part is reading now is
    // not pulled from under it.
    if (position_capture_off_) return;
    if (capture_.running() && (!position_capture_ || capture_shared())) return;
    // One job at a time: the one under way says how it went, and the next
    // look asks again.
    if (capture_worker_.busy()) return;
    PositionJobIn in = position_job_in(now);
    const std::uint64_t since_try = in.since_try;
    if (!in.running) {
        // None runs. The last start found no game, no connection or no
        // Npcap, which logs nothing new: said once, and tried again at the
        // next look, to attach as soon as the game connects. Or the capture
        // itself refused it, and every try puts its lines in the log again:
        // then at once only for a new process, connection or UDP port -- the
        // job asks should_reattach -- and otherwise less and less often.
        if (position_start_failed_) {
            in.failed = true;
            if (since_try < position_backoff_ms_) in.backoff = position_backoff_ms_;
        }
        // A capture the job may start: protocol.ini's framing for both
        // sides, its 0x0081 rule, and every opcode let through, as
        // start_position_capture hands them -- no capture runs to read them
        // meanwhile, and the worker is idle.
        push_framings();
        capture_.set_opcode_filter({});
    } else {
        // Silent since its last segment: the game's traffic has gone
        // somewhere else -- another adapter, a connection the table does not
        // show yet. One that has heard none since it started is not started
        // again for that alone: the same sockets would give the same
        // capture. And once per quiet spell: after a start again for it, not
        // again until the game's TCP has been heard.
        const PacketCapture::Stats st = capture_.stats();
        if (!silence_restart_armed_ && st.last_tcp_ms != 0) silence_restart_armed_ = true;
        if (silence_restart_armed_ && st.last_tcp_ms != 0 && now > st.last_tcp_ms) in.silent = now - st.last_tcp_ms;
    }
    // The job reads the socket tables, decides and starts the capture again
    // if it must, on the worker; positions_tick takes in what it did.
    capture_worker_.post([this, in] { position_job(in); });
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
    changed |= ImGui::Checkbox("Hold the walk key##hunt", &hunt_config_.hold_walk);
    help_marker("Walk to a monster with the arrow held down until the walk is over, as "
                "a player walks. Off, every look presses a step of the length beside "
                "it and lets go -- the character stops between steps, several times a "
                "second.");
    if (!hunt_config_.hold_walk) {
        ImGui::SameLine();
        int step_ms = static_cast<int>(hunt_config_.step_ms);
        ImGui::SetNextItemWidth(160);
        if (ImGui::SliderInt("Step (ms)", &step_ms, 40, 500)) {
            hunt_config_.step_ms = static_cast<std::uint32_t>(step_ms);
            changed = true;
        }
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
    // "Nhảy lên bục trên" (2026.11.05b) beside the old hop's box: its reasons
    // not to are the simple screen's tick's -- greyed, but a ticked one may
    // still be unticked.
    {
        ImGui::SameLine();
        const std::string why = simple_jump_up_refusal_vi();
        ImGui::BeginDisabled(!why.empty() && !hunt_config_.jump_up);
        changed |= ImGui::Checkbox("Jump up onto floors above (Up / Up Up / Ctrl+Up)", &hunt_config_.jump_up);
        ImGui::EndDisabled();
        help_marker("Straight up, by the game's own map file with a margin -- Up, Up Up or Ctrl+Up -- onto the floor "
                    "of a monster above, and onto the floors of the way back to the home. Only in a hunting area "
                    "drawn on the Bản đồ tab, on a map GoBot trusts, packets On, never with Stand still, the jump key "
                    "Up; the take-off, the landing and the monster inside the area, and no floor outside it within "
                    "650 px above. 4 a minute, 2 per monster; 3 that did not get up in a row, or 5 in a run, switch "
                    "it off until the next start. A map change within 10 s of a jump stops hunting and switches this "
                    "off. Off: nothing new is pressed, and the log says where a jump would have gone. Ticked, where "
                    "jumps up may be planned (the jump key Up, packets On, the map trusted), the old hop beside it "
                    "never presses in such an area, off for the run too; anywhere else it hops as before.");
    }
    changed |= ImGui::Checkbox("Only while the game has focus##hunt",
                               &hunt_config_.require_focus);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Bring the game to the front when starting##hunt",
                               &hunt_config_.focus_on_start);
    help_marker("Clicking START HUNTING takes focus off the game, and the hunter "
                "will not send anything to an unfocused window -- so without this "
                "it starts and waits forever. Keeping it in front while running is "
                "the next box.");
    {
        bool keep = simple_opts_.refocus;
        if (ImGui::Checkbox("Keep it in front while running##hunt", &keep)) {
            simple_opts_.refocus = keep;
            save_simple_option("refocus", keep);
        }
        help_marker("While anything runs -- the hunter, the macro, the packet bot, or whatever else the "
                    "simple screen's stop button would stop -- the game is brought back to the front "
                    "at once whenever another window takes it -- GoBot's own too -- a moment (0.2 s) "
                    "after the last click or key, so a click lands first. Anything typed into another "
                    "window after that goes to the game. The same switch as the box under the simple "
                    "screen's start button.");
    }
    // "Nhảy xuống bậc dưới": its reasons not to are the simple screen's tick's.
    {
        const std::string why = simple_drop_refusal_vi();
        ImGui::BeginDisabled(!why.empty());
        changed |= ImGui::Checkbox("Drop to things below you (Ctrl+Down)", &hunt_config_.drop_down);
        ImGui::EndDisabled();
        help_marker("Packets only; walking only, never with Stand still; with the hunting spot, only inside its "
                    "area; maps GoBot saw you enter, or named by where you and the monsters stand; never "
                    "near a gate (drop_gate_px); never a bare Down: Down goes only with Ctrl held. A map "
                    "change after a drop stops hunting and switches this off.");
    }
    // "Tự tìm quái trong vùng" (2026.11.05b): its reasons not to are the
    // simple screen's tick's.
    {
        ImGui::SameLine();
        const std::string why = simple_seek_refusal_vi();
        ImGui::BeginDisabled(!why.empty());
        changed |= ImGui::Checkbox("Seek monsters anywhere in the area", &hunt_config_.seek_in_zone);
        ImGui::EndDisabled();
        help_marker("With the Bản đồ tab's hunting area in use, the packets steering and Stand still off: go to any "
                    "monster inside the area the character gets to -- in view or not -- walking, off a floor's end "
                    "or by a jump up, and fight it from a standing point inside the area. One in reach first, then "
                    "your own floor, below, above. With none to get to, stand where it is; never out of the area. "
                    "Off: as 2026.11.05.");
    }

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

    // ---- where your position and the monsters' come from ----
    {
        // In the order of PacketPositions: Off, Watch, On.
        const char* const kModes[] = { "Off: the screen only", "Watch: log the packets, fight from the screen",
                                       "On: the packets steer while healthy" };
        int mode = static_cast<int>(hunt_config_.packet_positions);
        ImGui::SetNextItemWidth(330);
        if (ImGui::Combo("Positions from packets", &mode, kModes, IM_ARRAYSIZE(kModes))) {
            hunt_config_.packet_positions = static_cast<PacketPositions>(mode);
            changed = true;
            // While hunting: the capture follows at once rather than at the
            // next start. Watch and On both capture; Off stops it.
            if (hunting_) {
                if (hunt_config_.packet_positions == PacketPositions::Off) {
                    stop_position_capture();
                } else if (!capture_.running()) {
                    position_capture_off_ = false;
                    start_position_capture();
                }
            }
        }
        help_marker("Your own position and the monsters', from the game's packets instead of the name "
                    "plates on screen. Needs Npcap (GoBot already runs as Administrator): a capture of the "
                    "game's own traffic runs from GoBot's start to its close, hunting or not, unless this is "
                    "Off or the capture was stopped by hand. Watch only writes a \"Positions\" line to "
                    "gobot_gui.log every 10 s while hunting, comparing the packets with the screen; "
                    "On fights from the packets whenever they are healthy and from the screen whenever "
                    "they are not -- no Npcap, no traffic, a map just changed, a monster list in use. "
                    "packet_scale and packet_dy in hunt.ini lay the packets over the screen.");
        if (hunting_ && packet_eyes_) {
            ImGui::SameLine();
            text_dim("%s", packet_eyes_->source() == PacketEyes::Source::Packets
                               ? "from the packets now"
                               : (std::string("from the screen: ") + packet_why_name(packet_eyes_->why())).c_str());
        }
    }

    if (changed) hunter_->set_config(hunt_config_);
}

void GuiApp::draw_radar_panel() {
    // Open: the detector kept after a hunt, and the camera fed (camera_panels_open).
    camera_panel_frame_ = ImGui::GetFrameCount();
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
        if (detector_running_) {
            stop_detector();
        } else {
            start_detector();
            // The player's: it watches on after a hunt ends, until STOP WATCHING.
            detector_by_player_ = detector_running_;
        }
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
    // The target is drawn on the picture only while the hunter goes by the
    // screen: from the packets it is in world pixels, and nothing yet maps
    // those back onto the screen.
    const bool target_on_screen =
        hunting_ && hs.has_target &&
        (!packet_eyes_ || packet_eyes_->source() == PacketEyes::Source::Screen);
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
                         target_on_screen ? &hs.target : nullptr,
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
                const bool is_target = target_on_screen && !n.is_player &&
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
            if (have_me && target_on_screen) {
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
        // The pet's apart: no pet out holds its key all along, HP and MP read.
        if (st.held_no_pet_reading) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::Text("(%llu held: no pet HP reading)",
                        static_cast<unsigned long long>(st.held_no_pet_reading));
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
    // The skills on their timers paced by your casts (cast_paced, 2026.11.08),
    // as the simple screen's "Kỹ năng" window offers it: off as shipped, and
    // kept by Apply now and Save macro like the rest of this tab.
    ImGui::Checkbox("Ra chiêu theo vòng (chờ game ra chiêu rồi mới bấm chiêu kế)##macro", &macro_config_.cast_paced);
    help_marker(cast_paced_tip_vi());
    if (macro_config_.cast_paced) text_dim("%s", cast_paced_status_vi().c_str());

    const auto due = macro_->next_due_ms();

    float live_hp, live_mp, live_pet;
    macro_->current_vitals(live_hp, live_mp, live_pet);
    // Of the potions under their mark, the one that drinks: only the more
    // urgent tier does (macro_urgent_potion), and the others wait.
    const int urgent_hp = macro_urgent_potion(macro_config_.keys, MacroKeyKind::HpPotion, live_hp);
    const int urgent_mp = macro_urgent_potion(macro_config_.keys, MacroKeyKind::MpPotion, live_mp);
    const int urgent_pet = macro_urgent_potion(macro_config_.keys, MacroKeyKind::PetPotion, live_pet);

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

        // In the order of MacroTrigger, which the choice is an index into --
        // and last a buff: on a timer, and marked as a buff (MacroKey::buff),
        // so it stays one whatever its interval, 0 included.
        static const char* const kWhen[] = { "on a timer", "HP below", "MP below", "pet HP below", "a buff" };
        constexpr int kWhenBuff = 4;

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
            int when = !k.conditional() && k.buff ? kWhenBuff : static_cast<int>(k.trigger);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##when", &when, kWhen, static_cast<int>(std::size(kWhen)))) {
                k.buff = when == kWhenBuff;
                k.trigger = k.buff ? MacroTrigger::Always : static_cast<MacroTrigger>(when);
                // A buff at 0 made anything else: 0 is off for a potion and a
                // skill, so it gets a potion's 3-s cooldown, not 0.
                if (!k.buff && k.interval_ms == 0) k.interval_ms = 3000;
            }
            if (k.trigger == MacroTrigger::PetHpBelow && ImGui::IsItemHovered())
                ImGui::SetTooltip("The pet's HP, off the red bar in the pet's slot under the HP and MP "
                                  "bars -- found on the HUD by itself while HP and MP are read.");
            if (!k.conditional() && ImGui::IsItemHovered())
                ImGui::SetTooltip("On a timer, a key is a buff at a minute or more and a skill under "
                                  "it. \"a buff\" makes it one whatever its interval: cast on its own "
                                  "clock, fight or no fight, and again after a death -- at 0, every %u s, "
                                  "as GoC casts a buff whose cooldown is 0.",
                                  static_cast<unsigned>(kBuffRepeatMinMs / 1000));

            ImGui::TableNextColumn();
            if (k.conditional()) {
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt("##thr", &k.threshold_pct, 0, 0))
                    k.threshold_pct = std::clamp(k.threshold_pct, 1, 99);
            } else {
                text_dim("-");
            }

            ImGui::TableNextColumn();
            // A pet potion's, with the pet row's "every N ms" set, is that
            // one value, for every pet potion key (MacroConfig::pet_every_ms):
            // shown and set here as it is there, never a key's own number
            // that nothing goes by.
            const bool pet_every = macro_config_.pet_every_ms != 0 && macro_key_kind(k) == MacroKeyKind::PetPotion;
            int iv = static_cast<int>(pet_every ? macro_config_.pet_every_ms : k.interval_ms);
            ImGui::SetNextItemWidth(-1);
            // A buff may be 0: as often as a buff goes, every kBuffRepeatMinMs.
            // One by its interval, unmarked as in a file from before the mark,
            // is marked as it is edited, as the simple screen's seconds box
            // does: typed under a minute it stays a buff, and not a skill
            // pressed every 10 ms.
            const bool is_buff = !k.conditional() && macro_key_kind(k) == MacroKeyKind::Buff;
            const int least = is_buff ? 0 : 10;
            if (ImGui::InputInt("##iv", &iv, 0, 0)) {
                if (is_buff) k.buff = true;
                if (pet_every)
                    macro_config_.pet_every_ms = static_cast<std::uint32_t>(
                        std::clamp(iv, static_cast<int>(MacroConfig::kPetEveryLeastMs),
                                   static_cast<int>(MacroConfig::kPetEveryMostMs)));
                else
                    k.interval_ms = static_cast<std::uint32_t>(std::max(least, iv));
            }
            if (pet_every && ImGui::IsItemHovered())
                ImGui::SetTooltip("Every pet potion key goes this often while the pet is under its mark: one value "
                                  "for all of them (pet_every_ms, \"Bơm liên tục, mỗi N ms\" on the simple screen), "
                                  "%u-%u ms.",
                                  static_cast<unsigned>(MacroConfig::kPetEveryLeastMs),
                                  static_cast<unsigned>(MacroConfig::kPetEveryMostMs));
            else if (k.conditional() && ImGui::IsItemHovered())
                ImGui::SetTooltip("For a conditional key this is the cooldown: "
                                  "the shortest gap between two firings. It does "
                                  "not delay the reaction.");

            ImGui::TableNextColumn();
            int hold = static_cast<int>(k.hold_ms);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##hold", &hold, 0, 0))
                k.hold_ms = static_cast<std::uint32_t>(std::clamp(hold, 0, static_cast<int>(MacroConfig::kHoldMostMs)));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("This key's own hold, at most %u ms: no key of the game is held down. 0 is the "
                                  "Key hold range under Options (70-110 ms as shipped).",
                                  static_cast<unsigned>(MacroConfig::kHoldMostMs));

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
                const float v = k.trigger == MacroTrigger::HpBelow   ? live_hp
                                : k.trigger == MacroTrigger::MpBelow ? live_mp
                                                                     : live_pet;
                const int urgent = k.trigger == MacroTrigger::HpBelow   ? urgent_hp
                                   : k.trigger == MacroTrigger::MpBelow ? urgent_mp
                                                                        : urgent_pet;
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
                const std::uint32_t every = macro_timer_interval_ms(k);
                const float frac = every ? 1.0f - static_cast<float>(due[i]) / static_cast<float>(every) : 0.0f;
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
    // A pet potion's own: HP read, and no pet's bar -- no pet out, or its
    // slot not on the HUD. It holds meanwhile, as any key with no reading.
    const bool pet_keys = std::any_of(macro_config_.keys.begin(), macro_config_.keys.end(), [](const MacroKey& m) {
        return m.enabled && m.keycode != 0 && macro_key_kind(m) == MacroKeyKind::PetPotion;
    });
    if (pet_keys && live_pet < 0.0f && live_hp >= 0.0f) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("A key above waits on the pet's HP, but no pet's bar is being read. Summon the pet: "
                           "its HP is the red bar in its slot under the HP and MP bars, which is found by itself.");
        ImGui::PopStyleColor();
    }
    // A key on a buff row and a skill row, both on: pressed on both clocks.
    // Said as the simple screen says it, and never refused (2026.11.08). With
    // "Repeating keys" off no row is pressed, so nothing is said.
    {
        const std::string clash = macro_config_.timers_enabled ? macro_clash_note_vi(macro_config_.keys) : std::string();
        if (!clash.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextWrapped("%s", clash.c_str());
            ImGui::PopStyleColor();
        }
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
                if (ImGui::InputInt("##h", &hold, 0, 0))
                    s.hold_ms = static_cast<std::uint32_t>(std::clamp(hold, 0, static_cast<int>(MacroConfig::kHoldMostMs)));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("This step's own hold, at most %u ms: no key of the game is held down. 0 is "
                                      "the Key hold range under Options.",
                                      static_cast<unsigned>(MacroConfig::kHoldMostMs));

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
                "immediately holds. Keeping it in front while running is the next box.");
    {
        bool keep = simple_opts_.refocus;
        if (ImGui::Checkbox("Keep the game in front while running##macro", &keep)) {
            simple_opts_.refocus = keep;
            save_simple_option("refocus", keep);
        }
        help_marker("While anything runs -- the macro, the hunter, the packet bot, or whatever else "
                    "the simple screen's stop button would stop -- the game is brought back to the "
                    "front at once whenever another window takes it -- GoBot's own too -- a moment "
                    "(0.2 s) after the last click or key, so a click lands first. Anything typed into "
                    "another window after that goes to the game.");
    }

    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Extra jitter %", &macro_config_.global_jitter_pct, 0, 50);
    help_marker("Added on top of each key's own jitter. Perfectly regular timing is "
                "the clearest sign a client is automated.");

    {
        // The game reads its keyboard once a frame, and a press shorter than a
        // frame is lost now and then: the key goes down and up and nothing
        // happens. 0 and 0 keep the input manager's own short 25-60 ms.
        int hmin = static_cast<int>(macro_config_.key_hold_min_ms);
        int hmax = static_cast<int>(macro_config_.key_hold_max_ms);
        bool edited = false;
        ImGui::SetNextItemWidth(110);
        edited |= ImGui::InputInt("Key hold, shortest (ms)", &hmin, 0, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        edited |= ImGui::InputInt("longest##hold", &hmax, 0, 0);
        help_marker("How long each key of the macro is held down: a random time between the two, "
                    "for a key with no hold of its own (the Hold columns, at most 500 ms: no key of the "
                    "game is held down). Too short and the game "
                    "now and then does not see the press: the key is pressed and nothing happens. "
                    "70 and 110 by default. 0 and 0: the short 25-60 ms of before.");
        if (edited) {
            if (hmin <= 0 && hmax <= 0) {
                macro_config_.key_hold_min_ms = macro_config_.key_hold_max_ms = 0;
            } else {
                const int lo = std::clamp(hmin, static_cast<int>(MacroConfig::kHoldLeastMs),
                                          static_cast<int>(MacroConfig::kHoldMostMs));
                macro_config_.key_hold_min_ms = static_cast<std::uint32_t>(lo);
                macro_config_.key_hold_max_ms = static_cast<std::uint32_t>(
                    std::clamp(std::max(hmax, lo), lo, static_cast<int>(MacroConfig::kHoldMostMs)));
            }
        }

        int recheck = static_cast<int>(macro_config_.potion_recheck_ms);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Re-press a potion (ms, 0 = off)", &recheck, 0,
                             static_cast<int>(MacroConfig::kRecheckMostMs))) {
            macro_config_.potion_recheck_ms =
                recheck <= 0 ? 0u
                             : static_cast<std::uint32_t>(std::max(recheck, static_cast<int>(MacroConfig::kRecheckLeastMs)));
        }
        help_marker("A potion whose bar has not risen this long after it was pressed, while it still "
                    "wants to drink, is pressed again at once instead of after its cooldown: the game "
                    "may have ignored the press. Once per press. 1000 by default; 0 is the plain "
                    "cooldown. The log says \"Potion check: ...\" each time.");

        int urgent = static_cast<int>(macro_config_.potion_urgent_ms);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Potion when far below (ms, 0 = off)", &urgent, 0,
                             static_cast<int>(MacroConfig::kUrgentMostMs))) {
            macro_config_.potion_urgent_ms =
                urgent <= 0 ? 0u
                            : static_cast<std::uint32_t>(std::max(urgent, static_cast<int>(MacroConfig::kUrgentLeastMs)));
        }
        help_marker("A potion whose bar is under half of its mark -- skills draining MP, or blows "
                    "taking HP, faster than one potion per cooldown makes up -- may drink again this "
                    "long after its last press, not after its whole interval (3 s). Between the "
                    "mark and half of it the interval stands. 1000 by default; 0 is the plain "
                    "cooldown.");

        // A buff is cast by pressing its key several times, as GoC does (4, 200 ms apart).
        int presses = static_cast<int>(macro_config_.buff_presses);
        int apart = static_cast<int>(macro_config_.buff_press_gap_ms);
        ImGui::SetNextItemWidth(110);
        const bool presses_edited = ImGui::InputInt("Buff presses per cast", &presses, 0, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        const bool apart_edited = ImGui::InputInt("apart (ms)##buff", &apart, 0, 0);
        help_marker("Each buff is cast by pressing its key this many times, this far apart: the game "
                    "drops a single press now and then, and a buff dropped is gone for its whole "
                    "interval. GoC presses 4 times, 200 ms apart, which is the default; 1 is a single "
                    "press. The log says \"Buff: key ... cast\" each time, and why when a buff cannot "
                    "be cast.");
        if (presses_edited)
            macro_config_.buff_presses = static_cast<std::uint32_t>(
                std::clamp(presses, 1, static_cast<int>(MacroConfig::kBuffPressesMost)));
        if (apart_edited)
            macro_config_.buff_press_gap_ms = static_cast<std::uint32_t>(
                std::clamp(apart, static_cast<int>(MacroConfig::kBuffPressGapLeastMs),
                           static_cast<int>(MacroConfig::kBuffPressGapMostMs)));
    }

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
    // newly mapped ones have to be let through or nothing changes. And the
    // packet world's, which are no layout's: without them your reports, the
    // map entry and the monster table would go, and the positions with them.
    std::vector<std::uint16_t> allowed = registry_.mapped_opcodes();
    const std::vector<std::uint16_t>& positions = PacketWorld::opcodes();
    allowed.insert(allowed.end(), positions.begin(), positions.end());
    // And every opcode the packets plan reads of your own character, the
    // link and the casts (PacketSelf::opcodes): HP, MP, the bag, gold, buffs,
    // the tick. The filter holds for inner and UDP frames alike.
    const std::vector<std::uint16_t>& self = PacketSelf::opcodes();
    allowed.insert(allowed.end(), self.begin(), self.end());
    capture_.set_opcode_filter(allowed);

    save_protocol();
    char msg[128];
    std::snprintf(msg, sizeof(msg), "Applied %zu packet layout(s)", specs.size());
    set_status(msg, 6000);
}

bool GuiApp::start_calibration() {
    if (!calibrator_ || calibrator_->running()) return false;
    if (!input_.window_valid()) {
        set_status("Pick the game window on the Capture tab first", 6000);
        return false;
    }
    if (!keys_may_start()) return false;
    // It walks the character itself: the hunter and a walk to a place let go
    // of the arrow it holds on every pass, and the packet bot steers too.
    if (hunting_ || bot_running_ || (navigator_ && navigator_->running())) {
        set_status("Stop hunting, walking to a place and the packet bot first -- the calibration "
                   "walks the character itself", 8000);
        return false;
    }
    if (const std::uint32_t vk = InputManager::parse_key(calib_cast_key_))
        calibrator_->set_cast_key(vk);
    // Everything else that presses keys held off for its twenty seconds: a
    // potion or a buff pressed meanwhile would be read as a field responding.
    if (!take_hold(kCalibHold)) {
        set_status("Something else has the game just now (" + coord_.critical_reason() +
                   ") -- start the calibration once it is done", 8000);
        return false;
    }
    calib_held_ = true;

    // The fields we are hunting for are in opcodes that are not mapped
    // yet, so a filter built from the known ones would discard exactly
    // the packets this needs to see.
    capture_.set_opcode_filter({});

    // Clicking the button put GoBot's own window in front, and the run
    // refuses a game that is not there.
    if (!input_.window_focused() && !input_.focus_window()) {
        stop_calibration();
        set_status("Could not bring the game to the front -- click the game once, then press "
                   "Start calibration again", 8000);
        return false;
    }
    if (!calibrator_->start()) {
        stop_calibration();
        set_status(calibrator_->status(), 8000);
        return false;
    }
    calib_ran_ = false;
    findings_.clear();
    return true;
}

void GuiApp::stop_calibration() {
    if (calibrator_) calibrator_->stop();
    if (calib_held_) {
        calib_held_ = false;
        end_hold(kCalibHold);
    }
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
    ImGui::BulletText("It brings the game to the front and drives your character for ~20 seconds.");

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
        if (ImGui::Button("Abort", ImVec2(240, 40))) stop_calibration();
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
        if (ImGui::Button(calib_ran_ ? "Run again" : "Start calibration", ImVec2(240, 40)))
            start_calibration();
        ImGui::PopStyleColor(2);
        if (!capture_running_) ImGui::EndDisabled();
    }

    // Collect the result once the run completes. Asked again rather than read
    // off `running` and `phase` above: a run the button has just started still
    // reads there as the last one, Done, and would be taken as finished --
    // analysed at once, with nothing in it, and never again.
    if (!calibrator_->running() && calibrator_->phase() == CalibPhase::Done && !calib_ran_) {
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
        }
        text_dim("Potions   the macro's -- see Timed keys");
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
        ImGui::TextWrapped("Npcap is not installed, or not running. Install it from "
                           "https://npcap.com -- 'WinPcap API-compatible Mode' is not needed.");
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
        "Pick the game window explicitly. Auto-find takes a title on its own only "
        "when it is a window's whole title, and a folder, editor or browser tab "
        "named after the game can have that title too -- the bot would then type "
        "combat keys into it.");
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
                retarget(w.hwnd);
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
        // Above 100% display scale Windows enlarges a game that does not handle
        // DPI itself, and frames of it read off the screen are blurred and the
        // wrong size: nothing the readers look for is found.
        if (game_scaling_.stretched()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextWrapped("Windows shows this window %s to %d%% (drawn %dx%d, shown %dx%d), and the "
                               "screen readers cannot work on it: %s.",
                               game_scaling_.percent >= 100 ? "enlarged" : "shrunk", game_scaling_.percent,
                               game_scaling_.own_w, game_scaling_.own_h, game_scaling_.real_w,
                               game_scaling_.real_h, cure_advice(game_scaling_cure_));
            ImGui::PopStyleColor();
            if (game_scaling_cure_ == ScaleCure::SetOverride || game_scaling_cure_ == ScaleCure::SetOverrideOrMove) {
                if (ImGui::Button("Set the game to draw at its own size")) {
                    const std::string vi = fix_game_scaling();
                    set_status(game_scaling_cure_ == ScaleCure::RestartGame
                                   ? "Set -- close the game and start it again"
                                   : "Could not set it -- see the log", 8000);
                    simple_note(vi, 12000);
                }
                ImGui::SameLine();
                text_dim("(its high-DPI override, as on the Compatibility tab)");
            } else if (!game_scaling_program_.empty() && game_scaling_cure_ == ScaleCure::OverrideIgnored) {
                text_dim("The game's program: %s", game_scaling_program_.c_str());
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextUnformatted("No target window selected");
        ImGui::PopStyleColor();
    }
    // GoBot itself held to Windows' scaling: nothing it reads is right, whatever
    // the game does.
    if (threads_view_percent() > 100) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("Windows shows GoBot itself the screen scaled to %d%%, so nothing it reads off the "
                           "screen is the screen's real pixels: %s.",
                           threads_view_percent(),
                           real_pixels_possible()
                               ? "Windows holds it to that, most likely by a high-DPI override on gobot_gui.exe's "
                                 "Compatibility tab -- take it off and start GoBot again"
                               : "this Windows is older than 10 1607 -- set its display scale to 100%");
        ImGui::PopStyleColor();
        if (own_scaling_fixed_) {
            text_dim("Taken off: close GoBot and start it again.");
        } else if (own_override_found_ && ImGui::Button("Take the high-DPI override off GoBot")) {
            const std::string vi = fix_own_scaling();
            set_status(own_scaling_fixed_ ? "Taken off -- close GoBot and start it again"
                                          : "Could not take it off -- see the log", 8000);
            simple_note(vi, 12000);
        }
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

    text_dim("Buffs and potions are the macro's: Features > Buffs, Healing.");

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
    // The macro's potion keys are the only potions: the packet bot's own, in
    // bot_default.ini's [Heal], drank beside them for the same HP drop.
    ImGui::TextWrapped("Potions are the macro's keys: \"HP below\", \"MP below\" or \"pet HP below\" under "
                       "Timed keys, or the potion rows on the simple screen -- set them there. They drink "
                       "while the macro runs; with the packet bot running, on the HP and MP its packets give.");
    bool any = false;
    for (const MacroKeyKind kind : { MacroKeyKind::HpPotion, MacroKeyKind::MpPotion, MacroKeyKind::PetPotion })
        for (const std::size_t i : macro_keys_of_kind(macro_config_.keys, kind)) {
            const MacroKey& k = macro_config_.keys[i];
            any = true;
            text_dim("%s: key %s, under %d%%%s", k.label.c_str(), InputManager::key_name(k.keycode).c_str(),
                     k.threshold_pct, k.enabled ? "" : "  (off)");
        }
    if (!any) text_dim("No potion keys yet.");

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
    if (ImGui::Button("Clear")) {
        Logger::instance().clear_ring();
        log_copied_ = false;        // shown empty at once, not up to 250 ms later
    }
    ImGui::SameLine();
    if (ImGui::Button("Verbose logging"))
        Logger::instance().set_level(LogLevel::Debug);

    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // The ring copied again only when a line has gone into it -- or it was
    // cleared -- and no more than four times a second. It used to be copied
    // whole on every frame, up to 2000 lines under the mutex every logging
    // thread waits on, and every one of them drawn whether in view or not.
    // The count is read before the copy: a line landing between the two is
    // in the copy, and only makes the next frame copy once more.
    Logger& logs = Logger::instance();
    const std::uint64_t seq = logs.ring_sequence();
    const std::uint64_t now = now_ms();
    if (!log_copied_ || (seq != log_copied_seq_ && now - log_copied_ms_ >= kLogCopyGapMs)) {
        log_copied_ = true;
        log_copied_seq_ = seq;
        log_copied_ms_ = now;
        log_copy_ = logs.ring();
        log_rows_level_ = -1;
    }
    // One row for each line of each kept line that passes the filter: rows of
    // one height, so the clipper can skip straight to the ones in view.
    if (log_rows_level_ != log_level_filter_) {
        log_rows_level_ = log_level_filter_;
        log_rows_.clear();
        for (std::size_t i = 0; i < log_copy_.size(); ++i) {
            if (static_cast<int>(log_copy_[i].level) < log_level_filter_) continue;
            const std::string& text = log_copy_[i].text;
            std::size_t begin = 0;
            for (;;) {
                const std::size_t nl = text.find('\n', begin);
                const std::size_t end = nl == std::string::npos ? text.size() : nl;
                log_rows_.push_back({ i, begin, end });
                if (nl == std::string::npos) break;
                begin = nl + 1;
            }
        }
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(log_rows_.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const LogRow& row = log_rows_[static_cast<std::size_t>(r)];
            const LogLine& l = log_copy_[row.line];
            ImVec4 col = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
            if (l.level == LogLevel::Warn)  col = kAmber;
            if (l.level == LogLevel::Error) col = kRed;
            if (l.level == LogLevel::Debug || l.level == LogLevel::Trace) col = kDim;
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(l.text.data() + row.begin, l.text.data() + row.end);
            ImGui::PopStyleColor();
        }
    }
    clipper.End();

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
