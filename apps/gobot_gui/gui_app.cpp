#include "gui_app.hpp"
#include "gobot/alerts.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/jump_check.hpp"
#include "gobot/reviver.hpp"
#include "gobot/screen_layout.hpp"
#include "gobot/session_manager.hpp"
#include "gobot/stall_watch.hpp"     // login_page_up, for a chat line in doubt (B4)

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

void copy_to(char* dst, std::size_t cap, const std::string& src) {
    std::snprintf(dst, cap, "%s", src.c_str());
}

/// The hold a chat line is typed under, and kept under while the chat line
/// may be open, by name.
const char* const kChatHold = "auto chat";

/// The hold everything is stopped under (stop_jobs), by name.
const char* const kStopHold = "stopping";

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
/// reading as Tiêu Dùng (BagCleaner::Result::tab_unsure): a minute since
/// 2026.11.24 -- the charm job's own gap after a run that stopped on trouble
/// -- not the ten minutes a charm not found waits (CharmClock::kRetryMinutes,
/// kept: a charm not in the bag cannot come sooner). The owner: "Kiểm tra các
/// phần khác xem có phần nào không làm được thì dừng không, cần thay cơ chế
/// này bằng 1 cơ chế khác" -- no long pause in place of a stop. Such a run
/// stops at the tab after a couple of seconds' hold, the bot held off for no
/// longer, and whatever spoiled the read (a window over the strip, the pointer)
/// may well be gone a minute on. 2026.11.08 to 2026.11.24: ten minutes.
constexpr std::uint64_t kCharmTabRetryMs = 60'000;
/// How soon the next charm run comes after one that clicked GoBot's own
/// message and left it up (BagCleaner::Result::notice_clicked, 2026.11.24): it
/// clicks it again then -- twice at most, at the place remembered, never a key
/// -- and so on without limit, each such run renewing the two minutes the
/// message is GoBot's. Not every five seconds: clicks that did not close it
/// are not helped by more of them at once. Until 2026.11.24 a message two
/// clicks did not close was left to the player, and no charm was used under it.
constexpr std::uint64_t kCharmNoticeAgainMs = 60'000;
/// The first charm check after a trash worker starts -- a BẮT ĐẦU -- or after
/// "Dùng bùa" is ticked on again; a clock the last worker left later than that
/// is kept (GuiApp::TrashClocks). And since 2026.11.12 (the owner: "khi bật thì
/// bắt đầu đếm thời gian") the least the timed pass and the shop's look wait
/// after a start whose last worker left a clock still counting -- a quick
/// MACRO OFF and ON (bag::start_due_ms): with none, each counts a whole
/// interval from the start. The pass's was every start's first wait before
/// 2026.11.12, the shop's its least.
constexpr std::uint64_t kCharmFirstMs = 8'000;
constexpr std::uint64_t kPassFirstMs = 15'000;
constexpr std::uint64_t kShopFirstMs = 45'000;

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
/// keeper that runs for hours needs seven. Copied out of the program's one
/// set (2026-11-16) -- the same pictures AutoLogin::load_pictures gives with
/// no folder named -- so the keeper's start reads no PNG another job has
/// read already.
TemplateStore keeper_pictures(const AutoLogin::Settings& for_these) {
    return AutoLogin::pictures_for(for_these, *TemplateStore::shared_best(exe_relative("configs/templates.ini")));
}

/// The PC's clock `ms` from now, hh:mm:ss: when a wait of the keeper's for
/// the hour's restarts ends (2026.11.24), as the player reads the time.
std::string clock_after(std::uint64_t ms) {
    const std::time_t t = std::time(nullptr) + static_cast<std::time_t>((ms + 999) / 1000);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    return buf;
}

/// The keeper's line on the simple screen for a crossing not in, made again
/// CrashWatch::kCrossAgainMs on (2026.11.24): what GoBot does now, with the
/// fixed reason of the alert (alert_text::crossing_reason), never the flow's
/// own detail -- that is in the red line under it.
std::string not_in_vi(bool restart, const char* reason) {
    return std::string(restart ? "Vào lại game chưa được" : "Vào game chưa được") +
           (reason != nullptr && reason[0] != '\0' ? std::string(" (") + reason + ")" : std::string()) +
           " — GoBot thử lại sau " + std::to_string(CrashWatch::kCrossAgainMs / 1000) + " giây.";
}

/// How often a worker short of the game's pictures looks for them again
/// (2026.11.24, the owner: "Kiểm tra các phần khác xem có phần nào không làm
/// được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác"). Until then
/// Tổ đội, Tự hồi sinh, Dọn rác and Tự chat ended at their start with a line
/// on their tab, and stayed ended until the next BẮT ĐẦU -- a picture missing
/// for a moment (an install being repaired, a folder being copied in) cost
/// the whole night's feature.
constexpr int kPicturesAgainMs = 30'000;

/// What such a worker says, once for each thing found missing.
std::string pictures_again_vi(const std::string& what) {
    return "Thiếu hình giao diện của game (" + what + ") — GoBot thử lại sau " +
           std::to_string(kPicturesAgainMs / 1000) + " giây.";
}

/// The wait itself, before the worker's loop (2026.11.24): `missing` names
/// what the set held lacks for this worker, empty when nothing; while it
/// names something the worker presses nothing, says so once
/// (say_waiting, with the words for pictures_again_vi), and every
/// kPicturesAgainMs asks for the program's one set again
/// (TemplateStore::shared_best: a load that failed is not kept, and a
/// pictures folder that changed is loaded afresh -- a fixed install is
/// picked up). Its stop flag ends the wait at once: false then, and the
/// worker leaves as on any stop. True as soon as the pictures are there.
bool wait_for_pictures(std::shared_ptr<const TemplateStore>& pictures, const std::string& name_map_ini,
                       const char* who_en, const std::function<std::string(const TemplateStore&)>& missing,
                       const std::function<bool()>& stopped,
                       const std::function<void(const std::string&)>& say_waiting) {
    std::string said;
    for (;;) {
        if (stopped()) return false;
        const std::string what = pictures ? missing(*pictures) : std::string("hình giao diện");
        if (what.empty()) {
            if (!said.empty()) GB_INFO("%s: the pictures it needs are there now -- going on", who_en);
            return true;
        }
        if (what != said) {
            GB_WARN("%s: a picture it needs is missing (%s) -- nothing pressed, looked for again every %d s",
                    who_en, what.c_str(), kPicturesAgainMs / 1000);
            say_waiting(what);
            said = what;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPicturesAgainMs);
        while (std::chrono::steady_clock::now() < until) {
            if (stopped()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (stopped()) return false;
        pictures = TemplateStore::shared_best(name_map_ini);
    }
}

/// Why the hunter holds the skills, as the macro counts it (2026.11.12).
MacroEngine::Fight::Hold fight_hold(Hunter::SkillHold h) {
    switch (h) {
        case Hunter::SkillHold::Drop:     return MacroEngine::Fight::Hold::Drop;
        case Hunter::SkillHold::Turning:  return MacroEngine::Fight::Hold::Turning;
        case Hunter::SkillHold::NoTarget: return MacroEngine::Fight::Hold::NoTarget;
        case Hunter::SkillHold::Outside:  return MacroEngine::Fight::Hold::Outside;
        case Hunter::SkillHold::SeekLeg:  return MacroEngine::Fight::Hold::SeekLeg;
        case Hunter::SkillHold::Jump:     return MacroEngine::Fight::Hold::Jump;
        case Hunter::SkillHold::None:     break;
    }
    return MacroEngine::Fight::Hold::None;
}

/// A key of the macro's switched on (simple_view.cpp's any_key_on): a refused
/// start of the macro is owed only then (2026.11.24 review).
bool macro_keys_on(const MacroConfig& cfg) {
    return std::any_of(cfg.keys.begin(), cfg.keys.end(),
                       [](const MacroKey& k) { return k.enabled && k.keycode != 0; });
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
    stop_jobs();
    // Tự chat waited for this time: a line half typed is left in the chat line
    // as it stops, and its thread has to be gone before what it reads is.
    stop_chat();
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

    const std::string proto = exe_relative(app_config_.protocol_path);
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
    // Handed to the capture at once: the one for positions is started from
    // the first tick on, before any hunt, and a capture with the default
    // framing would read the game's stream as garbage until HUNT ON.
    push_framings();

    capture_.set_packet_callback([this](const std::vector<GamePacket>& p) { on_packets(p); });
    // On the watchdog's thread, which has said "No game traffic" already:
    // tick() says what it means -- the game gone quiet under the capture that
    // runs all session.
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

    // Following the client's size unless edited by hand in [Detect] of
    // hunt.ini.
    detect_tuning_ = DetectorTuning::defaults();
    {
        IniFile hunt_ini;
        if (hunt_ini.load(exe_relative("configs/hunt.ini"))) detect_tuning_.load(hunt_ini);
    }
    detector_.set_tuning(detect_tuning_);
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
            // none runs -- not while the Capture tab's does.
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
    // Its swing left to the macro when the attack key is a skill the macro
    // casts in turn and it is pacing them (2026.11.16b): an atomic of the
    // macro's, read on the hunter's thread.
    hunter_->set_swing_yield([this](std::uint32_t vk) { return macro_ && macro_->swing_yields(vk); });
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
    // And one per move of yours with no walk (2026.11.12): the key its skill
    // is cast with, by your casts paired with GoBot's presses, and GoBot's
    // own keys about it -- what learns the dash (the owner's Q1).
    packet_world_.set_move_keys([this](const PacketWorld::OwnMove& m) { return more_move_keys(m); });
    // Skills that follow the fight ask the hunter, from the macro's own
    // threads, whether it is fighting: only its atomics are read there. No
    // hunter running is no hunter, and every key keeps its timer. Walking
    // back to the hunting area (2026.11.10), every skill waits; so it does
    // while the hunter holds them (2026.11.12, the owner's Q2): a drop
    // settling or falling, a turn not yet confirmed, nothing to fight in the
    // area, outside it or stranded -- and the dash's key alone where its
    // landing would leave the area or a floor.
    macro_->set_fight([this] {
        MacroEngine::Fight f;
        if (!hunter_ || !hunter_->running()) return f;
        f.mode = hunter_->skills_held() ? MacroEngine::Fight::Mode::Held
                 : hunter_->engaged()   ? MacroEngine::Fight::Mode::Fighting
                 : hunter_->returning() ? MacroEngine::Fight::Mode::Returning
                                        : MacroEngine::Fight::Mode::Idle;
        f.hold = fight_hold(hunter_->skill_hold());
        f.dx = hunter_->engaged_dx();
        f.attack_key = hunter_->attack_key();
        f.dash_edge = hunter_->dash_edge();
        f.dash_keys = hunter_->dash_keys();
        // The target past the attack key's reach (F3): following the fight,
        // the rotation passes that key's turn, as the hunter holds its swing.
        f.attack_far = hunter_->attack_far();
        // Its last swing's key-down (more_note_attack), for the rotation to
        // tell that swing's cast from its own key's (2026.11.16b) -- as the
        // hunter stores it before the press (2026.11.17): more_note_attack
        // hears of it only once the press is over, after its cast may have
        // been taken.
        f.swing_ms = std::max(swing_ms_.load(), hunter_->last_swing_ms());
        return f;
    });
    // Every key the macro pressed, with its time, for what the packets say
    // came of it: a potion's bag frame, a skill's cast (PacketSelf's presses).
    macro_->set_on_press([this](std::uint32_t vk, std::uint64_t at_ms) { packet_self_.note_press(vk, at_ms); });
    load_simple_options();
    alerts_init();
    load_hotkeys();
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
    // walk onto a gate with ↓ down changes the map. Since 2026.11.24 no stop
    // (the owner: "thử lại luôn" -- every can-not-do-it goes on): every key is
    // let go of, once an episode, said in amber under the tick, and the hunt
    // goes on by itself once Windows takes the key-ups -- the hunter holds
    // still meanwhile (Hunter::drop_watch). Until then the hunt stopped here.
    if (input_.keys_owed() && now_ms() - owed_look_ms_ >= 100) {
        owed_look_ms_ = now_ms();
        if (!input_.settle_owed() && input_.owed_tries() >= InputManager::kOwedTries && hunting_ &&
            !owed_released_) {
            owed_released_ = true;
            input_.release_all();
            input_.release_stray_drop_keys();
            // A high jump's ↑ owed is named as ↑ (2026.11.05b).
            const bool up = input_.up_owed();
            drop_note_vi_ = up ? "Phím ↑ hoặc Ctrl có thể đang bị giữ: GoBot đã nhả mọi phím; đánh quái tự chạy tiếp "
                                 "khi Windows nhận lệnh nhả phím."
                               : "Phím ↓ hoặc Ctrl có thể đang bị giữ: GoBot đã nhả mọi phím; đánh quái tự chạy tiếp "
                                 "khi Windows nhận lệnh nhả phím.";
            simple_note(drop_note_vi_, 15000);
            if (!simple_view_) set_status(drop_note_vi_, 15000);
            GB_WARN("Hunter: keys owed after a %s (%s or Ctrl up not sent, %d tries) -- every key let go of; the hunt "
                    "goes on",
                    up ? "jump up" : "drop", up ? "Up" : "Down", input_.owed_tries());
        }
    }
    // The episode over: the next one lets every key go again.
    if (!input_.keys_owed()) owed_released_ = false;
    // The hunter's drop: what GoBot says against one, and a press that
    // changed the map -- said once, the hunt going on.
    drop_tick();

    // How Windows shows the game, every few seconds: the display scale can be
    // changed under a running GoBot, and a game started again is shown afresh.
    if (now_ms() - game_scaling_looked_ms_ > 3000) check_game_scaling();

    // The game moved into its screen's corner after a login (2026.11.14):
    // GoBot's own window moved beside it when it now covers the game.
    if (clear_after_login_.exchange(false) && overlap_fraction() > 0.02f) {
        move_clear_of_game();
        GB_INFO("Keeper: the game was moved into its screen's corner after the login, under GoBot's window -- "
                "GoBot's window moved beside it (%s)",
                overlap_fraction() < 0.02f ? "clear now" : "not enough room beside the game");
    }

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
        stop_jobs();
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
    // the packets rests the reader at a second at most (vitals_pace,
    // packets_vitals.cpp).
    vitals_.set_period_ms(vitals_pace());
    // The full screen's FIND THE BARS, once a frame read after its click is in.
    full_find_bars_tick();

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
    // the popup closer back; with no trash worker to look, that ends once the
    // game shows nothing waiting.
    watch_left_held();

    // The pet's HP food on reaching TổCổLong (2026.11.24): an arrival owes a
    // round, and a round owed with no trash worker starts one.
    pet_arrival_tick();

    // The revive ran out of tries at a death. Until 2026.11.24 the player's
    // rule stopped the whole bot here (stop_after_revive); since, the owner
    // ("Kiểm tra các phần khác xem có phần nào không làm được thì dừng không,
    // cần thay cơ chế này bằng 1 cơ chế khác"), nothing is stopped: the worker
    // keeps its hold while the character lies dead and asks for another round
    // every Reviver::kRoundGapMs, never the village or Cash, and this only
    // says it -- once a death -- on the screen and to the phone.
    if (revive_gave_up_.exchange(false)) revive_out_of_tries();

    // Tự chat was stopped by the player with the chat line maybe open, and has
    // handed it over: its hold is still on, so nothing has typed into the line
    // since. Whatever presses keys stops -- DỪNG and F12 have stopped most of
    // it already -- and the hold goes; nothing of that starts again until a
    // look shows the line shut (keys_may_start).
    if (chat_handover_.exchange(false)) {
        // Unless a look has shown it shut since (keys_may_start, for a BẮT
        // ĐẦU pressed in between): then what that started is left running.
        if (chat_doubt_) {
            // What ran is owed (the 2026.11.24 review), as the bin's doubt
            // resumes what it stopped: with the run still on, it starts again
            // once a look shows the line shut (watch_chat_doubt -> owed_now;
            // owed_tick waits on the doubt meanwhile). Not under DỪNG's stop.
            const bool was_hunting = hunting_, was_macro = macro_running_;
            stop_hunt();
            stop_macro();
            bool stopping = false;
            {
                std::lock_guard<std::mutex> lock(hold_mutex_);
                stopping = stopping_;
            }
            if (!stopping) {
                if (was_hunting && hunt_config_.enabled) owe_start(hunt_owed_, "Hunting");
                if (was_macro) owe_start(macro_owed_, "The macro");
            }
            const bool owed = hunt_owed_ || macro_owed_;
            const std::string note = owed ? "Ô chat trong game có thể vẫn đang mở, nên GoBot dừng cả đánh "
                                            "quái, bình và buff để không gõ phím vào đó. Đóng ô chat trong game "
                                            "(xoá chữ trong đó rồi bấm Enter): GoBot tự bật lại khi thấy ô chat "
                                            "đã đóng."
                                          : "Ô chat trong game có thể vẫn đang mở, nên GoBot dừng cả đánh "
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
    // game shows the line shut. What was stopped above waits for BẮT ĐẦU.
    watch_chat_doubt();
    // A worker that has finished is joined here, rather than at the next start.
    if (!chat_running_) join_pumping(chat_thread_);

    // A start the run asked for and that was refused by something that
    // passes -- Tự đánh quái with Npcap or its service up late, a doubt seen
    // over since, the cold start's jobs -- asked again every 30 s while the run
    // is on, and in this very tick when a watcher above saw its doubt over
    // (2026.11.24, the owner: "... không làm được thì dừng không, cần thay cơ
    // chế này bằng 1 cơ chế khác"). Until then it waited for the next BẮT ĐẦU.
    // Never after the player's own stop: DỪNG, F12, F11, the Attack switch and
    // the tick taken off forget it.
    owed_tick();

    // The game's window not found after the keeper got in (reattach_game):
    // looked for again every kReattachAgainMs while the keeper runs, and the
    // look that finds it does all that follows a log in below -- until
    // 2026.11.24 a relogin's window missed at the first look was looked for no
    // more, and the reader, the macro and the hunter stayed on the old one.
    // Not mid-crossing (the 2026.11.24 review): a window found at the channel
    // list or the character select would get the after-login follow-ups --
    // a cold start's jobs too -- before the character is in the world. The
    // keeper's own In sets keeper_reattach_ after the crossing anyway.
    if (reattach_missed_ && keeper_running_ && !keeper_stop_ && now_ms() - reattach_missed_ms_ >= kReattachAgainMs &&
        !keeper_crossing())
        keeper_reattach_ = true;
    if (keeper_reattach_.exchange(false)) {
        // A look again for a window missed before -- this keeper's last log
        // in, or a cold start's game found up -- rather than a log in of its
        // own: the hunting spot is set aside once, at the log in, and again
        // only on the look that finds the window.
        const bool again = reattach_missed_;
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
        if (hunter_ && hunter_->running() && (found || !again)) hunter_->suspend_spot();
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

    // Your HP and MP from the packets (packets_vitals.cpp): whether each goes
    // by them now, said as that changes, and every value that came and every
    // read of the bars set side by side for the log -- before anything below
    // goes by them.
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

    // Your HP and MP from the packets go to the macro, bar by bar, whenever
    // the packets have that bar -- its value of the packets' current epoch,
    // on the capture running now, the server heard within 5 s, a maximum
    // known -- asked afresh now (gate_vitals, packets_vitals.cpp), whatever
    // the bars read (2026.11.17). Otherwise the bars read off the screen, as
    // before; the pet's HP always.
    SelfVitals packets = gate_vitals(now_ms());
    // A bar gone back to the screen while the reader rests -- a gap, a
    // restart, the server quiet -- has
    // the run's pace now, not at the next tick: the cut stamps paced_from_ms
    // before the read below is judged, so a read up to a second old goes to
    // the macro as no reading, until the read the cut wakes.
    if ((packets.hp < 0.0f || packets.mp < 0.0f) && vitals_.period_ms() > vitals_run_period())
        vitals_.set_period_ms(vitals_pace());
    const bool from_packets = packets.hp >= 0.0f || packets.mp >= 0.0f;

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

    // The game's traffic gone quiet, as the capture's watchdog asked: said
    // here, on GoBot's own thread.
    if (traffic_lost_.exchange(false)) say_traffic_lost();

    // Last: the Load line, once every [Performance] load_log_s seconds.
    load_tick();
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
    return simple_running() || macro_running_ || (macro_ && macro_->running()) || hunting_ || party_running_ ||
           revive_running_ || popups_running() || keeper_running_ || trash_running_ || chat_running_ ||
           quest_running_ || simple_find_bars_ || full_find_bars_ || coord_.critical_active() || jump_test_running() ||
           jump_kind_running();
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
    // How late the capture read the game's packets (2026.11.12, the owner's
    // "delay khoảng 0.5 -1s" on the Bản đồ tab): where a lag the tab shows
    // comes from, the next log says.
    if (load_last_.at_ms != 0 && capture_.running()) {
        const PacketCapture::ReadDelay d = capture_.take_read_delay();
        if (d.packets > 0)
            GB_INFO("Capture delay: last %d s | %llu packets read %.1f ms on average, %.1f ms at most, after the "
                    "quickest -- Npcap's stamp to GoBot's read",
                    every_s, static_cast<unsigned long long>(d.packets), d.avg_ms, d.max_ms);
    }
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
    add(party_running_, "party");
    add(revive_running_, "revive");
    add(popups_running(), "popups");
    add(keeper_running_, keeper_oneshot_ ? "keeper(once)" : "keeper");
    add(trash_running_, trash_oneshot_ ? "trash(once)" : "trash");
    add(chat_running_, chat_oneshot_ ? "chat(once)" : "chat");
    add(quest_running_, quest_oneshot_ ? "quest(once)" : "quest");
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
                          (popups_running() ? 256u : 0u);
    const bool changed = jobs != redraw_jobs_;
    redraw_jobs_ = jobs;
    const bool asked = redraw_.exchange(false);
    return asked || changed;
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
    GB_INFO("Following game window %p: keys, the HP/MP reader and the monster detector", hwnd);
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

    // Whether all of the game is on the screen, nothing of the taskbar over
    // it (2026.11.13c): its frames are read off the screen, and the rows past
    // its edge or under the taskbar -- where the hotbar and the chat are --
    // read black or as the taskbar. Windows' answers only, no file.
    AutoLogin::Fit fit = input_.window_valid() ? AutoLogin::game_fit(game) : AutoLogin::Fit{};
    // Minimised, or no client to measure: as it was, and nothing said -- the
    // look once it is shown again says what has changed. A window gone is
    // nothing hidden.
    if (input_.window_valid() && (fit.width() <= 0 || fit.height() <= 0)) fit = game_fit_;
    if (!fit.same(game_fit_) && (fit.hides() || game_fit_.hides())) {
        if (fit.hides())
            GB_WARN("The game's bottom %d and top %d rows, right %d and left %d columns are hidden; the first of them "
                    "%s (client %dx%d at %ld,%ld; screen %dx%d)",
                    fit.hidden_bottom, fit.hidden_top, fit.hidden_right, fit.hidden_left,
                    fit.taskbar && fit.off_screen ? "under the taskbar and off the screen"
                    : fit.taskbar                 ? "under the taskbar"
                                                  : "off the screen",
                    fit.width(), fit.height(), fit.client.left, fit.client.top, fit.screen_w, fit.screen_h);
        else if (fit.width() > 0)
            GB_INFO("The game is all on the screen again (client %dx%d)", fit.width(), fit.height());
    }
    game_fit_ = fit;
    std::lock_guard<std::mutex> lock(game_fit_mutex_);
    game_fit_shared_ = fit;
}

std::string GuiApp::game_unseen(int rows, const std::string& what, bool from_screen) const {
    // Drawn by the game itself (PrintWindow): rows past the screen's edge or
    // under the taskbar are in the frame all the same.
    if (!from_screen) return {};
    AutoLogin::Fit fit;
    {
        std::lock_guard<std::mutex> lock(game_fit_mutex_);
        fit = game_fit_shared_;
    }
    if (fit.hidden_bottom <= rows) return {};
    return AutoLogin::fit_reason(fit, what) + " Xem dòng \"Game không nằm trọn trên màn hình\" ở thẻ Đánh quái.";
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

    // Every opcode let through, as for the capture for positions: the packet
    // world reads what it needs of them.
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
    // Whoever started it -- the one for positions included -- a capture
    // stopped here stays stopped: the hunter goes back to the screen at its
    // next pass, and the capture for positions is not started again until
    // the next hunt, or the Capture tab's Start.
    position_capture_ = false;
    position_capture_off_ = true;
    set_status("Capture stopped");
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
    trip_packets_.on_packets(all);
    frame_tally_.add(all);
    more_on_packets(all);
}

void GuiApp::push_framings() {
    FramingSpec f = framing_;
    // Each wrapper unpacked, so that its one frame comes out whole for the
    // packet world; a wrapper that does not unpack -- the client's are
    // ciphered -- is left shut and counted.
    f.unpack = f.unwraps();
    capture_.set_framing(f);
    if (has_client_framing_) capture_.set_client_framing(client_framing_);
}

void GuiApp::reload_configs() {
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
        if (ImGui::BeginTabItem("Features")) { draw_feature_list(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log"))      { draw_log_panel();    ImGui::EndTabItem(); }

        // The HP/MP reader's own panel and the capture by hand, behind one
        // tab: neither is needed to run the bot.
        if (advanced_)
            if (ImGui::BeginTabItem("Tools")) { draw_tools_panel(); ImGui::EndTabItem(); }

        ImGui::EndTabBar();
    }

    draw_status_bar();
    ImGui::End();
}

// =========================================================================
//                            THE FEATURE LIST
// =========================================================================

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
    // Attack is the name-plate hunter, the one engine that fights: by the
    // name plates, or by the game's packets while they are healthy.
    bool attacking = hunting_;
    draw_feature("Attack", &attacking,
                 hunt_config_.packet_positions == PacketPositions::On
                     ? "picks a target and fights it -- by the game's packets while healthy, by name plates otherwise"
                     : "picks a target and fights it -- by name plates",
                 [this] { draw_attack_panel(); });
    // The player's own switch: nothing owed is asked for the hunt after its
    // stop (2026.11.24), as after F11; a start it asks for and that is refused
    // is owed (the 2026.11.24 review).
    if (attacking != hunting_) {
        if (attacking) {
            start_hunt();
            if (!hunting_ && hunt_config_.enabled) owe_start(hunt_owed_, "Hunting");
        } else {
            hunt_owed_ = false;
            stop_hunt();
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

    // --- keys ---
    bool macro_on = macro_running_;
    draw_feature("Timed keys", &macro_on,
                 "presses keys on their own schedules",
                 [this] { draw_macro_panel(); });
    // Likewise for the macro (2026.11.24; a refused start owed, the review).
    if (macro_on != macro_running_) {
        if (macro_on) {
            start_macro();
            if (!macro_running_ && macro_keys_on(macro_config_)) owe_start(macro_owed_, "The macro");
        } else {
            macro_owed_ = false;
            stop_macro();
        }
    }

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

    // What is doing the picture matching, said plainly. It is linked into this
    // executable rather than shipped as a DLL beside it, so there is nothing in
    // the build folder to tell you by.
    ImGui::Dummy(ImVec2(0, 10));
    text_dim("matching: %s", TemplateMatcher::backend().c_str());
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
    // An hour's cap, waited out, since 2026.11.24 ("Gives up after %d
    // restart(s)" of a run until then).
    ImGui::Text("Restarts   at most %d an hour", s.max_restarts);
    text_dim("A restart is every time the game had to be closed and entered again: found gone, hung or "
             "stalled, or ended by a crossing stuck part way in -- the login key's included. "
             "One more in the same hour waits, nothing ended or clicked, until the oldest is an hour old; "
             "a crossing that does not get in is made again 30 s on. Nothing but GoBot not running as "
             "administrator stops the keeper. A client that falls over that often is not failing at random, "
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

// =========================================================================
//                        THE SHAPE THE ORIGINAL USES
// =========================================================================

/// Run, stop, and what the bot is attached to.
void GuiApp::draw_run_bar() {
    const bool running = macro_running_;

    ImGui::PushStyleColor(ImGuiCol_Button, running ? kRed : kGreen);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    if (ImGui::Button(running ? "STOP" : "START", ImVec2(110, 32))) {
        if (running) {
            macro_owed_ = false;    // the player's own stop of the macro (2026.11.24)
            stop_macro();
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
            // Refused: owed, as any (the 2026.11.24 review).
            if (!macro_running_ && macro_keys_on(macro_config_)) owe_start(macro_owed_, "The macro");
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Starts the keyboard macro and the HP/MP reader.");

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

        if (macro_ && macro_->suspended()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextUnformatted("HOLDING -- not in the world");
            ImGui::PopStyleColor();
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

/// What it hits, and how it stands to hit it: the hunter's settings.
void GuiApp::draw_attack_panel() {
    ImGui::TextWrapped(
        "Fighting by name plates: monsters are found on screen by their red "
        "health bars, and your character by its name -- or, with Positions "
        "from packets On and the packets healthy, you and the monsters from "
        "the game's own traffic.");
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
}

/// The HP/MP reader's own panel, and the packet capture by hand.
void GuiApp::draw_tools_panel() {
    ImGui::TextWrapped("Neither is needed to run the bot: the simple screen finds the bars, and the capture "
                       "for positions runs by itself.");
    ImGui::Dummy(ImVec2(0, 8));

    if (ImGui::BeginTabBar("##tools")) {
        if (ImGui::BeginTabItem("HP / MP")) { draw_vitals_panel();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Capture")) { draw_capture_panel(); ImGui::EndTabItem(); }
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
            ImGui::SetTooltip("The HP/MP reader's own panel and the packet "
                              "capture by hand. Neither is needed to run the bot.");
        ImGui::EndMenu();
    }

    if (advanced_) {
        if (ImGui::BeginMenu("Capture")) {
            if (ImGui::MenuItem("Start", nullptr, false, !capture_running_)) start_capture();
            if (ImGui::MenuItem("Stop",  nullptr, false,  capture_running_)) stop_capture();
            if (ImGui::MenuItem("Refresh interfaces")) refresh_interfaces();
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
    }

    ImGui::EndMenuBar();
}

// ----------------------------------------------------------------- macro

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
    macro_owed_ = false;    // owed no longer, by whatever asked (2026.11.24)
}

void GuiApp::stop_macro() {
    if (!macro_running_) return;
    macro_->stop();
    macro_running_ = false;
}

void GuiApp::toggle_macro() {
    // The full screen's F10 and START MACRO: the player's own hand on the
    // macro -- nothing owed is asked for it after a stop (2026.11.24); a
    // start it asks for that is refused is owed (the 2026.11.24 review).
    if (macro_running_) {
        macro_owed_ = false;
        stop_macro();
        return;
    }
    start_macro();
    if (!macro_running_ && macro_keys_on(macro_config_)) owe_start(macro_owed_, "The macro");
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
    // Its looks can wait for the game's slice to end on a one-core PC.
    quiet_this_thread();
    const std::string config = exe_relative("configs/party.ini");

    // Never on the code's own defaults. A file that cannot be read says
    // nothing about what the player ticked, and a worker that ran anyway
    // answered and sent invitations by rules nobody chose. Tried a few times
    // first: the tab writes the file as a box is ticked, and something may
    // hold it for a moment just then -- a virus scanner, say.
    // The file's time is taken before it is read, so a write that lands
    // during the read is read again by the loop below.
    FileWatch rules_watch(2000);
    rules_watch.due(config);
    Party::Settings rules;
    bool read = rules.load(config);
    for (int i = 0; !read && i < 5 && !party_stop_; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        read = rules.load(config);
    }
    // Still not read: the worker waits and reads it again every 2 s -- the
    // FileWatch's own cadence in the loop below -- acting on nothing
    // meanwhile, and goes on as soon as it reads (2026.11.24, the owner: "Kiểm
    // tra các phần khác xem có phần nào không làm được thì dừng không, cần thay
    // cơ chế này bằng 1 cơ chế khác"). Until then Tổ đội ended here until the
    // next BẮT ĐẦU. Never on the code's defaults, as before.
    if (!read) {
        GB_WARN("Party: cannot read %s -- waiting, nothing answered or sent, read again every 2 s",
                config.c_str());
        say_party("Chưa đọc được configs/party.ini — Tổ đội chờ, chưa làm gì; đọc lại mỗi 2 giây.");
        while (!read) {
            for (int slept = 0; slept < 2000 && !party_stop_; slept += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (party_stop_) {
                party_running_ = false;
                return;
            }
            // Stamped afresh before the read, as above: a write landing
            // during it is read again by the loop below.
            rules_watch.forget();
            rules_watch.due(config);
            read = rules.load(config);
        }
        GB_INFO("Party: %s read now -- going on", config.c_str());
        say_party("Đã đọc được configs/party.ini — Tổ đội chạy.");
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

    // The program's one set of pictures, held while this runs (2026-11-16).
    // None loaded: no return any more (2026.11.24) -- the worker waits,
    // answering and sending nothing, and looks for them again every 30 s
    // (wait_for_pictures); its stop ends the wait at once.
    std::shared_ptr<const TemplateStore> pictures =
        TemplateStore::shared_best(exe_relative("configs/templates.ini"));
    if (!wait_for_pictures(
            pictures, exe_relative("configs/templates.ini"), "Party",
            [](const TemplateStore& s) { return s.size() == 0 ? std::string("hình giao diện") : std::string(); },
            [this] { return party_stop_.load(); },
            [this](const std::string& what) { say_party(pictures_again_vi(what)); })) {
        party_running_ = false;
        return;
    }
    const TemplateStore& store = *pictures;

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
    const auto nap_unless = [](int ms, const std::function<bool()>& stop) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (stop()) return false;
            const auto left = until - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) return true;
            std::this_thread::sleep_for(
                std::min<std::chrono::steady_clock::duration>(left, std::chrono::milliseconds(50)));
        }
    };
    const auto nap = [&](int ms) { return nap_unless(ms, stopped); };

    // The character down -- HP read under bag::kJobDownHpPct, as the bag's
    // jobs, the quest and the popups have it, or the death panel seen by the
    // reviver, which is running and has not yet brought the character back
    // (death_seen_ms_; a minute at most, should the player revive by hand).
    // An invite, and the game's message after it, are done with the game's
    // mouse held: since 2026-11-15 each of their waits for the game can run
    // kReactMs (the list, the row's menu, Req Party landing, a message's
    // Xác Nhận), and the reviver waits for that mouse before its first click
    // on the death panel -- thirty seconds a wait, several in a row, with the
    // character dead. So those waits, and the clicks in them, end on it as on
    // a stop (react_wait.hpp: every look asks the character down too), and the
    // mouse goes back. The worker's own pauses between looks, which hold
    // nothing, keep the plain stop: ended at once by this, they would spin.
    const auto down = [this] {
        const float hp = hp_percent_now();
        if (hp >= 0.0f && hp < static_cast<float>(bag::kJobDownHpPct)) return true;
        const std::uint64_t death = death_seen_ms_.load();
        const std::uint64_t now = now_ms();
        return revive_running_.load() && death != 0 && now >= death && now - death < 60'000;
    };
    const auto mouse_stopped = [&] { return stopped() || down(); };
    const auto mouse_nap = [&](int ms) { return nap_unless(ms, mouse_stopped); };

    // The game's answer to an invite when it is a message box of its own --
    // "already in a party", the invite refused (2026.11.14b, the owner: "nếu
    // chẳng may pt trúng người đã vào pt thì sẽ bị hiện dialog mà không tự động
    // tắt"): closed by its own Xác Nhận with these hands -- the stop asked as
    // the button goes down, nothing clicked while another job holds the game
    // (Tự chat typing, say), and never a key: Enter opens the chat line. Its
    // waits hold the game's mouse, so they end on the character down as well
    // (mouse_stopped, above).
    InputManager::GameClick reply_how = InputManager::GameClick::Clicked;
    InviteReply::Hands reply_hands;
    reply_hands.grab = [&](Frame& f) { return capture.grab(input_.target_window(), f) && f.valid(); };
    reply_hands.click = [&](int x, int y) {
        if (coord_.critical_active()) return false;
        return input_.click_on_game(x, y, false, mouse_stopped, &reply_how);
    };
    reply_hands.move = [&](int x, int y) { input_.move_pointer(x, y); };
    reply_hands.nap = mouse_nap;
    reply_hands.stop = mouse_stopped;
    const GlyphFont* reply_font = have_font ? &font : nullptr;
    // On the clock the chat's votes go by (PartyChat::saw).
    const auto steady_ms = [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    };
    // The watch's time by the clock as well as by its naps (the review of
    // 2026.11.14b): its slow looks on the 1-CPU guest do not hold the game's
    // mouse, which a pass or a shop round may be waiting for, much past it.
    reply_hands.now_ms = steady_ms;
    // The last player invited, for a message that comes later (declined, run
    // out); and a message that would not close, which stops the invites.
    std::string last_invited;
    std::uint64_t last_invite_ms = 0, late_looked_ms = 0, reply_stuck_until = 0;
    bool reply_stuck_said = false;
    // A message of the game's that the worker's click did not close (2026.11.17b,
    // B): clicked again on a later pass, kRetryGapMs on at least, with the
    // game's mouse let go in between -- InviteReply::kCloseTries clicks in all,
    // then no invite for kStuckMs. Until then each watch held the mouse up to
    // kReactMs (30 s) after its first click, and every other job waited.
    int reply_clicks = 0;               // clicks so far on a message still up; 0: none known
    std::uint64_t reply_retry_ms = 0;   // not clicked again before this
    std::string reply_who;              // whose invite it answered, for the log
    // Who was invited and whether it led to a join, and who is held off for
    // an invite that did not (2026.11.17b, A): on now_ms()'s clock.
    InviteTries tries;
    // The player list as the worker left it (2026.11.17b, C): one it opened
    // and could not shut is shut on the next invite's turn too, and said
    // once until a close works.
    bool list_left_open = false, list_close_said = false;
    // "1 phút", "10 phút": a hold, for the tab.
    const auto minutes_vi = [](std::uint64_t ms) {
        return std::to_string(std::max<std::uint64_t>(1, (ms + 30'000) / 60'000)) + " phút";
    };
    // An invite that came to nothing -- the game's message after it, or no
    // join on the HUD kJoinWaitMs on: counted, and the player held off for the
    // count's hold in PartyChat too, so that saw() does not give the name back
    // and the list is not opened for it.
    const auto invite_failed = [&](const std::string& who, const std::string& why_en, const std::string& why_vi) {
        const InviteTries::Outcome o = tries.failed(who, now_ms(), why_en);
        chat.hold_off(who, steady_ms(), o.hold_ms);
        GB_INFO("Party: invite to %s did not lead to a join (%s) -- %d in a row; not invited again for %d s",
                who.c_str(), why_en.c_str(), o.fails, static_cast<int>(o.hold_ms / 1000));
        say_party("Mời " + who + ": " + why_vi + " -- " + minutes_vi(o.hold_ms) + " nữa mới mời lại" +
                  (o.fails >= InviteTries::kFailsForLong ? " (" + std::to_string(o.fails) + " lần liền không vào)."
                                                         : "."));
        return o;
    };
    const auto told_reply = [&](const std::string& who_in, const InviteReply::Result& r, const char* when) {
        const std::string words = r.words.empty() ? std::string("?") : r.words;
        // A message already up before an invite answers nobody known (review
        // of 2026.11.17b): the log says so in English, and the tab's
        // Vietnamese line names no player rather than an English stand-in.
        const std::string who = who_in.empty() ? std::string("an earlier invitee") : who_in;
        const std::string head = who_in.empty() ? std::string("Thông báo có sẵn") : "Mời " + who_in;
        // Its Xác Nhận's click is watched up to InviteReply::kCloseHoldMs
        // (2026.11.17b), the game's mouse held: a slow game said with how
        // long it took, and one that showed nothing that long.
        if (r.how == InviteReply::How::Closed && r.gone_after_ms > kReactSlowMs)
            GB_INFO("Party: the game took %d ms to show its message gone after the click on its Xác Nhận",
                    r.gone_after_ms);
        if (r.how == InviteReply::How::Closed) {
            GB_INFO("Party: the game's message after the invite to %s at %d,%d, %s: \"%s\" -- closed with its Xác "
                    "Nhận (%d click(s))",
                    who.c_str(), r.label.x, r.label.y, when, words.c_str(), r.clicks);
            say_party(head + ": game hiện thông báo \"" + words + "\" -- đã bấm Xác Nhận để tắt.");
        } else {
            GB_WARN("Party: the game's message after the invite to %s at %d,%d, %s (\"%s\"), still up %d ms after "
                    "the click on its Xác Nhận (%s, %s) -- the mouse let go, clicked again on a later pass",
                    who.c_str(), r.label.x, r.label.y, when, words.c_str(), InviteReply::kCloseHoldMs,
                    InviteReply::how_name(r.how), InputManager::game_click_name(reply_how));
            say_party(head + ": game hiện thông báo \"" + words +
                      "\" mà GoBot chưa tắt được -- sẽ bấm Xác Nhận lại.");
        }
    };
    // After a watch that found a message: closed, or one click more on it
    // spent -- and after kCloseTries, no invite for kStuckMs, said once.
    const auto reply_after = [&](const std::string& who, const InviteReply::Result& r) {
        if (r.how == InviteReply::How::None || r.how == InviteReply::How::Stopped) return;
        if (r.how == InviteReply::How::Closed) {
            reply_clicks = 0;
            return;
        }
        reply_who = who;
        reply_clicks += std::max(1, r.clicks);
        if (reply_clicks < InviteReply::kCloseTries) {
            reply_retry_ms = now_ms() + InviteReply::kRetryGapMs;
            return;
        }
        GB_WARN("Party: the game's message is still up after %d clicks on its Xác Nhận -- no invite for %d s",
                reply_clicks, static_cast<int>(InviteReply::kStuckMs / 1000));
        say_party("Thông báo của game vẫn chưa tắt sau " + std::to_string(reply_clicks) +
                  " lần bấm Xác Nhận: bạn bấm Xác Nhận giúp. " + minutes_vi(InviteReply::kStuckMs) +
                  " nữa mới mời tiếp.");
        reply_clicks = 0;
        reply_stuck_until = now_ms() + InviteReply::kStuckMs;
    };

    say_party("watching");
    std::string answered;
    std::string refused;            // the dialog last left alone, and why
    // One look this pass, taken afresh each pass; kept across them so that
    // its frame's 3 MB are grabbed into again rather than made anew at up to
    // 6.7 looks a second (take_party_look keeps the buffer).
    PartyLook look;
    // Why the chat's lines were last said to be unreadable, or empty: said
    // once each time it starts (2026.11.13c).
    std::string chat_unseen_said;

    while (!party_stop_) {
        // Pick up changes made while it runs. The simple screen writes the
        // file as boxes are ticked and has no "restart it" to ask for; a rule
        // that only took effect next time would look like a rule ignored.
        // Its time asked every 2 s, and the file read only when that has
        // moved (2026.11.16): two whole reads every 2 s cost a slow PC for
        // nothing while nobody touched it. One that would not open is read
        // again 2 s on.
        if (rules_watch.due(config)) {
            Party::Settings fresh;
            const bool fresh_read = fresh.load(config);
            if (fresh_read) rules = fresh;
            IniFile fresh_ini;
            if (fresh_ini.load(config)) {
                const std::string keyword = fresh_ini.get("Chat", "keyword", rules.magic_word);
                if (keyword != chat_rules.keyword) {
                    chat_rules.keyword = keyword;
                    chat.set_settings(chat_rules);
                }
            }
            if (!fresh_read) rules_watch.retry();
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

        // The game's answer that comes later (2026.11.14b): "Đối phương đã hủy
        // yêu cầu tham gia nhóm." once the invite is declined or runs out,
        // some ten seconds on. Looked for kLateMs after an invite, every half
        // second at most, with the mouse to itself and nothing else of GoBot's
        // holding the game; closed by its own Xác Nhận, and that player held
        // off as for the answer at once. And (2026.11.17b, B) a message the
        // last click did not close, clicked again here once reply_retry_ms
        // has come -- or seen gone, by itself or by that click late.
        {
            const std::uint64_t t = now_ms();
            if (!last_invited.empty() && t - last_invite_ms >= InviteReply::kLateMs) last_invited.clear();
            // A message waiting for its next click is that click's, kept
            // kRetryGapMs apart: not clicked sooner as a late answer.
            const bool late_due = rules.auto_invite && !last_invited.empty() && reply_clicks == 0;
            const bool retry_due = reply_clicks > 0 && t >= reply_retry_ms;
            if ((late_due || retry_due) && t - late_looked_ms >= 500 && !trash_pass_ &&
                !coord_.critical_active()) {
                late_looked_ms = t;
                if (bag::lone_ok(frame, store, ScreenRect{ 0, 0, frame.width, frame.height })) {
                    std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::try_to_lock);
                    bool ready = mouse.owns_lock() && !trash_pass_;
                    if (ready && !input_.window_focused()) {
                        input_.focus_window();
                        ready = mouse_nap(250);
                    }
                    if (ready) {
                        const InviteReply::Result late =
                            InviteReply::watch(store, reply_font, reply_hands, 2 * InviteReply::kLookMs + 50,
                                               InviteReply::kCloseHoldMs, 1);
                        if (late.how != InviteReply::How::None && late.how != InviteReply::How::Stopped) {
                            // A message not seen before is the last invite's
                            // answer: that invite came to nothing, counted at
                            // once -- unless the HUD has judged it already.
                            const bool again = reply_clicks > 0;
                            const std::string who = again ? reply_who : last_invited;
                            if (!again && !last_invited.empty()) {
                                if (tries.waiting(last_invited))
                                    invite_failed(last_invited, "the game answered it with a message",
                                                  "game trả lời bằng một thông báo");
                                const std::string when = std::to_string((now_ms() - last_invite_ms) / 1000) +
                                                         " s after it (late)";
                                told_reply(last_invited, late, when.c_str());
                                last_invited.clear();
                            } else {
                                told_reply(who, late, "clicked again");
                            }
                            reply_after(who, late);
                        }
                    }
                } else if (retry_due) {
                    GB_INFO("Party: the game's message is gone (%d click(s) on its Xác Nhận before)", reply_clicks);
                    reply_clicks = 0;
                }
            }
        }

        // Each invite's join, kJoinWaitMs after it went out (2026.11.17b, A),
        // on this pass's look -- never waited for: a pass that comes before
        // then asks nothing. No join seen holds that player off.
        if (tries.due(now_ms())) {
            const PartyHud hud = party_read(look, store);
            InviteTries::Hud seen;
            seen.size = hud.state == PartyState::Unknown ? 0 : hud.size;
            seen.stale = hud.stale;
            for (const InviteTries::Outcome& o : tries.judge(seen, now_ms())) {
                if (o.joined) {
                    GB_INFO("Party: %s joined after the invite -- %s", o.who.c_str(), o.why.c_str());
                    continue;
                }
                chat.hold_off(o.who, steady_ms(), o.hold_ms);
                GB_INFO("Party: invite to %s did not lead to a join (%s) -- %d in a row; not invited again for %d s",
                        o.who.c_str(), o.why.c_str(), o.fails, static_cast<int>(o.hold_ms / 1000));
                say_party("Mời " + o.who + ": sau " + std::to_string(InviteTries::kJoinWaitMs / 1000) +
                          " giây chưa thấy vào tổ đội -- " + minutes_vi(o.hold_ms) + " nữa mới mời lại" +
                          (o.fails >= InviteTries::kFailsForLong ? " (" + std::to_string(o.fails) +
                                                                       " lần liền không vào)."
                                                                 : "."));
            }
        }

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
        //
        // Not while the bottom of the game is off the screen or under the
        // taskbar on a look read off the screen (2026.11.13c): the chat's lines
        // sit just above the chat bar, and what is read there is black, or the
        // taskbar. Said once each time it starts, on the tab and in the log.
        std::string chat_unseen;
        if (rules.auto_invite && have_font) {
            chat_unseen = game_unseen(kChatRowsUnder, "đọc được dòng chat để mời người chat từ khoá", look.from_screen);
            if (chat_unseen != chat_unseen_said) {
                if (!chat_unseen.empty()) {
                    GB_INFO("Party: the chat's lines are not read -- the bottom of the game is off the screen or "
                            "under the taskbar");
                    say_party(chat_unseen);
                }
                chat_unseen_said = chat_unseen;
            }
        }
        // A message of the game's that two clicks did not close: no invite for
        // a minute (InviteReply::kStuckMs), said once.
        const bool reply_stuck = now_ms() < reply_stuck_until;
        if (reply_stuck && !reply_stuck_said)
            GB_INFO("Party: no invite for now -- a message of the game's did not close after its Xác Nhận");
        reply_stuck_said = reply_stuck;
        if (rules.auto_invite && have_font && chat_unseen.empty()) {
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
            // Nobody counted as asking while a message of the game's is stuck,
            // nor while one the last click did not close waits to be clicked
            // again (2026.11.17b): the list is not opened over it.
            if (reply_stuck || reply_clicks > 0) lines.clear();

            for (const std::string& who : chat.saw(lines, now)) {
                if (party_stop_) break;
                // A message left up by an earlier asker's turn of this pass,
                // or the invites stopped by one: the rest wait for a later
                // pass (review of 2026.11.17b) -- not the list opened over it
                // again, and not its clicks spent all at once without the
                // kRetryGapMs between them.
                if (reply_clicks > 0 || now_ms() < reply_stuck_until) break;
                if (!rules.may_invite(who)) {
                    say_party(who + " asked, and the rules say no");
                    continue;
                }
                // Held off for an invite that did not lead to a join
                // (2026.11.17b, A): before anything is opened or clicked.
                // PartyChat::hold_off keeps saw() from giving the name back
                // already; this catches a name it read differently.
                {
                    std::uint64_t left = 0;
                    if (tries.held(who, now_ms(), &left)) {
                        GB_INFO("Party: %s asked; held off %d s more after an invite that did not lead to a join -- "
                                "the list not opened",
                                who.c_str(), static_cast<int>((left + 999) / 1000));
                        continue;
                    }
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
                // The HUD read for it is what the invite's join is judged
                // against kJoinWaitMs on (InviteTries).
                PartyHud hud_before;
                if (!party_may_invite(who, look, store, rules.always_leader, &hud_before)) continue;
                const int size_before = hud_before.state == PartyState::Unknown || hud_before.stale ? 0
                                                                                                    : hud_before.size;

                // The mouse to itself for the whole of it -- opening the list,
                // finding the row, the invite -- so a trash pass that starts
                // meanwhile waits for it rather than having its drags clicked
                // through.
                std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::try_to_lock);
                if (!mouse.owns_lock() || trash_pass_) {
                    say_party(who + " asked while the trash cleaner had the mouse; not invited");
                    continue;
                }

                // Its waits end on the character down as on a stop
                // (mouse_stopped): the rest of this pass's askers wait for the
                // next pass, and the reviver gets the mouse.
                std::string why;
                if (!list.ensure_open(input_, capture, store, &why, mouse_stopped)) {
                    if (stopped()) break;
                    if (down()) {
                        say_party(who + " asked while the character is down; not invited");
                        break;
                    }
                    say_party(who + " asked, but the list would not open: " + why);
                    continue;
                }
                // The list shut again after this asker's turn, whatever came
                // of it (2026.11.17b, C): by its own button, never ESC --
                // when the worker opened it, or opened it before and could
                // not shut it; one the player had up is left. Not shut: said
                // once until a close works, and not pressed again this turn.
                if (list.opened_it()) list_left_open = true;
                const auto shut_list = [&] {
                    if (!list_left_open || stopped() || down()) return;
                    std::string why_open;
                    if (list.close(input_, capture, store, &why_open, mouse_stopped)) {
                        list_left_open = false;
                        list_close_said = false;
                        return;
                    }
                    if (why_open == "stopped" || list_close_said) return;
                    list_close_said = true;
                    GB_INFO("Party: the player list did not shut after %s's turn -- %s; left as it is",
                            who.c_str(), why_open.c_str());
                };
                Frame open_now;
                if (!capture.grab(input_.target_window(), open_now) || !open_now.valid()) {
                    shut_list();
                    continue;
                }
                // A message of the game's up from before -- a late refusal, or
                // one the player opened (2026.11.14b): closed by its Xác Nhận
                // first, on two looks of its own, and nobody held off for it.
                // One click, the mouse held kCloseHoldMs at most (2026.11.17b);
                // still up, it is clicked again on a later pass, and nobody is
                // invited over it.
                if (bag::lone_ok(open_now, store, ScreenRect{ 0, 0, open_now.width, open_now.height })) {
                    const InviteReply::Result before =
                        InviteReply::watch(store, reply_font, reply_hands, 2 * InviteReply::kLookMs + 50,
                                           InviteReply::kCloseHoldMs, 1);
                    if (before.how == InviteReply::How::Stopped) break;
                    if (before.how != InviteReply::How::None) {
                        GB_INFO("Party: a game message was up before inviting %s (\"%s\") -- %s with its Xác Nhận "
                                "(%d click(s))",
                                who.c_str(), before.words.c_str(), InviteReply::how_name(before.how), before.clicks);
                        // Nobody's invite known: reply_who left empty, so a
                        // retry's tab line names no player.
                        reply_after("", before);
                        if (before.how != InviteReply::How::Closed) {
                            say_party(who + " asked; a message of the game's is up and did not close yet -- not "
                                            "invited");
                            shut_list();
                            continue;
                        }
                        if (!capture.grab(input_.target_window(), open_now) || !open_now.valid()) {
                            shut_list();
                            continue;
                        }
                    }
                }

                const auto rows = list.read(open_now, font);
                const int at = list.find(rows, who);
                if (at < 0) {
                    say_party(who + " asked, but is not among the " +
                              std::to_string(rows.size()) + " on this map");
                    // No invite went out, so not one that came to nothing (A,
                    // review of 2026.11.17b): not counted in InviteTries, and
                    // never the ten minutes -- a friend still on the way, or a
                    // name the list misread, would be shut out once there. A
                    // short hold in the chat only, the same each time, so the
                    // list is not opened for nobody on every line of theirs.
                    chat.hold_off(who, steady_ms(), InviteTries::kNotListedHoldMs);
                    GB_INFO("Party: %s asked but is not among the %d on the player list -- not asked again for %d s",
                            who.c_str(), static_cast<int>(rows.size()),
                            static_cast<int>(InviteTries::kNotListedHoldMs / 1000));
                    shut_list();
                    continue;
                }
                const PlayerList::Row* row = nullptr;
                for (const auto& r : rows) if (r.index == at) row = &r;
                if (row == nullptr) {
                    shut_list();
                    continue;
                }

                // Whatever it says, the row and perhaps Req Party were
                // clicked: the party may change from here, and the next asker
                // of this pass is not invited on this pass's look.
                const bool sent = list.invite(input_, capture, store, *row, &why, mouse_stopped);
                party_events().changed(now_ms(), "an invite was sent", party_text::kEventInvited);
                // Whether it led to a join is asked of the HUD kJoinWaitMs on
                // (A), sent or not: Req Party may have been clicked all the
                // same.
                if (why != "stopped") tries.sent(who, now_ms(), size_before);
                // The game's answer, when it is a message box of its own --
                // already in a party, refused (2026.11.14b): its Xác Nhận
                // clicked, never a key, and that invite counted as come to
                // nothing at once (A: a minute's hold, ten after two in a row).
                // Looked for after an invite that did not go as well. The
                // mouse is still this worker's, and held kCloseHoldMs at most
                // after the click (B).
                const InviteReply::Result reply =
                    InviteReply::watch(store, reply_font, reply_hands, InviteReply::kWatchMs,
                                       InviteReply::kCloseHoldMs, 1);
                if (reply.how == InviteReply::How::Stopped) break;
                if (reply.how != InviteReply::How::None) {
                    last_invited.clear();
                    const std::string when = std::to_string(reply.seen_after_ms) + " ms after it";
                    told_reply(who, reply, when.c_str());
                    invite_failed(who, "the game answered it with a message", "game trả lời bằng một thông báo");
                    reply_after(who, reply);
                    shut_list();
                    continue;
                }
                // The list shut after the watch: a good invite shuts it by
                // itself, and close() presses nothing then.
                shut_list();
                if (!sent) {
                    say_party("invite to " + who + " did not go: " + why);
                    continue;
                }
                say_party("invited " + who);
                ++party_invited_;
                alert_party(false, who);
                // Its answer may come later still: looked for kLateMs -- once
                // the list is shut, a message it hid is in sight.
                last_invited = who;
                last_invite_ms = now_ms();
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
    // max_restarts' hour, a wait and the crossings not in, from none: counted
    // from each start of the keeper, as the run's number was (2026.11.24).
    // No keeper thread runs here: the last one was joined above.
    keeper_restart_times_.clear();
    keeper_wait_until_ms_ = 0;
    keeper_wait_told_ = false;
    keeper_retry_told_ = false;
    keeper_again_ = false;
    keeper_waiting_ = false;
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
    // A look every few seconds: it can wait for the game's slice to end.
    quiet_this_thread();
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
    // The disconnect alert told for the trouble under way: once a trouble --
    // not again after a wait for the hour's restarts, or a crossing made again
    // (2026.11.24). Over once back in the world, or the game seen fine with
    // nothing owed.
    bool told_down = false;

    // What comes after one of this loop's own crossings. Since 2026.11.24
    // (the owner: "Kiểm tra các phần khác xem có phần nào không làm được thì
    // dừng không, cần thay cơ chế này bằng 1 cơ chế khác") false only to stop
    // -- told to, or GoBot not running as administrator, said and alerted in
    // run_crossing. A crossing not in is made again: kCrossAgainMs on (Retry)
    // or once the hour's restarts allow one more (Wait), without end. Until
    // then either stopped the keeper for a person.
    const auto after = [&](Crossed got, Crossing how) {
        switch (got) {
            case Crossed::In: {
                seen_running = true;
                crossed_in = true;
                told_down = false;
                // In by the screen's word. Beside it, what the packets had of
                // the way in -- the last map entry, and the link -- only
                // written down (the packets plan, item I).
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
                keeper_stall_restart();
                return true;
            }
            case Crossed::Cancelled:
                return true;
            case Crossed::Retry:
            case Crossed::Wait: {
                // Made again on the look after the wait, as it began. Only a
                // stop ends the 30 s; the login key's request ends the wait
                // for the hour too -- taken up on that look while the game is
                // up, or let go of, said, at the next wait for the hour with
                // the game in trouble, so it never ends every wait at once.
                keeper_again_ = true;
                keeper_again_how_ = how;
                keeper_waiting_ = true;
                const bool on =
                    got == Crossed::Retry
                        ? keeper_nap_until(now_ms() + static_cast<std::uint64_t>(CrashWatch::kCrossAgainMs), nullptr)
                        : keeper_nap_until(keeper_wait_until_ms_, &keeper_login_now_);
                keeper_waiting_ = false;
                return on;
            }
            case Crossed::Stopped:
            case Crossed::NotElevated:
                break;
        }
        return false;
    };

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
        const bool fine = move == CrashWatch::Move::Watch || move == CrashWatch::Move::Follow;

        // A crossing of this loop's that did not get in, made again now its
        // wait is over (2026.11.24) -- with the client fine to CrashWatch,
        // which a game left at the channel list or the character select is,
        // or gone. The same crossing: nothing counted, alerted or ended for it
        // -- its restart was counted as its client was ended -- and the flow
        // carries on from wherever the game is, or finds it in the world
        // already. Not followed meanwhile as a cold start's game found up: the
        // character is not in the world. A client that hangs or shows a box
        // meanwhile is a restart of its own, below.
        if (keeper_again_ && (fine || seen.what == CrashWatch::Trouble::Gone)) {
            // A client the failed crossing got up and that is gone now: a
            // relaunch, counted against max_restarts (the 2026.11.24 review)
            // -- one that dies on each launch was relaunched every minute or
            // two with no count. Refused, the keeper waits as for any restart
            // over the hour's, the crossing still owed.
            if (seen.what == CrashWatch::Trouble::Gone && keeper_again_how_ != Crossing::Cold &&
                keeper_again_launched_) {
                if (!keeper_take_restart(watch_settings)) {
                    keeper_waiting_ = true;
                    const bool on = keeper_nap_until(
                        std::min(keeper_wait_until_ms_, now_ms() + static_cast<std::uint64_t>(CrashWatch::kCrossAgainMs)),
                        &keeper_login_now_);
                    keeper_waiting_ = false;
                    if (!on) break;
                    keeper_login_now_ = false;
                    continue;
                }
                keeper_again_launched_ = false;
            }
            keeper_again_ = false;
            keeper_closed_counted_ = false;
            keeper_note("crossing again -- the last crossing did not get in");
            if (!after(run_crossing(keeper_again_how_, watch, watch_settings, login, store), keeper_again_how_))
                break;
            continue;
        }

        if (fine) {
            seen_running = true;
            // Up and answering with nothing owed: the trouble told is over.
            told_down = false;
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
            // A look every 6 s (2026.11.16; 3 s before): a crash is still
            // seen within seconds, at half the cost on a slow PC. The login
            // key's request cuts the wait short, so "Vào game ngay" is not
            // answered later than it was.
            for (int i = 0; i < 60 && !keeper_login_now_ && !keeper_stop_; ++i)
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
        // either way, and one that fails is made again (after).
        const bool cold = move == CrashWatch::Move::Start;
        // A client gone because a crossing ended it -- the login key's, then
        // cancelled by the key -- was counted as it was ended: the same
        // closing is not a second restart, nor a disconnect to tell of.
        const bool counted = !cold && keeper_closed_counted_ && seen.what == CrashWatch::Trouble::Gone;
        keeper_closed_counted_ = false;
        if (!cold && !counted && !told_down) {
            told_down = true;
            alert("disconnect",
                  alert_text::disconnect(seen.what, seen.code,
                                         CrashWatch::restart_allowed(keeper_restart_times_, now_ms(),
                                                                     watch_settings.max_restarts,
                                                                     CrashWatch::kRestartWindowMs) &&
                                             elevated_ && login.unidentified().empty(),
                                         seen.minutes));
        }

        // This client gone is a restart, counted before anything is ended
        // (keeper_take_restart). The hour's used up -- said and alerted there,
        // once a wait -- nothing is ended: the keeper waits, nothing clicked,
        // until one more is allowed, and looks again; the trouble may be gone
        // by then (2026.11.24, the owner's default of question 3; until then
        // it stopped here for a person after max_restarts in a run). A stop
        // ends the wait, and so does the login key: the game is looked at
        // again at once -- a person may have put it right. The key's crossing
        // cannot be made on a game in trouble with the hour used up, so its
        // request goes then, said: left pending, it would end every wait
        // after this one at once.
        if (!cold && !counted && !keeper_take_restart(watch_settings)) {
            keeper_waiting_ = true;
            // Looked at again every kCrossAgainMs meanwhile (the 2026.11.24
            // review): a game put right by a person, or come back by itself,
            // ends the wait; still in trouble, refused again in silence.
            const bool on = keeper_nap_until(
                std::min(keeper_wait_until_ms_, now_ms() + static_cast<std::uint64_t>(CrashWatch::kCrossAgainMs)),
                &keeper_login_now_);
            keeper_waiting_ = false;
            if (!on) break;
            if (keeper_login_now_.exchange(false)) {
                const std::uint64_t now = now_ms();
                const std::string until = clock_after(keeper_wait_until_ms_ > now ? keeper_wait_until_ms_ - now : 0);
                keeper_note("login key: no crossing -- the game needs a restart, and the hour's are used up until " +
                            until + "; looking at the game again");
                say_keeper_vi("Phím vào game ngay: game cần đóng rồi vào lại, mà đã vào lại " +
                              std::to_string(CrashWatch::restarts_within(keeper_restart_times_, now,
                                                                         CrashWatch::kRestartWindowMs)) +
                              " lần trong 1 giờ — GoBot chờ tới " + until + " rồi tự vào lại.");
            }
            continue;
        }

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
        // One owed from before is this one.
        keeper_again_ = false;
        const Crossing how = cold ? Crossing::Cold : Crossing::Restart;
        if (!after(run_crossing(cold ? Crossing::Cold : Crossing::Restart, watch, watch_settings, login, store), how))
            break;      // stopped, or not running as administrator: said and alerted there
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

void GuiApp::keeper_not_in(const std::string& english, const std::string& now_vi, const std::string& problem_vi) {
    GB_WARN("Keeper: %s", english.c_str());
    say_keeper(english);
    say_keeper_vi(now_vi, problem_vi);
}

bool GuiApp::keeper_nap_until(std::uint64_t until_ms, const std::atomic<bool>* cut) {
    while (!keeper_stop_ && now_ms() < until_ms && !(cut != nullptr && cut->load()))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return !keeper_stop_;
}

bool GuiApp::keeper_take_restart(const CrashWatch::Settings& watch_settings) {
    // max_restarts in any hour, not in a run (2026.11.24, the owner: "Kiểm tra
    // các phần khác xem có phần nào không làm được thì dừng không, cần thay cơ
    // chế này bằng 1 cơ chế khác" -- his default of question 3). Used up, it
    // is a wait, not the end: until then "that is N restarts ... stopping, a
    // person is needed", and the keeper stopped.
    const std::uint64_t now = now_ms();
    const std::uint64_t window = CrashWatch::kRestartWindowMs;
    std::uint64_t next_at = 0;
    // Spaced once the hour's are used (the 2026.11.24 review): one more every
    // window / max_restarts after the newest -- 12 min at the defaults -- not
    // a sleep until the oldest is an hour old.
    if (CrashWatch::restart_spaced(keeper_restart_times_, now, watch_settings.max_restarts, window,
                                   static_cast<std::uint64_t>(CrashWatch::kCrossAgainMs), &next_at)) {
        // The hour's alone are kept: one older counts for nothing any more.
        keeper_restart_times_.erase(
            std::remove_if(keeper_restart_times_.begin(), keeper_restart_times_.end(),
                           [now, window](std::uint64_t t) { return t < now && now - t >= window; }),
            keeper_restart_times_.end());
        keeper_restart_times_.push_back(now);
        ++keeper_restarts_;
        keeper_wait_told_ = false;
        return true;
    }
    keeper_wait_until_ms_ = next_at;
    // Said and alerted once a wait: a look in it that finds the game still in
    // trouble asks again, and is refused again, in silence.
    if (!keeper_wait_told_) {
        keeper_wait_told_ = true;
        const int n = CrashWatch::restarts_within(keeper_restart_times_, now, window);
        const std::string until = clock_after(next_at > now ? next_at - now : 0);
        const std::string told = alert_text::gave_up_restarts(n, until);
        GB_WARN("Keeper: %d restart(s) in the last hour (max_restarts %d) and the game needs another -- nothing "
                "ended or crossed until %s, when the oldest is an hour old; then it goes on",
                n, watch_settings.max_restarts, until.c_str());
        say_keeper(std::to_string(n) + " restart(s) in the last hour -- waiting until " + until +
                   " before the next, nothing ended or clicked meanwhile");
        say_keeper_vi(told);
        alert("keeper_gave_up", told);
    }
    return false;
}

/// One crossing for the keeper, whichever way it began -- see Crossing.
GuiApp::Crossed GuiApp::run_crossing(Crossing how, CrashWatch& watch,
                                     const CrashWatch::Settings& watch_settings,
                                     AutoLogin::Settings& login, TemplateStore& store) {
    const bool key = how == Crossing::Key;
    keeper_again_launched_ = false;

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
    // Not set up: made again kCrossAgainMs on, login.ini read afresh then
    // (2026.11.24; the keeper stopped here until then) -- the file may be put
    // right meanwhile. The phone told once an episode, until back in.
    if (!login.unidentified().empty()) {
        std::string pages;
        for (const AutoLogin::Page* page : login.unidentified())
            pages += (pages.empty() ? "" : ", ") + page->key;
        keeper_not_in("the crossing is not set up, so it cannot be made -- see configs/login.ini; looked at again in " +
                          std::to_string(CrashWatch::kCrossAgainMs / 1000) + " s",
                      not_in_vi(how == Crossing::Restart, "chưa cài đặt đủ các màn hình đăng nhập"),
                      AutoLogin::advice(AutoLogin::Why::NotSetUp, pages));
        if (!keeper_retry_told_) {
            keeper_retry_told_ = true;
            alert("keeper_gave_up", alert_text::gave_up_not_set_up());
        }
        return Crossed::Retry;
    }
    // The game and VTCZone run as administrator, and Windows drops clicks
    // sent to them from a program that does not. The one stop left: only
    // starting GoBot again as administrator puts it right.
    if (!elevated_) {
        keeper_give_up("GoBot is not running as administrator, so its clicks cannot reach VTCZone or the game",
                       "GoBot không chạy bằng quyền Administrator nên không bấm được vào VTCZone/game.");
        alert("keeper_gave_up", alert_text::gave_up_not_elevated());
        return Crossed::NotElevated;
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
            keeper_note("logging back in (" +
                        std::to_string(CrashWatch::restarts_within(keeper_restart_times_, now_ms(),
                                                                   CrashWatch::kRestartWindowMs)) +
                        " restart(s) in the last hour, at most " + std::to_string(watch_settings.max_restarts) +
                        ")");
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
    // this crossing began. With the hour's used up, nothing is ended: the
    // crossing is cancelled, and made again once one more is allowed
    // (Crossed::Wait, 2026.11.24; it stopped for a person until then). Only by
    // a keeper the player ticked "Tự vào lại game" for: one started for the
    // login key alone never ends the game -- the player asked for a crossing,
    // not for the client to be closed -- and a stuck attempt there simply
    // fails.
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
                keeper_note("ending a game stuck part way in (" +
                            std::to_string(CrashWatch::restarts_within(keeper_restart_times_, now_ms(),
                                                                       CrashWatch::kRestartWindowMs)) +
                            " restart(s) in the last hour, at most " + std::to_string(watch_settings.max_restarts) +
                            ")");
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
    // Whether this crossing got the client up (keeper_again_launched_).
    bool reached_game = false;
    flow.on_step([this, doing, &flow, &reached_game](AutoLogin::Step s, const std::string& detail) {
        if (s == AutoLogin::Step::Connect || s == AutoLogin::Step::Channel || s == AutoLogin::Step::ConnectAgain ||
            s == AutoLogin::Step::Character || s == AutoLogin::Step::Message)
            reached_game = true;
        say_keeper(std::string(AutoLogin::step_name(s)) + ": " + detail);
        if (s == AutoLogin::Step::Blocked || s == AutoLogin::Step::InWorld) return;
        std::string vi = doing + AutoLogin::step_name_vi(s) + "…";
        // The game's message after Kết Nối (2026.11.13b): closed, and how
        // often this crossing -- Kết Nối again each time, no limit.
        // Said once each is closed: the line that says Kết Nối comes again.
        if (s == AutoLogin::Step::Message && detail.rfind("the game put up a message", 0) == 0)
            vi += " Game hiện thông báo (như \"Tài khoản đã bị chặn\"): GoBot đã bấm Xác Nhận và bấm Kết Nối lại (lần " +
                  std::to_string(flow.messages_closed()) + ").";
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
        // The arrival kept before the popup worker's gate "the keeper is
        // crossing" opens (2026-11-15): the check-in board comes up now, its
        // ESC waits 30 s from here (PopupCloser::board_due), and set after the
        // gate opened the worker could take an old arrival for this one for a
        // moment. Pictured 15 s on as well (CheckinShots' in15).
        if (in) crossed_in_ms_ = now_ms();      // the check-in board comes up now: its ESC 30 s on
        keeper_flow_ = nullptr;
    }
    if (keeper_stop_) return Crossed::Stopped;
    // The hour's restarts used up (said and alerted by keeper_take_restart):
    // the game is left as it is, and the crossing made again once one more is
    // allowed (2026.11.24; the keeper stopped here until then).
    if (out_of_restarts) return Crossed::Wait;
    if (key && keeper_key_cancel_.exchange(false)) {
        keeper_note("login key: cancelled");
        say_keeper_vi("Đã huỷ vào game.");
        return Crossed::Cancelled;
    }
    if (!in) {
        // Not in, every attempt of the crossing's used: said, the phone told
        // once an episode (until back in), and made again kCrossAgainMs on
        // (2026.11.24, the owner: "Kiểm tra các phần khác xem có phần nào
        // không làm được thì dừng không, cần thay cơ chế này bằng 1 cơ chế
        // khác"). The next round reads login.ini and the pictures again and
        // goes through once what was in the way is gone: a page slow to come,
        // VTCZone's sign-in form or the launcher's Security Alert answered by
        // a person -- neither is ever clicked or typed into. Until then the
        // keeper stopped here, "a person is needed".
        keeper_not_in((how == Crossing::Restart ? "could not get back in: " : "could not get in: ") + flow.trouble() +
                          " -- crossing again in " + std::to_string(CrashWatch::kCrossAgainMs / 1000) + " s",
                      not_in_vi(how == Crossing::Restart, alert_text::crossing_reason(flow.why())), flow.advice());
        if (!keeper_retry_told_) {
            keeper_retry_told_ = true;
            alert("keeper_gave_up", alert_text::gave_up_crossing(flow.attempts_made(), flow.why()));
        }
        keeper_again_launched_ = reached_game;
        return Crossed::Retry;
    }
    keeper_closed_counted_ = false;
    // In: whatever kept it out is behind it -- told again should it come back
    // -- and nothing is owed (2026.11.24).
    keeper_retry_told_ = false;
    keeper_again_ = false;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        keeper_problem_vi_.clear();
    }
    // The game put in its screen's corner: GoBot's window moved off it, if
    // it now lies over it, by the window's own thread (2026.11.14).
    if (flow.moved_to_corner()) clear_after_login_ = true;
    keeper_note(key ? "login key: in the world" : "back in the world");
    if (how == Crossing::Restart)
        alert("back_in", alert_text::back_in(CrashWatch::restarts_within(keeper_restart_times_, now_ms(),
                                                                          CrashWatch::kRestartWindowMs)));
    say_keeper_vi(how == Crossing::Restart ? "Đã vào lại game. Đang theo dõi game."
                  : keeper_oneshot_        ? "Đã vào game."
                                           : "Đã vào game. Đang theo dõi game.");
    keeper_reattach_ = true;
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
        // Said once: a cold start waiting on the window asks every few
        // seconds, and since 2026.11.24 tick() does for any log in of the
        // keeper's, every kReattachAgainMs from this miss.
        if (!reattach_missed_)
            GB_WARN("The game is up, but its window could not be found -- looking again");
        reattach_missed_ = true;
        reattach_missed_ms_ = now_ms();
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
/// walking would be worse. When a round's tries run out, nothing is stopped
/// (2026.11.24, the owner: "Kiểm tra các phần khác xem có phần nào không làm
/// được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác"; until then the
/// player's rule stopped the whole bot there): the hold stays while the
/// character lies dead, and Reviver::kRoundGapMs on this asks for another
/// round (Reviver::again), without end, until the character is up -- by a
/// revive of its own or by hand. tick() says it, once a death.
void GuiApp::revive_loop() {
    name_this_thread("revive");
    // A look a second or so for a death: it can wait for the game's slice
    // to end on a one-core PC.
    quiet_this_thread();
    const auto tell = [this](const std::string& line) {
        GB_INFO("Revive: %s", line.c_str());
        say_revive(line);
    };
    const auto finish = [&](const std::string& why) {
        tell(why);
        revive_running_ = false;
    };

    // The program's one set of pictures, held while this runs (2026-11-16).
    std::shared_ptr<const TemplateStore> pictures =
        TemplateStore::shared_best(exe_relative("configs/templates.ini"));
    ScreenCapture capture;
    capture.set_label("revive");
    const auto grab = [&](Frame& f) {
        return input_.window_valid() && capture.grab(input_.target_window(), f) && f.valid();
    };
    // None loaded, or not the ones the reviver needs (Reviver::ready): no
    // return any more (2026.11.24, the owner: "Kiểm tra các phần khác xem có
    // phần nào không làm được thì dừng không, cần thay cơ chế này bằng 1 cơ chế
    // khác") -- the worker waits, clicking nothing, says so once in red on the
    // card (revive_wait_vi_), and looks for them again every 30 s
    // (wait_for_pictures); its stop ends the wait at once. Until then Tự hồi
    // sinh ended here until the next BẮT ĐẦU, and a death that night was not
    // seen. The reviver asked is a look only -- the one that runs is made
    // below, from the set the wait ended with.
    // Waiting (the 2026.11.24 review): the death alert by the HP figure stays
    // on meanwhile (simple_alerts.cpp, by_hp) -- this worker watches no death
    // panel, so a death in the wait would be told to nobody.
    revive_waiting_ = true;
    const bool have_pictures = wait_for_pictures(
        pictures, exe_relative("configs/templates.ini"), "Revive",
        [&](const TemplateStore& s) {
            if (s.size() == 0) return std::string("hình giao diện");
            std::string why;
            return Reviver(input_, s, grab).ready(&why) ? std::string() : why;
        },
        [this] { return revive_stop_.load(); },
        [&](const std::string& what) {
            const std::string line = pictures_again_vi(what);
            {
                std::lock_guard<std::mutex> lock(worker_mutex_);
                revive_wait_vi_ = line;
            }
            tell(line);
        });
    revive_waiting_ = false;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        revive_wait_vi_.clear();
    }
    if (!have_pictures) {
        revive_running_ = false;
        return;
    }
    const TemplateStore& store = *pictures;
    Reviver reviver(input_, store, grab);
    std::string why_not;
    if (!reviver.ready(&why_not)) {
        // Not to be: the set the wait ended with was just found ready. Said,
        // should a later change of Reviver's make it so, never a silent end.
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
    // tries are told by revive_out_of_tries alone, once a death however many
    // rounds it takes (2026.11.24; stop_after_revive, with why the bot
    // stopped, until then).
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

    // A round run out: when the next is asked for (now_ms()), 0 for none.
    std::uint64_t next_round_ms = 0;
    tell("watching");
    while (!revive_stop_) {
        for (int i = 0; i < 12 && !revive_stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (revive_stop_) break;

        const Reviver::Turn turn = reviver.turn();
        // No death panel nor its question on this look (2026.11.21, B10): the
        // popup closer may give the check-in board its ESC with HP read down
        // (checkin_down_esc). Any other turn -- a panel, a wait, no frame --
        // takes that back.
        revive_no_panel_ms_ = turn == Reviver::Turn::Alive ? now_ms() : 0;
        if (turn == Reviver::Turn::Alive || turn == Reviver::Turn::Revived) {
            release();
            // Up -- by a revive, or by hand while the round waited: nothing
            // owed.
            next_round_ms = 0;
            revive_round_ms_ = 0;
        }
        if (turn == Reviver::Turn::OutOfTries) {
            // The hold stays -- let go of only once the character is up --
            // so nothing presses a key at the panel meanwhile; the bot is not
            // stopped (2026.11.24). The next round kRoundGapMs on, without
            // end -- never the village, never Cash.
            const std::uint64_t now = now_ms();
            // The hold taken again when an Alive or Revived look let it go --
            // the panel flickering, HP not read -- and the panel came back
            // with the tries still used up (the 2026.11.24 review): else every
            // key went to the death panel for the whole gap. Not over the
            // trash's or the chat doubt's own hold, as guard.take has it.
            const bool under_other = (trash_hold_ && coord_.critical_reason() == "trash cleaning") ||
                                     (chat_doubt_ && coord_.critical_reason() == kChatHold);
            if (!holding && !under_other && take_hold("reviving")) {
                holding = true;
                input_.release_all();
            }
            if (next_round_ms == 0) {
                next_round_ms = now + static_cast<std::uint64_t>(Reviver::kRoundGapMs);
                revive_round_ms_ = next_round_ms;
                tell("out of tries (round " + std::to_string(reviver.round()) + ") -- nothing stopped: every "
                     "key stays held while the character is dead, and another round of " +
                     std::to_string(Reviver::kAttempts) + " tries comes in " +
                     std::to_string(Reviver::kRoundGapMs / 1000) + " s");
            } else if (now >= next_round_ms) {
                next_round_ms = 0;
                revive_round_ms_ = 0;
                reviver.again();
            }
        }
    }

    release();
    revive_no_panel_ms_ = 0;
    revive_round_ms_ = 0;
    tell("stopped");
    revive_running_ = false;
}

void GuiApp::revive_out_of_tries() {
    std::string why;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        why = revive_gave_up_why_;
    }
    // Said, never a stop (2026.11.24): until then this was stop_after_revive,
    // which stopped every job as the panic key does, said "Đã dừng." and kept
    // the out-of-tries words in red until the next BẮT ĐẦU. Now the worker
    // keeps its hold while the character lies dead and asks for another round
    // every Reviver::kRoundGapMs; revived by hand, everything goes on.
    const std::string vi = Reviver::out_of_tries_vi();
    simple_note(vi, 30000);
    GB_WARN("Revive: %s -- nothing is stopped: every key stays held while the character is dead, and another "
            "round of %d tries comes every %d s, never the village or Cash, until it is up",
            why.c_str(), Reviver::kAttempts, Reviver::kRoundGapMs / 1000);
    alert("revive_failed", vi);
    set_status("The revive failed 3 times -- probably out of revive items. Another round of 3 in 30 s, and so on; "
               "revived by hand, everything goes on.",
               30000);
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

void GuiApp::request_pet() {
    // As request_shop, with no request of its own: the worker buys what
    // PetRestock owes at its next turn, and stays while it is owed.
    vitals_for_job();
    if (trash_running_) return;
    join_pumping(trash_thread_);
    trash_stop_ = false;
    trash_oneshot_ = true;
    trash_running_ = true;
    trash_thread_ = std::thread([this] { trash_loop(); });
}

void GuiApp::pet_arrival_tick() {
    // 2026.11.24, the owner: "Khi ra tới bản đồ TổCổLong thì cần mua lại HP
    // cho pet". The box and its slots as the file has them -- the card writes
    // it as it is ticked -- read every 2 s, never every tick.
    const std::uint64_t now = now_ms();
    if (pet_read_ms_ == 0 || now - pet_read_ms_ >= 2000) {
        pet_read_ms_ = now;
        CleanSettings s;
        if (s.load(exe_relative("configs/trash.ini"))) pet_on_ = s.buy_pet && s.buy_pet_slots > 0;
    }
    const bool armed = pet_on_ && simple_running() && !simple_cold_start_;
    pet_armed_ = armed;
    const PacketWorld::Place place = packet_world_.place();
    const std::string stem = place.map ? GameMapBook::stem_of(place.map, place.area) : std::string();
    if (pet_restock_.look(stem, place.epoch, armed, now))
        GB_INFO("Pet: into %s (map entry %u) -- a round of the pet's HP food (%s) owed; the dungeon's lobby waits "
                "for it %llu s at most",
                stem.c_str(), place.epoch, CleanSettings::kBuyPetName,
                static_cast<unsigned long long>(PetRestock::kWaitMostMs / 1000));
    const std::uint32_t owed = pet_restock_.owed(now);
    if (owed == 0) return;
    // DỪNG or F12 since: nothing is owed by a bot that does not run.
    if (!simple_running()) {
        pet_restock_.over(owed);
        GB_INFO("Pet: the bot stopped -- the pet's round for map entry %u dropped", owed);
        return;
    }
    // No worker to buy it -- no timed bag job ticked, or the last one gone:
    // one for this round, at most every 5 s.
    if (!trash_running_ && now - pet_wake_ms_ >= 5000) {
        pet_wake_ms_ = now;
        GB_INFO("Pet: a trash worker started for the pet's round (map entry %u)", owed);
        request_pet();
    }
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
    // So nothing that presses keys starts until the game has been looked at
    // (keys_may_start), or the bin is seen with nothing waiting -- looked for
    // every couple of seconds, by watch_left_held when no trash worker runs:
    // see bin_seen_clear.
    if (trash_hold_.exchange(false)) {
        // The last bag job unticked while the run goes on, not DỪNG (the
        // audit's B9, 2026.11.21): what is stopped here is remembered, and
        // started again by itself once the bin is seen clear -- the big
        // button reads DỪNG LẠI meanwhile, and "bấm BẮT ĐẦU" there stopped
        // everything else instead. Under DỪNG nothing is remembered.
        bool stopping = false;
        {
            std::lock_guard<std::mutex> lock(hold_mutex_);
            stopping = stopping_;
        }
        if (!stopping) {
            if (hunting_) left_held_resume_hunt_ = true;
            if (macro_running_) left_held_resume_macro_ = true;
        }
        stop_hunt();
        stop_macro();
        ++left_held_gen_;
        trash_left_held_ = true;
        const bool goes_on = !stopping && simple_running();
        // Nothing goes on (the note says to press BẮT ĐẦU): nothing remembered
        // either (the review of 2026.11.21) -- or a later start of the hunter
        // alone found the macro remembered, and started it unasked.
        if (!goes_on) {
            left_held_resume_hunt_ = false;
            left_held_resume_macro_ = false;
        }
        simple_note(goes_on ? "Hộp thoại của thùng rác có thể vẫn đang mở trong game, nên GoBot dừng "
                              "cả đánh quái, bình và buff để không gõ phím vào đó. Bấm Hủy Bỏ trong game: "
                              "GoBot thấy thùng rác không còn giữ gì thì tự chạy lại chúng."
                            : "Hộp thoại của thùng rác có thể vẫn đang mở trong game, nên GoBot dừng "
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
    // A Tự chat worker told to stop with a line under way looks at the line
    // it leaves, and may yet hand it over as maybe open: what started
    // meanwhile would type into it.
    if (chat_running_ && chat_quit_)
        return refuse("Đang dừng Tự chat. Chờ một chút rồi bấm lại.");
    // The program's one set of pictures, asked for by the first look that
    // needs them -- here, where a worker may set a doubt between two checks.
    const auto pictures = [this]() -> const TemplateStore& {
        if (!simple_store_) simple_store_ = TemplateStore::shared_best(exe_relative("configs/templates.ini"));
        return *simple_store_;
    };
    // Tự chat was stopped with the chat line maybe open, and no worker is
    // left to watch it: every key would go into it. Not until two looks a
    // frame apart show it shut, as the sender and watch_chat_doubt ask: one
    // frame has been seen showing the line shut while it was open. (One a
    // worker still watches holds everything off by itself.)
    if (chat_doubt_ && !chat_doubt_watched_) {
        std::string why;
        if (!chat_line_looks_shut(pictures(), 2, &why)) return refuse(why);
        chat_seen_shut();
    }
    if (!trash_left_held_) return true;

    // The last stop ended the trash worker's hold while the bin's dialog may
    // still have been open with an item in it, and that dialog's box takes
    // the macro's potion keys as numbers -- the hunter's keys go into it as
    // well. Not until the game is seen with no dialog up and nothing waiting
    // in the bin.
    std::string why;
    if (!bin_looks_clear(pictures(), &why)) {
        simple_note(why, 12000);
        if (!simple_view_) set_status(why, 12000);
        GB_WARN("Not started, as the bin's dialog may still be open: %s", why.c_str());
        return false;
    }
    bin_seen_clear();
    return true;
}

// The note for what resume_after_bin started, and only that (the review of
// 2026.11.21): the macro alone is the potions and the buffs, not the fight.
static const char* resumed_note(bool hunt, bool macro) {
    return hunt && macro ? "Thùng rác không còn giữ gì: đánh quái, bình và buff chạy lại."
           : hunt        ? "Thùng rác không còn giữ gì: đánh quái chạy lại."
                         : "Thùng rác không còn giữ gì: bình và buff chạy lại.";
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
        GB_INFO("Trash: the game shows nothing waiting in the bin any more");
        // The run still on: what the doubt stopped starts again by itself
        // (the audit's B9, 2026.11.21). Else BẮT ĐẦU -- and with something
        // else still running, the big button reads DỪNG LẠI, so the note
        // says to press it first.
        const int resumed = resume_after_bin();
        // And what the run owes (2026.11.24) -- a start this doubt refused,
        // the cold start's among them -- is asked in this tick (owed_tick),
        // not on its 30 s: it says for itself what started, or why not.
        const bool owed = owed_now();
        // Refused by its own start (a chat line in doubt, no character name,
        // no packets for the spot): that said why -- and the note, never
        // "chạy lại" for what did not start (the review of 2026.11.21).
        if (resumed < 0 || (resumed > 0 && (resumed & kResumeRefused))) return;
        // Nor "Bấm BẮT ĐẦU" over what GoBot is about to ask for itself.
        const std::string note =
            resumed > 0        ? resumed_note((resumed & kResumedHunt) != 0, (resumed & kResumedMacro) != 0)
            : owed             ? "Thùng rác không còn giữ gì: GoBot thử bật lại những việc chưa bật được."
            : simple_running() ? "Thùng rác không còn giữ gì. Bấm DỪNG LẠI rồi BẮT ĐẦU để đánh quái, bình và "
                                 "buff chạy lại."
                               : "Thùng rác không còn giữ gì. Bấm BẮT ĐẦU để đánh quái, bình và "
                                 "buff chạy lại.";
        simple_note(note, 12000);
        if (!simple_view_) set_status(note, 12000);
        return;
    }
    // Cleared by another look meanwhile -- a trash worker ticked on again
    // looks for itself (look_for_clear): what the doubt stopped, likewise --
    // and what is owed, asked in this tick (2026.11.24).
    if (!trash_left_held_ && (left_held_resume_hunt_ || left_held_resume_macro_)) {
        const int resumed = resume_after_bin();
        owed_now();
        if (resumed > 0 && !(resumed & kResumeRefused))
            simple_note(resumed_note((resumed & kResumedHunt) != 0, (resumed & kResumedMacro) != 0), 12000);
        return;
    }
    if (!trash_left_held_ || trash_running_) return;
    const std::uint64_t now = now_ms();
    if (now - left_held_looked_ms_ < 2000) return;
    left_held_looked_ms_ = now;
    left_held_look_gen_ = left_held_gen_;
    // One look at a time, so left_held_store_ is only ever the look's to set.
    left_held_look_ = std::async(std::launch::async, [this] {
        if (!left_held_store_) left_held_store_ = TemplateStore::shared_best(exe_relative("configs/templates.ini"));
        // Two looks a frame apart (2026.11.21): what it clears may now start
        // the potions and the hunter by itself (resume_after_bin), and one
        // frame has been seen to miss what was up -- the chat's own rule.
        if (!bin_looks_clear(*left_held_store_, nullptr)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return bin_looks_clear(*left_held_store_, nullptr);
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

bool GuiApp::stop_jobs() {
    // Everything that presses keys through the coordinator -- the hunter, the
    // macro, the jump key's "Thử" -- refused from this moment to the end. The
    // hunter and the macro cannot be told to stop without being waited for,
    // and while the others were waited for they went on swinging and
    // drinking. Not over a hold that is somebody else's, which refuses them
    // just the same -- and which, let go of by its owner as it winds down, is
    // the stop's from then on (end_hold). Before anything, a bag job's key
    // window shut and forgotten: under the job's hold take_hold(kStopHold)
    // below does nothing, and the macro's letter keys would go on through it
    // (2026.11.10).
    coord_.disarm_pass_keys();
    {
        std::lock_guard<std::mutex> lock(hold_mutex_);
        stopping_ = true;
    }
    // Nothing the bin's doubt stopped comes back by itself after a stop
    // (resume_after_bin, 2026.11.21): BẮT ĐẦU starts it all.
    left_held_resume_hunt_ = false;
    left_held_resume_macro_ = false;
    // Nor anything owed (2026.11.24): owed_tick never starts again what the
    // player stopped -- DỪNG, F12, closing GoBot all come through here.
    hunt_owed_ = false;
    macro_owed_ = false;
    jobs_owed_ = false;
    take_hold(kStopHold);

    // ---- every one told, and nothing waited for ----
    stop_chat(true, false);
    stop_party(false);
    stop_revive(false);
    stop_trash(false);
    stop_quest(false);
    stop_expedition(false);
    stop_popups(false);
    stop_keeper(false);
    simple_style_stop();
    input_.release_all();

    // ---- then each one waited for: the engines first ----
    stop_hunt();
    stop_macro();
    stop_party();
    stop_revive();
    const bool bin_note = stop_trash();
    stop_quest();
    stop_expedition();
    stop_popups();
    stop_keeper();

    // What went down while they wound down -- a press under way when they
    // were told -- let go of as well.
    input_.release_all();
    // The stop's hold ends. One its owner keeps beyond the stop is left on: a
    // chat line in doubt handed over to tick(). (The revive's, out of tries,
    // was one until 2026.11.24, when stop_after_revive ended it; a revive no
    // longer stops anything, and its worker lets go of its hold as it stops.)
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
    // The bag's jobs -- trash, charms, the shop -- run on this thread, between
    // fights: they can wait for the game's slice to end.
    quiet_this_thread();
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

    // The program's one set of pictures, held while this runs (2026-11-16).
    // None loaded: a run no longer ends here (2026.11.24, the owner: "Kiểm tra
    // các phần khác xem có phần nào không làm được thì dừng không, cần thay cơ
    // chế này bằng 1 cơ chế khác") -- it waits, pressing nothing, and looks
    // for them again every 30 s (wait_for_pictures); its stop ends the wait at
    // once. A one-off pass (Dọn thử and the tabs' other buttons) still ends at
    // once with the line: the player pressed it to see it now, and the tab's
    // buttons stay greyed while a worker is here.
    std::shared_ptr<const TemplateStore> pictures =
        TemplateStore::shared_best(exe_relative("configs/templates.ini"));
    if (pictures->size() == 0 && trash_oneshot_) {
        finish("Không có hình giao diện của game, nên không nhận ra được túi đồ.");
        return;
    }
    // A button pressed meanwhile -- Dọn ngay, Thử tìm, Dùng bùa ngay, Mua ngay,
    // Thử mua, the hotkey -- is answered at once, not kept to be acted on
    // whenever the pictures come back (the 2026.11.24 review).
    std::string trash_missing = "hình giao diện";
    if (!wait_for_pictures(
            pictures, exe_relative("configs/templates.ini"), "Trash",
            [](const TemplateStore& s) { return s.size() == 0 ? std::string("hình giao diện") : std::string(); },
            [&] {
                if (trash_request_.exchange(0) != 0) tell(pictures_again_vi(trash_missing));
                return trash_stop_.load();
            },
            [&](const std::string& what) {
                trash_missing = what;
                tell(pictures_again_vi(what));
            })) {
        trash_running_ = false;
        return;
    }
    const TemplateStore& store = *pictures;
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
    // The kept hold's way out (2026.11.21, the audit's B1; bag::kept_sweep_due):
    // since when this worker has kept it, since when the check-in board has
    // been seen over it (look_for_clear), and its last sweep -- 0 for none.
    std::uint64_t kept_ms = 0, kept_board_ms = 0, kept_swept_ms = 0;
    // The same from the start when the game already shows the bin asking, or
    // something waiting in it -- left by a pass a stop cut short, or by hand:
    // held off now, not from the first timed pass an interval on.
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
    // doubt, which kept every key from starting with the worker gone (see
    // stop_trash), once the bin's dialogs can be told from the rest,
    // as keys_may_start asks. That stop stopped the fighting and the potion
    // keys as well, and those wait for BẮT ĐẦU.
    const auto look_for_clear = [&] {
        held_looked_ms = now_ms();
        Frame f;
        if (!grab(f) || !f.valid()) return;
        // The check-in board over a kept hold, and since when (2026.11.21, the
        // audit's B1): the kept hold's own sweep waits kReactMs for it, as the
        // popup closer does, and for its night hold to be over.
        const bool board = held && checkin_board_title(f, store);
        if (!board) kept_board_ms = 0;
        else if (kept_board_ms == 0) kept_board_ms = now_ms();
        if (!bag::nothing_held(f, store, settings.layout)) return;
        // Nor under the check-in board: a quantity box taken to be up when it
        // came over it -- 100 typed, not seen gone -- is out of sight of both
        // looks above. Held until the board is gone and a look shows nothing.
        if (shop_held && (bag::shop_left_up(f, store) || board)) return;
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
    // ---- the kept hold's way out ----
    // (2026.11.21, the audit's B1; the owner: "không để các chức năng block
    // nhau") A hold kept for something left up -- a shop round's quantity box
    // the check-in board came over at 00:00, a dialog nobody answers -- was
    // ended only by a look that showed the game clear, and nothing cleared it:
    // the popup closer does not act under this hold, the kept pass stops at
    // the board, and every potion, skill and the fighting waited for a person
    // to press ESC. Now this worker sweeps with ESC itself, under its own hold
    // and the game's mouse (BagCleaner::sweep_kept, the owner's stuck rule of
    // 2026.11.14b) -- when bag::kept_sweep_due says: kept kReactMs, the board
    // up kReactMs and past its night hold (00:03), a minute between sweeps --
    // and then looks again: the hold goes only on a clean look, as ever.
    // Never with a key window armed: no key of the macro's, a digit least of
    // all, goes while what was typed into a box may still be up.
    const auto kept_sweep = [&] {
        kept_swept_ms = now_ms();
        // Only with the character in the world, as the shop asks: no ESC on
        // the login or character screen, a map loading, a keeper's crossing.
        if (!input_.window_valid() || !client_on_screen(input_.target_window()) || keeper_cold_start_ ||
            hp_percent_now() < 0.0f)
            return;
        {
            std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
            if (keeper_flow_ != nullptr) return;
        }
        std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::defer_lock);
        const std::uint64_t give_up = now_ms() + 2'000;
        while (!trash_stop_ && now_ms() < give_up && !mouse.try_lock())
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!mouse.owns_lock() || trash_stop_ || chat_doubt_) return;
        if (!(coord_.critical_active() && coord_.critical_reason() == "trash cleaning")) return;
        GB_INFO("Trash: the hold kept %llu s for something left up%s -- sweeping it with ESC",
                static_cast<unsigned long long>((now_ms() - kept_ms) / 1000),
                kept_board_ms != 0 ? ", the check-in board over it" : "");
        trash_pass_ = true;
        for (int i = 0; i < 30 && coord_.held(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        input_.release_all();
        ScreenCapture sweep_capture;
        sweep_capture.set_screen_only(true);
        sweep_capture.set_label("kept");
        BagCleaner cleaner(input_, store,
                           [&](Frame& f) { return sweep_capture.grab(input_.target_window(), f); });
        cleaner.set_settings(settings);
        cleaner.set_counts(&counts);
        cleaner.set_debug_dir(exe_relative("trash_debug"));
        // A shop's hold: the shop's sweep, ESC on the box too; else a pass's,
        // the bin's question its Hủy Bỏ first.
        cleaner.set_mode(shop_held ? BagCleaner::Mode::ShopBuy : BagCleaner::Mode::Pass);
        // The hold this worker's, and the character not down -- the death
        // panel up, which no ESC may touch: esc_out asks it on every look.
        cleaner.set_guard([this] {
            if (!(coord_.critical_active() && coord_.critical_reason() == "trash cleaning")) return false;
            const float hp = hp_percent_now();
            return !(hp >= 0.0f && hp < static_cast<float>(bag::kJobDownHpPct));
        });
        cleaner.on_report([this](const std::string& line) { say_trash(line); });
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = &cleaner;
        }
        if (trash_stop_) cleaner.cancel();
        BagCleaner::Result r;
        bool clear = false;
        {
            const PrintWindowPause no_print;
            clear = cleaner.sweep_kept(r, "a kept hold");
        }
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = nullptr;
        }
        trash_pass_ = false;
        GB_INFO("Trash: the kept hold's sweep -- %d ESC(s), %s", r.esc_presses,
                clear ? "nothing up after it" : "something still up");
        look_for_clear();
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
    // Whether the job's own last press of `vk` is still within `cooldown_ms`
    // -- the line's, or the urgent one far under the mark: asked by
    // MacroEngine::own_potion_due under its lock, so the map alone, never the
    // engine. A key cooling lets the other kind be looked at (the 2026.11.12b
    // review: an MP potion due went undrunk while HP's cooled).
    const auto own_cooling = [&own_potion_ms](std::uint32_t vk, std::uint32_t cooldown_ms) {
        const auto at = own_potion_ms.find(vk);
        return at != own_potion_ms.end() && now_ms() < at->second + cooldown_ms;
    };
    const auto own_potions = [&] {
        BagCleaner::OwnPotions o;
        auto refused_said = std::make_shared<bool>(false);
        o.due = [this, never = job_never(), refused_said, own_cooling](std::string& line) -> std::uint32_t {
            if (!macro_ || macro_->running()) return 0;
            const MacroEngine::OwnPotion p = macro_->own_potion_due(never, own_cooling);
            if (p.refused != 0 && !*refused_said) {
                *refused_said = true;
                GB_INFO("Trash: a potion due on key %s is not pressed by a bag job -- letters, Space and "
                        "Home/End/PgUp/PgDn/Insert/Delete only, never a digit",
                        InputManager::key_name(p.refused).c_str());
            }
            if (p.vk == 0) return 0;
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
    // ---- the macro's HP potion a timed bag job cannot let through ----
    // (2026.11.21, the audit's B2; the owner: "không để các chức năng block
    // nhau"; bag::drink_yield) The jobs go at any HP (2026.11.11b, 2026.11.12b),
    // and the potion lines the simple screen seeds sit on 1 and 2 -- digits,
    // which no job lets through (the owner: "Giữ chặn phím số") -- so with the
    // macro running nothing drank while a pass, a shop round or a charm run
    // held the game, and its guard stopped it only with the character down.
    // Now, once the job has held the game kDrinkYieldAfterMs, the HP potion due
    // on a key it neither lets through (the pass's and the shop's window,
    // `keys`) nor presses itself (a charm run: a letter, Space or one of the six
    // keys, not in `never`) has the job give the hold back between two items,
    // purchases or charms (BagCleaner::set_yield), the macro drinks, and the job
    // is tried again kHpStoppedAgainMs on. No HP mark holds a job back: only the
    // potion that is due and cannot go. Never a job begun with something maybe
    // left in the bin (`kept`): it keeps the hold anyway. `yield_from_ms` is set
    // as each job's run begins and cleared as it ends.
    std::uint64_t yield_from_ms = 0;
    const auto drink_yield_of = [&](PassKeys keys, bool charm_run, bool kept, std::vector<std::uint32_t> never) {
        return [this, &yield_from_ms, keys = std::move(keys), charm_run, kept, never = std::move(never)] {
            if (yield_from_ms == 0 || !macro_ || !macro_->running()) return false;
            const std::uint32_t vk = macro_->hp_potion_due();
            const bool goes = vk != 0 && (charm_run ? pass_key_allowed(vk) &&
                                                          std::find(never.begin(), never.end(), vk) == never.end()
                                                    : keys.has(vk));
            const std::uint64_t now = now_ms();
            return bag::drink_yield(vk != 0 && !goes, kept, now > yield_from_ms ? now - yield_from_ms : 0);
        };
    };
    // ---- the HP and MP potions a charm run presses itself ----
    // (2026.11.12b, the owner: "máu thấp thì không dùng bùa nữa, cái này cũng
    // cần sửa"): the charms at any HP, as the pass and the shop. A charm run
    // lets no key window through -- "lúc dùng bùa thì không cần ra skill" --
    // so with the macro running too nothing else drinks while it holds the
    // game: the run presses the potion due itself, by own_potions' rules (the
    // tier for the reading, never a digit nor a key of the never-list, the
    // urgent cooldown far under the mark, a key cooling letting the other
    // kind go), and with the macro running only once the macro's own cooldown
    // for that key is over as well (own_potion_due's `macro_cooled`) -- a
    // press of the macro's just before the hold counted. The cooldown of the
    // job's own presses is own_potions'.
    const auto charm_potions = [&] {
        BagCleaner::OwnPotions o;
        auto refused_said = std::make_shared<bool>(false);
        o.due = [this, never = job_never(), refused_said, own_cooling](std::string& line) -> std::uint32_t {
            if (!macro_) return 0;
            const MacroEngine::OwnPotion p = macro_->own_potion_due(never, own_cooling, macro_->running());
            if (p.refused != 0 && !*refused_said) {
                *refused_said = true;
                GB_INFO("Trash: a potion due on key %s is not pressed by a charm run -- letters, Space and "
                        "Home/End/PgUp/PgDn/Insert/Delete only, never a digit",
                        InputManager::key_name(p.refused).c_str());
            }
            if (p.vk == 0) return 0;
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
    // The charms', the pass's and the shop's clocks as the last worker left
    // them: a MACRO OFF and ON in between starts a new worker, and each start
    // used to bring the charm check 8 s on and the shop's look 45 s on whatever
    // was due -- the ten minutes after a tab not read, the shop's wait after a
    // spoiled quiet second. Kept, but never sooner than a start's own wait; and
    // from that wait after its box is ticked on again, the worker gone
    // meanwhile. The pass's and the shop's since 2026.11.12 only while still
    // counting, else a whole interval from the start (start_counting, below).
    TrashClocks kept;
    {
        std::lock_guard<std::mutex> lock(trash_clocks_mutex_);
        kept = trash_clocks_;
    }
    unsigned charm_ticks = charm_ticked_.load(), shop_ticks = shop_ticked_.load(), pass_ticks = pass_ticked_.load();
    if (kept.charm_ticks_seen != charm_ticks) kept.charm_check_ms = 0;
    if (kept.pass_ticks_seen != pass_ticks) kept.pass_due_ms = 0;
    if (kept.shop_ticks_seen != shop_ticks) {
        kept.shop_due_ms = 0;
        kept.shop_quiet_level = 0;
        // Ticked on again: nothing owed, a whole interval to the first look.
        kept.owed_hp = kept.owed_mp = 0;
        kept.plan_due_ms = 0;
        kept.shop_troubles = 0;
        kept.look_owed = false;
    }
    std::uint64_t charm_check_ms = resumed_clock(kept.charm_check_ms, now_ms(), kCharmFirstMs);
    // The timed pass's clock and the shop's, counting once this worker is
    // here for a run (start_counting, below); 0 until then.
    std::uint64_t next = 0;
    std::uint64_t shop_due_ms = 0;
    bool counting = false;
    bool pass_was_on = settings.enabled;
    // A message of the game's after a charm's Xác Nhận that the last run left
    // up, or may have (BagCleaner::Result::notice_left): its question's pair,
    // and when it was left -- 0 for none. The next run closes it, for two
    // minutes; after that a message there is anyone's, and left alone. A run
    // that clicked it and left it up (Result::notice_clicked) counts them
    // anew (2026.11.24): one two clicks did not close is handed over too, and
    // clicked again kCharmNoticeAgainMs on, for as long as it takes.
    ScreenRect charm_notice_place;
    std::uint64_t charm_notice_ms = 0;
    // The last run clicked it and it stayed up (the 2026.11.24 review): no
    // two minutes' end to it -- a run put off past them (a shop round, a
    // pass, the boss's quiet) left it "anyone's" for good -- and the next run
    // makes one quick click (set_own_notice's clicked_out).
    bool charm_notice_clicked = false;
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
        if (charm_notice_ms != 0 && !charm_notice_clicked && now_ms() >= charm_notice_ms + 120'000)
            charm_notice_ms = 0;
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
        // At any HP (2026.11.12b, the owner: "máu thấp thì không dùng bùa nữa,
        // cái này cũng cần sửa"), as the pass and the shop since 2026.11.11b:
        // the run presses the HP and MP potions itself between two charms
        // (charm_potions), and only the character down -- HP read under
        // bag::kJobDownHpPct -- holds it back. Until 2026.11.12 it waited for
        // HP 10 over the HP potion's mark, which a fight hardly ever gave.
        {
            const float hp = hp_percent_now();
            if (hp < 0.0f) return not_now("Chưa đọc được máu của nhân vật, dùng bùa sau.");
            if (hp < static_cast<float>(bag::kJobDownHpPct)) return not_now("Nhân vật đã gục, dùng bùa sau.");
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
        // hold is taken from it, it stops there. And the character not down
        // (2026.11.12b): HP low no longer stops it -- the run drinks itself.
        cleaner.set_guard([this] {
            if (!(coord_.critical_active() && coord_.critical_reason() == "trash cleaning")) return false;
            const float hp = hp_percent_now();
            return !(hp >= 0.0f && hp < static_cast<float>(bag::kJobDownHpPct));
        });
        // The HP and MP potions, pressed by the run between two charms
        // (2026.11.12b): never with something maybe left in the bin, as a
        // pass's own.
        if (!held && !trash_left_held_) cleaner.set_own_potions(charm_potions());
        // The macro's HP potion on a key the run does not press itself -- a
        // digit -- gives way between two charms (2026.11.21, the audit's B2;
        // drink_yield_of): a timed run, not "Dùng bùa ngay".
        if (!forced) cleaner.set_yield(drink_yield_of(PassKeys{}, true, held || trash_left_held_, job_never()));
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
        if (notice_kept) cleaner.set_own_notice(charm_notice_place, charm_notice_clicked);
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
                // and its pages: 20 to 40 s -- moved on by the cleaner itself by
                // whatever the game took past 2 s to react to an input, up to
                // its 30 s each on a slow PC (2026.11.15d, react_took).
                cleaner.set_deadline_ms(BagCleaner::charm_deadline_ms(due_kinds));
                yield_from_ms = now_ms();
                r = cleaner.run();
                yield_from_ms = 0;
                held = r.keeps_hold(held);
                if (!held) shop_held = false;       // 2026.11.21, B1
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
        // that only found it still there -- but renewed by one that clicked it
        // (2026.11.24, Result::notice_clicked): two clicks that did not close
        // it no longer leave it to the player, and the message stays GoBot's
        // while GoBot keeps clicking it, about once a minute (below).
        if (!r.notice_left) {
            charm_notice_ms = 0;
            charm_notice_clicked = false;
        } else if (!r.notice_carried || charm_notice_ms == 0 || r.notice_clicked) {
            charm_notice_place = r.notice_place;
            charm_notice_ms = now_ms();
            charm_notice_clicked = r.notice_clicked;
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
            // was looked for, and the run waits its own minute below
            // (kCharmTabRetryMs; as long as this until 2026.11.24).
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
        // sweep: a minute, as any run that stopped on trouble (2026.11.24; ten
        // minutes from 2026.11.08).
        if (!forced && r.tab_unsure) charm_check_ms = now_ms() + kCharmTabRetryMs;
        // Given way for the macro's HP potion (2026.11.21, B2): again in
        // kHpStoppedAgainMs, as a pass or a shop round its HP stopped.
        if (r.yielded && !trash_stop_) charm_check_ms = now_ms() + bag::kHpStoppedAgainMs;
        // GoBot's own message clicked and still up (2026.11.24): clicked again
        // by the next run kCharmNoticeAgainMs on -- a button's run too, so the
        // timed one does not click it again at once -- without limit. No
        // charm is right-clicked while it is up; the rest of the bot goes on.
        if (r.notice_left && r.notice_clicked && !trash_stop_) charm_check_ms = now_ms() + kCharmNoticeAgainMs;
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
                                     (forced ? "." : "; thử lại sau 1 phút.");
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
    // before anything is clicked (2026.11.09), on this thread only. Until
    // 2026.11.12 a worker started with no clock of the last one's waited what
    // was left of it, 45 s at least; now it counts a whole interval from its
    // start (bag::start_due_ms), never sooner than the plan the file holds, so
    // the file is no longer read here -- a GoBot of before, started on this
    // Windows account, still reads it, and never buys early by it.
    CharmClock shop_clock(shop_clock_path());
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
    // When the next plan begins, for the log: by its time, or -- buying
    // after each pass (2026.11.13b) -- with the next timed pass.
    const auto next_plan_said = [&] {
        return bag::buys_after_pass(settings) ? std::string("after the next timed pass") : clock_at(plan_due_ms);
    };
    // The card's line (simple_shop.cpp), from atomics alone: the next look and
    // the next plan in wall-clock seconds, and what is owed -- set again only
    // when one of them moved. A worker here for one button's job does no timed
    // round: no times then.
    std::uint64_t shown_due = 0, shown_plan = 0;
    int shown_owed = -1;
    bool shown_timed = false, shown_after = false;
    const auto show_shop = [&] {
        const int left = owed.hp + owed.mp;
        const bool timed_worker = !trash_oneshot_.load();
        // Buying after each pass (2026.11.13b): no plan time to show -- the
        // card says "after each pass" -- only a part still owed and its look.
        const bool after = bag::buys_after_pass(settings);
        if (shown_due == shop_due_ms && shown_plan == plan_due_ms && shown_owed == left &&
            shown_timed == timed_worker && shown_after == after)
            return;
        shown_due = shop_due_ms;
        shown_plan = plan_due_ms;
        shown_owed = left;
        shown_timed = timed_worker;
        shown_after = after;
        const std::uint64_t now = now_ms();
        const auto wall_of = [&](std::uint64_t ms) {
            return wall_s() + static_cast<std::int64_t>(ms > now ? (ms - now + 999) / 1000 : 0);
        };
        shop_owed_ = left;
        shop_look_at_s_ = timed_worker && (!after || left > 0) ? wall_of(shop_due_ms) : std::int64_t{ 0 };
        // Nothing owed: the next look begins the next plan.
        shop_plan_at_s_ = !timed_worker || after       ? std::int64_t{ 0 }
                          : left > 0 && plan_due_ms != 0 ? wall_of(plan_due_ms)
                                                         : wall_of(shop_due_ms);
    };
    // ---- counting from the start (2026.11.12) ----
    // The owner: "khi bật thì bắt đầu đếm thời gian. Lúc nào tới thời gian thì
    // mới bắt đầu thực hiện". A worker here for a run -- BẮT ĐẦU, F10, a box
    // ticked on with the bot running -- or one here for a button that became
    // one counts from then: the first timed pass interval_minutes on and the
    // shop's first look buy_every_minutes on, but for a clock the last worker
    // left still counting, kept as it was -- a quick MACRO OFF and ON -- never
    // sooner than kPassFirstMs and kShopFirstMs (bag::start_due_ms). A worker
    // here for a button alone counts neither: the button's job goes at once,
    // and the two clocks are left for the next start as it found them. The
    // charms keep their own start, kCharmFirstMs. But for what the bin may
    // hold as it counts -- found as this worker began, or left by a stop --
    // which holds every key off, the fighting and the potions too, until a
    // pass gives it back: that pass kPassFirstMs on, as every start had it,
    // not an interval.
    const auto start_counting = [&] {
        counting = true;
        const std::uint64_t now = now_ms();
        const bool pass_kept = kept.pass_due_ms > now, shop_kept = kept.shop_due_ms > now;
        next = bag::start_due_ms(kept.pass_due_ms, now, settings.interval_minutes, kPassFirstMs);
        shop_due_ms = bag::start_due_ms(kept.shop_due_ms, now, settings.buy_every_minutes, kShopFirstMs);
        const bool give_back = (held || trash_left_held_) && next > now + kPassFirstMs;
        if (give_back) next = now + kPassFirstMs;
        const auto said = [&](std::uint64_t due, bool was_kept, int every, bool on) {
            return clock_at(due) +
                   (was_kept ? std::string(" (kept from the last run)")
                             : " (" + std::to_string(every) + " min from the start)") +
                   (on ? "" : ", its box off");
        };
        GB_INFO("Trash: counting from the start -- the first timed pass at %s, the shop's first look at %s",
                give_back ? (clock_at(next) + " (to give back what the bin may hold, every key held)").c_str()
                          : said(next, pass_kept, settings.interval_minutes, settings.enabled).c_str(),
                said(shop_due_ms, shop_kept, settings.buy_every_minutes, settings.buy_potions).c_str());
    };
    // "Tự dọn rác" or "Tự mua bình" ticked on again: that job counts afresh,
    // a whole interval from now -- or, in a worker not counting, from the next
    // start, the clock the last worker left forgotten.
    const auto pass_afresh = [&] {
        if (counting) next = bag::start_due_ms(0, now_ms(), settings.interval_minutes, kPassFirstMs);
        else kept.pass_due_ms = 0;
    };
    const auto shop_afresh = [&] {
        if (counting) shop_due_ms = bag::start_due_ms(0, now_ms(), settings.buy_every_minutes, kShopFirstMs);
        else kept.shop_due_ms = 0;
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
    // ---- "Mua ngay sau khi dọn rác" (2026.11.13b) ----
    // Users, via the owner: "không cần thiết định thời gian mua HP/MP nữa, sẽ
    // ngay lập tức thực hiện mua sau khi dọn rác xong". With the box ticked
    // and Tự dọn rác on (bag::buys_after_pass) every timed pass that ran owes
    // the shop a look (`look_owed`): ridden at once as above, or -- the ride
    // refused, the round put off by a gate -- the shop's own, by its rules,
    // until a plan begins. The clock begins no plan of its own then
    // (bag::shop_clock_looks, bag::shop_plan_begins); a plan's part still
    // owed is bought on as before. The box ticked off, or Tự dọn rác off: the
    // clock counts a whole interval from then.
    // Kept across workers (the review): a look a pass left owed survives a
    // MACRO OFF and ON; the mode as this worker finds it, so that look is not
    // dropped as the mode "comes".
    bool look_owed = kept.look_owed;
    bool after_was = bag::buys_after_pass(settings);
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
    // by the job's own key window (bag::hp_gates). Since 2026.11.11b ("mua bình
    // ở bất cứ mức máu nào", "Cả dọn rác") both gates are bag::kJobDownHpPct
    // whatever the window lets through: only the character down holds a job
    // back or stops it -- the margin over the floor for a potion on a digit is
    // gone. Such a potion, which the window never lets through, has the timed
    // job give way for it instead (2026.11.21, drink_yield_of). chat_hp_floor_
    // stays what chat and the other jobs go by.
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
    // ---- the pet's HP food on reaching TổCổLong (2026.11.24) ----
    // The owner: "Khi ra tới bản đồ TổCổLong thì cần mua lại HP cho pet. Cái
    // này ở trong tab Linh Thú của người giữ sân và là item "Thịtbò" => cần có
    // lựa chọn để mua và mua bao nhiêu slot tương tự như mua HP MP". For each
    // arrival PetRestock owes (pet_arrival_tick), one round of buy_pet_slots
    // purchases of 100 of the pet's Thịt bò from the same shop -- its Linh Thú
    // tab -- under the same hold, key window and HP rules as a potion round
    // (BagCleaner::Mode::ShopBuy with a plan of the pet's food alone): nothing
    // else bought, HP and MP left to their own clock. As soon as it may, in the
    // owner's order (a Tự chat line, a timed pass and the charms due first),
    // before a timed potion round. What puts it off -- HP not read, another
    // job's hold, somebody at the mouse, the check-in board -- has it tried
    // again a few seconds on (a minute for the board); trouble, kAgainMs (the
    // owner's 30 s for the shop); bought, or the game's no to it (its bag
    // full, a message), it is over; and past PetRestock::kWaitMostMs from the
    // arrival it is given up until the next one, the dungeon that waited for
    // it going on. Nothing of it is ever a stop of the bot or of a feature.
    std::uint32_t pet_entry = 0;        // the arrival the purchases below were for
    int pet_bought = 0;                 // of its slots: bought, or perhaps (unsure, a notice)
    const auto run_pet = [&](std::uint32_t entry) {
        if (entry != pet_entry) {
            pet_entry = entry;
            pet_bought = 0;
        }
        // Tried again `ms` on, while the arrival's time lasts; why in the log.
        const auto later = [&](std::uint64_t ms, const std::string& why) {
            pet_restock_.put_off(entry, now_ms() + ms);
            GB_INFO("Pet: the round put off %llu s -- %s", static_cast<unsigned long long>(ms / 1000), why.c_str());
        };
        for (int i = 0; i < 5; ++i) {
            CleanSettings fresh;
            if (fresh.load(config)) {
                settings = fresh;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        read_ms = now_ms();
        const int left = settings.buy_pet_slots - pet_bought;
        if (!settings.buy_pet || left <= 0) {
            pet_restock_.over(entry);
            GB_INFO("Pet: nothing to buy (%s) -- the round over", settings.buy_pet ? "its slots bought" : "buy_pet off");
            return;
        }
        if (!bag::can_check_shop(store) || !counts.loaded()) {
            pet_restock_.over(entry);
            tell("Mua HP pet: thiếu hình hoặc bộ chữ số của game để mua (xem thẻ Dọn rác); thử lại ở lần ra TổCổLong "
                 "sau.");
            return;
        }
        if (!input_.window_valid()) return later(5'000, "no game window");
        if (!client_on_screen(input_.target_window())) return later(5'000, "the game not whole on the screen");
        if (chat_doubt_) return later(5'000, "the chat line may be open");
        if (coord_.critical_active() && coord_.critical_reason() != "trash cleaning")
            return later(coord_.critical_reason() == kChatHold ? kAfterChatLineMs : 5'000,
                         "another job holds the game (" + coord_.critical_reason() + ")");
        if (keeper_cold_start_) return later(5'000, "GoBot is logging in again");
        {
            std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
            if (keeper_flow_ != nullptr) return later(5'000, "GoBot is on another screen of the game");
        }
        // HP as a potion round has it (bag::hp_gates): only the character down
        // holds it back; HP not read is no world to buy in.
        const PassKeys pass_keys = held || trash_left_held_ ? PassKeys{} : pass_keys_now();
        const bag::HpGates gate = hp_gates_of(pass_keys);
        job_level = gate.stop;
        const float hp_now = hp_percent_now();
        if (hp_now < 0.0f) return later(5'000, "HP not read (not in the world)");
        if (hp_now < static_cast<float>(gate.start)) return later(bag::kHpLookAgainMs, "the character down");
        // The Người Giữ Sân item by the catalog's icon, the potions' to tell it
        // from, as a potion round has them: nothing here is ever deleted.
        const std::vector<std::string> icons{ CleanSettings::kBuyHpId, CleanSettings::kBuyMpId, settings.buy_opener };
        TrashList list;
        bool icons_ok = true;
        for (const std::string& id : icons) icons_ok = icons_ok && catalog.find(id) != nullptr;
        if (icons_ok) {
            list.add_catalog(catalog, icons, TrashList::Action::Delete, settings.catalog_score);
            list.add_rivals(catalog);
        }
        if (!icons_ok || list.items.size() < 3) {
            pet_restock_.over(entry);
            tell("Mua HP pet: không có hình món Người Giữ Sân (" + settings.buy_opener +
                 ") trong bộ hình của game; thử lại ở lần ra TổCổLong sau.");
            return;
        }
        ScreenCapture pet_capture;
        pet_capture.set_screen_only(true);
        pet_capture.set_label("pet");
        BagCleaner cleaner(input_, store, [&](Frame& f) { return pet_capture.grab(input_.target_window(), f); });
        cleaner.set_settings(settings);
        cleaner.set_trash(&list);
        cleaner.set_counts(&counts);
        cleaner.set_debug_dir(exe_relative("trash_debug"));
        cleaner.set_mode(BagCleaner::Mode::ShopBuy);
        // The pet's food alone: what is left of its slots for this arrival,
        // kShopMaxBuys at most in the one hold.
        const int plan = std::min(left, BagCleaner::kShopMaxBuys);
        cleaner.set_shop_plan(0, 0, plan);
        // The hold this job's, and the character not down; the full-bag
        // message and the game's other messages closed whatever HP is.
        cleaner.set_guard([this, &hp_cut, stop_level = gate.stop] {
            const bool ours = coord_.critical_active() && coord_.critical_reason() == "trash cleaning";
            const float now_hp = hp_percent_now();
            const bool low = now_hp >= 0.0f && now_hp < static_cast<float>(stop_level);
            if (ours && low) hp_cut = HpCut{ now_hp, stop_level };
            return ours && !low;
        });
        cleaner.set_hold_check(
            [this] { return coord_.critical_active() && coord_.critical_reason() == "trash cleaning"; });
        cleaner.on_report([this](const std::string& line) { say_trash(line); });
        // The macro's letters and Space between the purchases, as a potion
        // round lets them; else the potions pressed by the round itself.
        if (pass_keys.any()) cleaner.set_key_window(key_window());
        else if (!held && !trash_left_held_) cleaner.set_own_potions(own_potions());
        // The macro's HP potion on a digit gives way before a double-click, as
        // for a timed potion round.
        cleaner.set_yield(drink_yield_of(pass_keys, false, held || trash_left_held_, {}));
        {
            std::lock_guard<std::mutex> lock(trash_cleaner_mutex_);
            trash_cleaner_ = &cleaner;
        }
        if (trash_stop_) cleaner.cancel();
        const auto last_input = [] {
            LASTINPUTINFO info{};
            info.cbSize = sizeof info;
            return GetLastInputInfo(&info) ? info.dwTime : 0;
        };
        BagCleaner::Result r;
        bool passed = false, touched = false, lost = false, own = false;
        {
            // The mouse, as a potion round takes it, a little at a time; the
            // hold; and a second with nobody at the mouse or keyboard --
            // looked for a few times, as a button's round does: the bot runs,
            // and the round has the arrival's time only.
            std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::defer_lock);
            const std::uint64_t give_up = now_ms() + 5'000;
            while (!trash_stop_ && now_ms() < give_up && !mouse.try_lock())
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (mouse.owns_lock() && !trash_stop_ && !chat_doubt_ && take_hold("trash cleaning")) {
                trash_pass_ = true;
                coord_.disarm_pass_keys();
                for (int i = 0; i < 30 && coord_.held(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                input_.release_all();
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                int own_seen = 0;
                for (int t = 0; t < kShopQuietTries && !trash_stop_ && !lost; ++t) {
                    const std::uint64_t own_from = InputManager::sent_count();
                    const DWORD quiet_from = last_input();
                    for (int slept = 0; slept < kShopQuietMs && !trash_stop_ && !lost; slept += 50) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        lost = !(coord_.critical_active() && coord_.critical_reason() == "trash cleaning");
                    }
                    const QuietSaid said = quiet_said(quiet_from, last_input(), own_from, InputManager::sent_count());
                    own = !trash_stop_ && !lost && said == QuietSaid::Own;
                    if (own && own_seen++ < kShopOwnRelooks) {
                        --t;
                        continue;
                    }
                    touched = !trash_stop_ && !lost && said == QuietSaid::Touched;
                    if (!touched && !own) break;
                }
                if (touched || own || trash_stop_ || lost) {
                    if (!held) end_hold("trash cleaning");
                    trash_pass_ = false;
                } else {
                    passed = true;
                    // A job runs: a timed job's put-off said again when it
                    // comes, as after any other.
                    put_off_why.clear();
                    pull_said = false;
                    tell("Mua HP pet (" + std::string(CleanSettings::kBuyPetShown) + ", ngăn Linh Thú của cửa hàng)...");
                    if (!input_.window_focused()) {
                        input_.focus_window();
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    }
                    // The tab's switch and its looks on top of a potion
                    // round's 30 s and 6 s a purchase.
                    cleaner.set_deadline_ms(BagCleaner::shop_deadline_ms(plan) + 30'000);
                    {
                        const PrintWindowPause no_print;
                        const PassKeysScope keys(coord_, pass_keys, "pet");
                        hp_cut = HpCut{};
                        yield_from_ms = now_ms();
                        r = cleaner.run();
                        yield_from_ms = 0;
                    }
                    say_stopped("pet round", hp_cut, r.trouble);
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
            if (trash_stop_) return;
            if (touched) return later(5'000, "someone is using the mouse or keyboard (the quiet second)");
            if (own) return later(5'000, "GoBot's own input in each quiet second");
            return later(lost ? 5'000 : 2'000, "the hold not had (" +
                                                   (coord_.critical_active() ? coord_.critical_reason()
                                                                             : std::string("the mouse in use")) +
                                                   ")");
        }
        // What came of it.
        pet_bought += r.shop_bought_pet + r.shop_unsure_pet;
        if (!r.board_up) board_said = false;
        const bool hp_stop = !trash_stop_ && (hp_cut.level > 0 || r.yielded);
        const bool cut = !trash_stop_ && !(coord_.critical_active() && coord_.critical_reason() == "trash cleaning");
        GB_INFO("Pet: the round -- %d bought, %d unsure, %d of %d for this arrival%s%s%s", r.shop_bought_pet,
                r.shop_unsure_pet, pet_bought, settings.buy_pet_slots, r.pet_ended ? ", the game said no" : "",
                r.board_up ? ", the check-in board" : "", r.trouble.empty() ? "" : (", " + r.trouble).c_str());
        std::string line;
        if (r.shop_bought_pet + r.shop_unsure_pet > 0)
            line = "Mua HP pet: đã mua " + std::to_string(r.shop_bought_pet + r.shop_unsure_pet) + " lần " +
                   std::to_string(CleanSettings::kBuyQuantity) + " " + CleanSettings::kBuyPetShown + ".";
        const auto add = [&line](const std::string& more) { line += (line.empty() ? "" : " ") + more; };
        if (r.pet_ended || pet_bought >= settings.buy_pet_slots) {
            // Bought, or the game's no: over until the next arrival.
            pet_restock_.over(entry);
            if (r.pet_ended)
                add("Mua HP pet: game báo không mua thêm được (túi đồ [Linh Thú] đầy, hoặc một thông báo khác): đã "
                    "đóng thông báo; lần ra TổCổLong sau mua lại.");
        } else if (trash_stop_) {
            // DỪNG, F12: the tick drops what is owed with the bot stopped.
        } else if (r.board_up) {
            later(BagCleaner::kBoardWaitMs, "the check-in board is up");
            board_put_off("pet round", false);
        } else if (hp_stop) {
            later(bag::kHpStoppedAgainMs, "stopped for HP");
        } else if (cut || r.keys_late) {
            later(5'000, cut ? "the hold lost" : "a key of the macro's not up in time");
        } else {
            // Trouble, or a round that bought less than its slots with nothing
            // said: tried again 30 s on while still in the lobby (the owner's
            // 30 s for the shop's trouble), else at the next arrival.
            later(PetRestock::kAgainMs, r.trouble.empty() ? std::string("not all bought") : r.trouble);
            // Its first full try had: the lobby no longer waits for it (the
            // 2026.11.24 review) -- the retries go on meanwhile.
            pet_restock_.tried(entry);
            add("Mua HP pet chưa xong" + (r.trouble.empty() ? std::string() : ": " + r.trouble) + ". Thử lại sau " +
                std::to_string(PetRestock::kAgainMs / 1000) + " giây nếu còn ở TổCổLong, không thì ở lần ra "
                "TổCổLong sau.");
        }
        if (!line.empty()) tell(line);
        if (held) {
            held_looked_ms = now_ms();
            tell("Còn một hộp thoại hoặc cửa hàng đang mở trong game. Đóng nó (Hủy Bỏ, rồi ESC); đánh quái, bình "
                 "và buff tạm dừng đến khi không còn hộp thoại nào.");
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
        const bool look_before = look_owed;
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
            // Buying after each pass (2026.11.13b): the look a pass left owed
            // begins a plan, and the plan's own time none.
            const bool after_pass_buys = bag::buys_after_pass(settings);
            if (bag::shop_plan_begins(forced, after_pass_buys, look_owed, now_ms() >= plan_due_ms)) {
                look_owed = false;
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
                            next_plan_said().c_str());
                    tell("Cả hai số ô đều là 0: không mua gì.");
                    return;
                }
                // Not written down, nothing bought (2026.11.09, kept): this
                // GoBot no longer reads the file -- it counts from its start
                // (2026.11.12) -- but a GoBot of before on this Windows
                // account still does, and would buy the plan early by the old
                // time -- as a charm is not used unwritten. A "Mua ngay" in
                // the middle of a plan leaves that plan as it was.
                if (!shop_clock.set_next(kShopClockId, wall_s() + static_cast<std::int64_t>(every) * 60)) {
                    if (!forced) owed = bag::ShopOwed{};
                    // Buying after each pass: still owed, 5 min on.
                    if (!forced && after_pass_buys) look_owed = true;
                    shop_due_ms = forced ? due_before : minutes(5);
                    GB_WARN("Trash: shop -- %s could not be written; nothing bought", shop_clock_path().c_str());
                    tell("Không ghi được " + shop_clock_path() +
                         ", nên chưa mua bình (GoBot bản cũ trên máy này còn đọc giờ mua trong file đó)." +
                         std::string(forced ? "" : " Thử lại sau 5 phút."));
                    return;
                }
                owed = bag::shop_new_plan(settings.buy_hp_slots, settings.buy_mp_slots);
                plan_due_ms = minutes(every);
                GB_INFO("Trash: shop plan HP %d + MP %d (owed %d+%d, at most %d a hold%s); clock saved, next plan %s",
                        settings.buy_hp_slots, settings.buy_mp_slots, owed.hp, owed.mp,
                        forced ? BagCleaner::kShopMaxBuys : BagCleaner::kShopBuysPerHold, forced ? ", Mua ngay" : "",
                        after_pass_buys ? "after the next timed pass (buy_after_trash)"
                                        : clock_at(plan_due_ms).c_str());
            }
            if (owed.hp + owed.mp == 0) {
                shop_due_ms = plan_due_ms;
                GB_INFO("Trash: shop -- nothing left of this plan (a number lowered); next plan %s",
                        next_plan_said().c_str());
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
        // The macro's HP potion on a key the window does not let through -- a
        // digit -- gives way before a purchase's double-click (2026.11.21, the
        // audit's B2; drink_yield_of): a timed round, one that rides a pass
        // too, not "Mua ngay" nor "Thử mua".
        if (!forced && !dry)
            cleaner.set_yield(drink_yield_of(pass_keys, false, held || trash_left_held_, {}));
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
                    // 30 s and 6 s a purchase of this round, 300 s at most --
                    // moved on by the cleaner by whatever the game took past 2 s
                    // to react to an input (2026.11.15d, react_took).
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
                        yield_from_ms = now_ms();
                        r = cleaner.run();
                        yield_from_ms = 0;
                    }
                    say_stopped("shop round", hp_cut, r.trouble);
                    // What stopped it, as its guard saw it (2026.11.11): HP
                    // under the round's level -- whatever HP reads by now, a
                    // potion let through may have lifted it since -- and,
                    // looked at now, the hold no longer this job's. Given way
                    // for the macro's HP potion (2026.11.21, B2) is HP's too.
                    hp_stop = !trash_stop_ && (hp_cut.level > 0 || r.yielded);
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
                look_owed = look_before;
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
        // keeps its clock as it was, owing the rest -- due already, so the
        // next worker counts a whole interval from its start (2026.11.12; its
        // start's 45 s before), and a plan come due by then begins afresh.
        // The board, the hold lost, HP its guard stopped it for, or a key of
        // the macro's not seen up in time put the rest off -- the board
        // kBoardWaitMs, HP kHpStoppedAgainMs, the others a part's gap,
        // kShopPartGapMs -- and are not trouble (bag::shop_end);
        // a full bag, or a message in the quantity box's place, ends the plan;
        // a trouble waits kShopTroubleAgainMs -- 30 s since 2026.11.15b, ten
        // minutes before; not every few seconds, which would hold the bot off
        // each time -- every one of them, without limit, since 2026.11.24 (the
        // owner: "Kiểm tra các phần khác xem có phần nào không làm được thì
        // dừng không, cần thay cơ chế này bằng 1 cơ chế khác"; the 30 s are his
        // own, of 2026.11.15b): until then the second in a row dropped what was
        // left of the plan until the next, an hour on as shipped. What may have gone
        // through still counts as bought, and a retry begins no plan of its own
        // (bag::shop_plan_begins). `troubled`: this round the second or later
        // in a row, the rest owed and tried again -- the card says so.
        bool troubled = false;
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
            const bool next_part = owed.hp + owed.mp > 0;
            if (!next_part) shop_due_ms = plan_due_ms;
            else if (trash_stop_) shop_due_ms = due_before;
            else if (r.board_up) shop_due_ms = now_ms() + BagCleaner::kBoardWaitMs;
            else if (hp_stop) shop_due_ms = now_ms() + bag::kHpStoppedAgainMs;
            else if (end == bag::ShopEnd::Paused) shop_due_ms = now_ms() + BagCleaner::kShopPartGapMs;
            else if (end == bag::ShopEnd::Trouble) shop_due_ms = now_ms() + bag::kShopTroubleAgainMs;
            else shop_due_ms = now_ms() + BagCleaner::kShopPartGapMs;
            const std::string when =
                next_part ? std::to_string((shop_due_ms > now_ms() ? shop_due_ms - now_ms() : 0) / 1000) + " s on"
                          : next_plan_said();
            GB_INFO("Trash: shop done -- bought %d+%d, unsure %d, full %s%s; owed %d+%d; %s %s",
                    r.shop_bought_hp, r.shop_bought_mp, r.shop_unsure, r.shop_full ? "yes" : "no",
                    r.shop_message ? ", a message instead of the box" : r.board_up ? ", the check-in board" : "",
                    owed.hp, owed.mp, next_part ? "next part" : "next plan", when.c_str());
            // A trouble with something still owed (2026.11.24): kept, and tried
            // again, however many came in a row -- said with their count.
            if (end == bag::ShopEnd::Trouble && next_part) {
                GB_INFO("Trash: shop -- %d trouble(s) in a row: owed %d+%d kept, tried again %llu s on", owed.troubles,
                        owed.hp, owed.mp,
                        static_cast<unsigned long long>((shop_due_ms > now_ms() ? shop_due_ms - now_ms() + 999 : 0) /
                                                        1000));
                troubled = owed.troubles >= 2 && !trash_oneshot_ && !trash_stop_;
            }
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
        // A notice GoBot closed itself, after a purchase's Xác Nhận or before
        // the shop's ESC (2026.11.11c): said, with where its picture is.
        if (r.shop_notices > 0)
            add("Game hiện thông báo lạ " + std::to_string(r.shop_notices) +
                " lần: GoBot đã bấm Xác Nhận của nó để tắt. Ảnh ở trash_debug (shop_notice).");
        // The trouble's words -- in the line below instead, the second time in
        // a row and on (2026.11.24).
        if (!r.trouble.empty() && !troubled) line += (line.empty() ? "" : " ") + std::string("Dừng: ") + r.trouble + ".";
        // What is left of the plan, and when it is bought: by this worker
        // later, or -- one here for the button alone -- at the first timed
        // look once the bot runs, an interval after BẮT ĐẦU (2026.11.12). In
        // seconds under a minute (2026.11.24: a trouble's 30 s read "1 phút").
        // Troubles in a row never drop it (2026.11.24): the second and on are
        // said as such, with the trouble's words and the 30 s -- until then
        // "Hai lần mua liền bị lỗi: GoBot bỏ phần còn lại của lượt này".
        const int left = owed.hp + owed.mp;
        const std::uint64_t in_ms = shop_due_ms > now_ms() ? shop_due_ms - now_ms() : 0;
        const std::uint64_t in_s = std::max<std::uint64_t>(1, (in_ms + 999) / 1000);
        const std::string in_said =
            in_s < 60 ? std::to_string(in_s) + " giây" : std::to_string((in_ms + 59'999) / 60'000) + " phút";
        if (troubled) {
            add("Mua bình bị lỗi " + std::to_string(owed.troubles) + " lần liền" +
                (r.trouble.empty() ? std::string() : " (" + r.trouble + ")") + ": GoBot thử lại sau " + in_said + ".");
        } else if (!dry && left > 0 && (!r.board_up || trash_oneshot_) && !trash_stop_) {
            add("Còn " + std::to_string(left) + " lần" +
                (trash_oneshot_ ? std::string(" của lượt này: GoBot mua ở lần mua theo giờ đầu tiên sau khi bấm BẮT "
                                              "ĐẦU.")
                                : ", mua tiếp sau " + in_said + "."));
        }
        if (!line.empty()) tell(line);
        if (held) {
            held_looked_ms = now_ms();
            tell("Còn một hộp thoại hoặc cửa hàng đang mở trong game. Đóng nó (Hủy Bỏ, rồi ESC); đánh quái, bình "
                 "và buff tạm dừng đến khi không còn hộp thoại nào.");
        }
    };

    // The dungeon run's boss fight or reward room last said on the card as
    // what a timed job waits for (B6, below); None once nothing waits on it.
    TripQuiet trip_told = TripQuiet::None;
    std::uint64_t trip_off_since = 0;   ///< the boss's room put a timed job off since (0: not)
    while (!trash_stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Here for a run -- from the start, or a button's worker become one:
        // the timed clocks count from now (2026.11.12).
        if (!counting && !trash_oneshot_) start_counting();
        if ((held || trash_left_held_) && now_ms() - held_looked_ms > 1500) look_for_clear();
        // The kept hold's way out (2026.11.21, B1): kept since when, and swept
        // by this worker when bag::kept_sweep_due says.
        if (!held) {
            kept_ms = kept_board_ms = kept_swept_ms = 0;
            shop_held = false;
        } else if (kept_ms == 0) {
            kept_ms = now_ms();
        }
        if (held && !trash_stop_) {
            bag::KeptSweep k;
            k.held = held;
            k.hold_ours = coord_.critical_active() && coord_.critical_reason() == "trash cleaning";
            k.now_ms = now_ms();
            k.kept_ms = kept_ms;
            k.board_ms = kept_board_ms;
            k.swept_ms = kept_swept_ms;
            k.night = checkin_night_hold(local_second_of_day());
            if (bag::kept_sweep_due(k)) kept_sweep();
        }
        const int request = trash_request_.exchange(0);
        if (request == 0 && now_ms() - read_ms > 1000) {
            CleanSettings fresh;
            if (fresh.load(config)) settings = fresh;
            read_ms = now_ms();
        }
        // "Tự dọn rác" switched off and on again, in the file or on the window:
        // the first timed pass a whole interval from now (2026.11.12), not one
        // the last clock had due.
        if (settings.enabled != pass_was_on) {
            pass_was_on = settings.enabled;
            if (pass_was_on) pass_afresh();
        }
        if (pass_ticked_.load() != pass_ticks) {
            pass_ticks = pass_ticked_.load();
            pass_afresh();
            if (counting)
                GB_INFO("Trash: Tự dọn rác ticked on -- the first timed pass at %s, %d min from now",
                        clock_at(next).c_str(), settings.interval_minutes);
        }
        // The potion shop, asked for with a button (4: buy now, 5: a trial) or
        // due by its time: a job of its own, like the charms'.
        // Switched off and on again: nothing owed, the next look a whole
        // interval from now (2026.11.12), as at a start -- a plan just bought
        // is never bought again sooner ("Mua ngay" is for that) -- and a spoiled
        // quiet second's wait from its first step.
        if (settings.buy_potions != shop_was_on) {
            shop_was_on = settings.buy_potions;
            if (shop_was_on) {
                owed = bag::ShopOwed{};
                plan_due_ms = 0;
                shop_afresh();
                shop_quiet.reset();
            }
        }
        // "Mua ngay sau khi dọn rác" (2026.11.13b) come or gone -- the box, or
        // Tự dọn rác under it: nothing owed by a pass, and the clock, buying
        // again, a whole interval from now.
        {
            const bool after_now = bag::buys_after_pass(settings);
            if (after_now != after_was) {
                after_was = after_now;
                look_owed = false;
                // A part still owed keeps its look (the review): only with
                // nothing owed does the clock count afresh.
                if (!after_now && settings.buy_potions && owed.hp + owed.mp == 0) shop_afresh();
                if (counting)
                    GB_INFO("Trash: shop %s", after_now ? "buys right after each timed pass (buy_after_trash), the "
                                                          "clock beginning no plan"
                                                        : ("by its clock again -- the first look at " +
                                                           clock_at(shop_due_ms) + ", " +
                                                           std::to_string(settings.buy_every_minutes) +
                                                           " min from now")
                                                              .c_str());
            }
        }
        // The same for a tick on the window, which the file may not have shown
        // off in between -- and the charms' clock, after a tab not read or a
        // run that stopped on something, from a start's wait.
        if (shop_ticked_.load() != shop_ticks) {
            shop_ticks = shop_ticked_.load();
            owed = bag::ShopOwed{};
            plan_due_ms = 0;
            shop_afresh();
            shop_quiet.reset();
            if (counting)
                GB_INFO("Trash: Tự mua bình ticked on -- the first look at %s, %d min from now, owing nothing",
                        clock_at(shop_due_ms).c_str(), settings.buy_every_minutes);
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
        // Buying after each pass (2026.11.13b): the clock looks only for what
        // a pass or a plan still owes.
        const bool shop_due = timed && !held && settings.buy_potions && now_ms() >= shop_due_ms &&
                              bag::shop_clock_looks(bag::buys_after_pass(settings), look_owed, owed.hp + owed.mp);
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
            // A retry after trouble pulls no pass (the 2026.11.24 review):
            // with the trouble cap gone a shop that never opens came due every
            // 30 s for good, and pulled a whole Dọn rác pass about once a
            // minute under the critical hold. It runs alone, under its own
            // short hold; the job itself still goes by shop_due.
            const bool ride_due = shop_due && owed.troubles == 0;
            ride = bag::ride_turn(was, ride_due, settings.enabled, cannot_pull, now_ms(), last_pass_end_ms);
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
        // A dungeon run's boss fight or reward room (2026.11.21, the audit's
        // B6; the owner: "không để các chức năng block nhau"): a timed pass,
        // charm run or shop round waits it out -- nothing taken, nothing
        // moved: each is still due, and goes in the player's order (chat,
        // the pass, the charms, the shop) the moment it is over. Under their
        // hold the hunter swung at nothing in the boss fight, and in the
        // reward room the NPC's 15 s and the 30 s to be out went by. Capped
        // (TripQuietFlag), so a long fight still gets its pass and potions in
        // the end. Never a button's job (request 1 to 5: `timed` is false),
        // nor a pass that gives back what may be in the bin -- that hold is
        // already on, and only the pass ends it. Ordinary hunting: no change.
        // In the boss's room a job waits no more than kBossPutOffMostMs (30
        // min, the owner's choice of 2026.11.21, the shop included); then it
        // goes, and the next waits anew.
        if ((pass_due || charm_due || shop_due) && !held && !trash_left_held_) {
            const TripQuiet trip = trip_quiet_.at(now_ms());
            if (trip != TripQuiet::None && trip_off_since == 0) trip_off_since = now_ms();
            if (TripQuietFlag::holds_job(trip, trip_off_since, now_ms())) {
                const char* job = pass_due ? "pass" : charm_due ? "charms" : "shop";
                // Said on the card once for each stretch of it.
                if (trip != trip_told)
                    tell(std::string("Chờ: ") + trip_quiet_vi(trip) + ", " +
                         (pass_due ? "dọn rác" : charm_due ? "dùng bùa" : "mua bình") + " sau.");
                trip_told = trip;
                put_off(job, "trip", std::string("a dungeon run's ") + trip_quiet_name(trip) +
                                         " (the expedition) -- it goes as that is over");
                continue;
            }
            if (trip != TripQuiet::None)
                GB_INFO("Trash: a %s put off %llu s by a dungeon run's boss fight goes now -- the potions and the bag "
                        "get their turn", pass_due ? "pass" : charm_due ? "charm run" : "shop round",
                        static_cast<unsigned long long>((now_ms() - trip_off_since) / 1000));
        }
        trip_told = TripQuiet::None;
        trip_off_since = 0;
        if ((pass_due || charm_due || shop_due) && chat_first()) continue;
        // The pet's HP food on reaching TổCổLong (2026.11.24, run_pet): the
        // round PetRestock owes for the entry the character is still in, as
        // soon as it may -- a button's job first, then, in the owner's order,
        // a Tự chat line, a timed pass and the charms due, and before a timed
        // potion round. Past the arrival's time, or the lobby left, it is over
        // until the next arrival, said once; nothing else changes for it.
        if (pet_restock_.expire(now_ms()))
            tell("Mua HP pet: quá " + std::to_string(PetRestock::kWaitMostMs / 1000) +
                 " giây ở TổCổLong mà chưa mua xong; lần ra TổCổLong sau mua lại.");
        const std::uint32_t pet_owed = pet_restock_.owed(now_ms());
        bool pet_due = false;
        if (pet_owed != 0) {
            const PacketWorld::Place here = packet_world_.place();
            const std::string stem_here = here.map ? GameMapBook::stem_of(here.map, here.area) : std::string();
            if (here.epoch != pet_owed || stem_here != expedition::kCoLongLobby) {
                pet_restock_.over(pet_owed);
                GB_INFO("Pet: the character left TổCổLong (map entry %u) before the pet's round -- over until the "
                        "next arrival", pet_owed);
            } else {
                pet_due = request == 0 && !held && !trash_left_held_ && pet_restock_.due(now_ms()) == pet_owed;
            }
        }
        if (pet_due && chat_first()) continue;
        if (pet_due && !pass_due && !charm_due) {
            run_pet(pet_owed);
            continue;
        }
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
        // A worker here for the pet's round alone (request_pet) stays while it
        // is owed, put off or not (2026.11.24).
        if (request == 0 && trash_oneshot_ && pet_owed != 0 && pet_restock_.owed(now_ms()) != 0) continue;
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

        // Not with HP under the start gate -- bag::kJobDownHpPct since
        // 2026.11.11b, the character down; an HP potion the window holds off
        // has the pass give way for it instead (2026.11.21, drink_yield_of)
        // rather than a margin over its mark. A timed pass waits a few seconds, said
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
        // Buying after each pass (2026.11.13b, the review): a timed pass with
        // nothing it could do still owes the shop its look -- on its own, as
        // no pass is pulled for it -- or nothing would ever be bought.
        const auto owe_look_unrun = [&] {
            if (request != 0 || !bag::pass_owes_look(bag::buys_after_pass(settings), owed.hp + owed.mp)) return;
            look_owed = true;
            shop_due_ms = std::min(shop_due_ms, now_ms());
        };
        TrashList list;
        list.build(exe_relative("configs/trash"), settings, catalog);
        if (list.items.empty()) {
            tell("Danh sách đồ bỏ đi đang trống, nên không có gì để dọn.");
            if (request == 0) no_pull_before_ms = next;
            owe_look_unrun();
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
            owe_look_unrun();
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
        // bag::kJobDownHpPct since 2026.11.11b, the character down: the pass
        // stops between items and gives the keys back. What HP stopped it at
        // goes to the log (hp_cut), and the pass is tried again
        // kHpStoppedAgainMs on.
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
        // The macro's HP potion on a key the window does not let through -- a
        // digit -- gives way between two items (2026.11.21, the audit's B2;
        // drink_yield_of): a timed pass, not one asked for with a button.
        if (request == 0 && !trash_oneshot_)
            cleaner.set_yield(drink_yield_of(pass_keys, false, left_held, {}));
        // An item stuck in the bin -- nothing come of GoBot's last input for it
        // in 30 s (2026.11.14b, 30 s since 2026.11.15d) -- is shut with ESC
        // (esc_out) under this guard alone: it stops for HP only with the
        // character down -- the death panel up, which no ESC may touch -- and
        // with the hold another job's (the review of 2026.11.14b).
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
                    yield_from_ms = now_ms();
                    r = cleaner.run();
                    yield_from_ms = 0;
                }
                if (!look_only) last_pass_end_ms = now_ms();
                GB_INFO("Trash: pass took %llu ms: %d deleted, %d failed%s",
                        static_cast<unsigned long long>(now_ms() - pass_from), r.deleted, r.failed,
                        r.trouble.empty() ? "" : " (stopped)");
                say_stopped("pass", hp_cut, r.trouble);
                // Stopped by its guard for HP (2026.11.11), or given way for
                // the macro's HP potion (2026.11.21, B2): tried again in a
                // few seconds, not the whole interval on -- unless the hold is
                // kept, below, which looks again in a minute.
                if ((hp_cut.level > 0 || r.yielded) && !trash_stop_) next = now_ms() + bag::kHpStoppedAgainMs;
                // Kept while the bin may still hold the item, and by a pass
                // that never got to a clean screen -- a look-only one, or one
                // that met a dialog it would not answer -- since it saw
                // nothing of an earlier item coming back. Otherwise ended, if
                // it is still this pass's: one taken over during the pass is
                // somebody else's to end.
                held = r.keeps_hold(held);
                // A shop's hold ended by a pass is not a shop's any more
                // (2026.11.21, B1): a later bin hold waits for no board.
                if (!held) shop_held = false;
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
                // "Mua ngay sau khi dọn rác" (2026.11.13b): every timed pass
                // that ran owes the shop a look -- ridden now, or, refused,
                // the shop's own at once, by its rules.
                // A plan's part still owed is that plan's (the review of
                // 2026.11.13b): it rides as a part, owing no new plan --
                // else every part's pass began the plan afresh and bought
                // until the bag was full.
                const bool after_pass_buys = bag::buys_after_pass(settings);
                if (after_pass_buys && ridden.timed && !look_only && !trash_stop_) {
                    if (bag::pass_owes_look(after_pass_buys, owed.hp + owed.mp)) look_owed = true;
                    shop_due_ms = std::min(shop_due_ms, now_ms());
                }
                ridden.shop_due = settings.buy_potions && now_ms() >= shop_due_ms &&
                                  bag::shop_clock_looks(after_pass_buys, look_owed, owed.hp + owed.mp);
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
                    pulled ? "the look that pulled it"
                    : look_owed ? "right after each timed pass, buy_after_trash"
                                : "come due while it ran");
            ride = bag::ShopRide::None;
            run_shop(false, false, true);
            if (!held) end_hold("trash cleaning");
            trash_pass_ = false;
            if (mouse.owns_lock()) mouse.unlock();
        }
        if (trash_oneshot_ && !held) break;
    }
    // The charms', the pass's and the shop's clocks for the next worker: a
    // MACRO OFF and ON keeps them (see where they were read) -- and the shop's
    // plan, what is owed of it said on the card while no worker buys it. The
    // pass's and the shop's only from a worker that counted them (2026.11.12):
    // one here for a button alone leaves them -- and the ticks of their boxes
    // -- as it found them.
    {
        std::lock_guard<std::mutex> lock(trash_clocks_mutex_);
        trash_clocks_.charm_check_ms = charm_check_ms;
        if (counting) {
            trash_clocks_.pass_due_ms = next;
            trash_clocks_.pass_ticks_seen = pass_ticks;
            trash_clocks_.shop_due_ms = shop_due_ms;
            trash_clocks_.shop_ticks_seen = shop_ticks;
        }
        trash_clocks_.shop_quiet_level = shop_quiet.level();
        trash_clocks_.charm_ticks_seen = charm_ticks;
        trash_clocks_.owed_hp = owed.hp;
        trash_clocks_.owed_mp = owed.mp;
        trash_clocks_.plan_due_ms = plan_due_ms;
        trash_clocks_.shop_troubles = owed.troubles;
        trash_clocks_.look_owed = look_owed;
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

void GuiApp::chat_seen_shut() {
    chat_doubt_ = false;
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
        // No bar, on a page of the login (2026.11.21, the audit's B4): the
        // session dropped back to it in the same window, and there is no
        // chat line there to be open. Every look must find the page, as every
        // look must find the bar shut.
        if (bar.state != ChatBarState::Closed && !shows_login_page(f, store))
            return no("Không thấy ô chat ở góc trái dưới màn hình game, nên chưa biết ô chat đã đóng "
                      "chưa. Đừng để cửa sổ nào che khung chat, rồi bấm BẮT ĐẦU.");
    }
    return true;
}

bool GuiApp::shows_login_page(const Frame& f, const TemplateStore& store) const {
    if (!f.valid()) return false;
    // login.ini's pages, or the ones that ship with no file -- read each time:
    // asked at most once a second, and only while a line is in doubt with no
    // bar seen. The pictures are the program's one set, which the keeper's
    // own are copied out of (keeper_pictures).
    AutoLogin::Settings login;
    login.load(exe_relative("configs/login.ini"));
    return login_page_up(f, login, store);
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
        // What a run still on owes (2026.11.24) -- a start this doubt refused
        // -- is asked in this tick (owed_tick), not on its 30 s: the note says
        // GoBot tries, not "Bấm BẮT ĐẦU".
        const bool owed = owed_now();
        // Nothing owed with the run on: the big button reads DỪNG LẠI (the
        // 2026.11.24 review), as watch_left_held says it.
        const std::string note = owed ? "Ô chat trong game đã đóng: GoBot thử bật lại những việc chưa bật được."
                                 : simple_running()
                                     ? "Ô chat trong game đã đóng. Bấm DỪNG LẠI rồi BẮT ĐẦU để đánh quái, bình và "
                                       "buff chạy lại."
                                     : "Ô chat trong game đã đóng. Bấm BẮT ĐẦU để đánh quái, bình và buff "
                                       "chạy lại.";
        simple_note(note, 12000);
        if (!simple_view_) set_status(note, 12000);
        GB_INFO("Chat: the game shows the chat line shut");
        return;
    }
    if (!chat_doubt_ || chat_doubt_watched_) return;
    const std::uint64_t now = now_ms();
    if (now - chat_doubt_looked_ms_ < 2000) return;
    chat_doubt_looked_ms_ = now;
    chat_doubt_look_gen_ = chat_doubt_gen_;
    // One look at a time, so chat_doubt_store_ is only ever this look's to
    // set; on a thread of its own, as a capture can take a while.
    chat_doubt_look_ = std::async(std::launch::async, [this] {
        if (!chat_doubt_store_) chat_doubt_store_ = TemplateStore::shared_best(exe_relative("configs/templates.ini"));
        return chat_line_looks_shut(*chat_doubt_store_, 2, nullptr);
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
    // A job between fights: it can wait for the game's slice to end.
    quiet_this_thread();
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

    // The program's one set of pictures, held while this runs (2026-11-16).
    std::shared_ptr<const TemplateStore> pictures =
        TemplateStore::shared_best(exe_relative("configs/templates.ini"));
    // Kept from one look at the file to the next, and replaced only by a
    // reading that worked: the tab rewrites the file as things change.
    const std::string config = exe_relative("configs/chat.ini");
    ChatSettings settings;
    if (!settings.load(config)) tell("Không đọc được configs/chat.ini, nên dùng cài đặt mặc định.");
    // What the line needs, missing: none of the game's pictures; the chat
    // bar's, without which nobody knows whether the line is open or shut; the
    // dialogs', as Enter answers a dialog and without them there is no
    // telling whether one is up. Nothing is typed without all three.
    const auto missing = [&](const TemplateStore& s) -> std::string {
        if (s.size() == 0) return "hình giao diện";
        if (!can_look_for_chat(s, settings)) return "hình ô chat " + settings.open_picture;
        if (!ChatDialogs::can_check(s)) return "hình hộp thoại";
        return {};
    };
    // A one-off -- Gửi thử, Kiểm tra ô chat -- still ends at once with the
    // line: the player pressed it to see it now, and the tab's buttons stay
    // greyed while a worker is here.
    if (chat_oneshot_) {
        const std::string what = missing(*pictures);
        if (!what.empty()) {
            finish("Thiếu hình giao diện của game (" + what + "), nên không biết ô chat đang mở hay đóng, hay có "
                   "hộp thoại nào đang mở không — không chat.");
            return;
        }
    }
    // A run no longer ends here (2026.11.24, the owner: "Kiểm tra các phần khác
    // xem có phần nào không làm được thì dừng không, cần thay cơ chế này bằng 1
    // cơ chế khác"): it waits, typing nothing, and looks for the pictures again
    // every 30 s (wait_for_pictures), the chat.ini read again too, as a
    // picture's name is in it. Unticking or DỪNG ends the wait at once; a run
    // asked for again meanwhile keeps the worker waiting (stays). Until then
    // Tự chat ended here until the next BẮT ĐẦU.
    // Kiểm tra ô chat or Gửi thử 1 câu pressed meanwhile: answered at once,
    // never kept to type a line at a moment the player did not choose (the
    // 2026.11.24 review).
    std::string chat_missing = "hình giao diện";
    for (;;) {
        const bool have = wait_for_pictures(
            pictures, exe_relative("configs/templates.ini"), "Chat",
            [&](const TemplateStore& s) {
                ChatSettings fresh;
                if (fresh.load(config)) settings = fresh;
                return missing(s);
            },
            [&] {
                if (chat_request_.exchange(0) != 0) tell(pictures_again_vi(chat_missing));
                return chat_stop_.load() || chat_quit_.load();
            },
            [&](const std::string& what) {
                chat_missing = what;
                tell(pictures_again_vi(what));
            });
        if (have) break;
        if (!stays()) return;
        // Turned into a run again while it waited: it goes on waiting, and
        // the run's start is made once, below.
        chat_restart_ = false;
    }
    const TemplateStore& store = *pictures;
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
    // the hold, kept from the line that left it so -- until a look shows it
    // shut. See ChatDoubt.
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
    // A page of the login where the line was (2026.11.21, the audit's B4): a
    // fresh frame, off the screen as every look of this worker's.
    hooks.no_line_here = [&] {
        Frame f;
        return grab(f) && f.valid() && shows_login_page(f, store);
    };
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
            // A dungeon run's boss fight or reward room (2026.11.21, the
            // audit's B6): a timed line waits it out; "Gửi thử" goes.
            in.dungeon = asked ? std::string() : std::string(trip_quiet_vi(trip_quiet_.at(now_ms())));
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
        // The bottom of the game off the screen or under the taskbar, on a
        // frame read off the screen (2026.11.13c): the bar there cannot be
        // seen, which is said.
        in.unseen = game_unseen(kChatRowsUnder, "thấy ô chat để Tự chat", capture.from_screen());
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
        sender.set_unseen(
            [this, &capture] { return game_unseen(kChatRowsUnder, "thấy ô chat để Tự chat", capture.from_screen()); });
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
    // A line GoBot could not judge or close (Result::stop: the bar like neither
    // picture, or the line that would not shut): the next try a whole line
    // interval from when its doubt ends -- this gap, until then (2026.11.24,
    // chat_retry_ms). Until 2026.11.24 such a line stopped Tự chat for the
    // session (stopped_self, gone), and so did max_failures lines in a row.
    std::uint64_t gap_after_doubt = 0;
    std::uint64_t read_ms = now_ms();
    // A run from its beginning: the first line after its grace, nothing sent.
    const auto begin_run = [&] {
        schedule.start(now_ms(), settings);
        chat_sent_ = 0;
        put_off_in_row = 0;
        gap_after_doubt = 0;
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
                    // The line would not close, or the bar did not look like
                    // either picture (r.stop): no longer the end of a run
                    // (2026.11.24, the owner: "Kiểm tra các phần khác xem có
                    // phần nào không làm được thì dừng không, cần thay cơ chế
                    // này bằng 1 cơ chế khác") -- until then Tự chat stopped
                    // here, run or no run. Its doubt, when the line may be
                    // open, holds every key until the line is seen shut
                    // (note_doubt above), and the timed lines keep their own
                    // turn and timer -- but none goes the moment that doubt
                    // ends (the 2026.11.24 review): a second Enter on the
                    // same unrecognised bar, a second doubt holding every key.
                    // It postpones the next timed try only, the turn and the
                    // failures left alone.
                    if (r.stop) gap_after_doubt = std::max(gap_after_doubt, chat_retry_ms(true, settings));
                }
                continue;
            }

            if (doubt.active()) {
                chat_next_ms_ = 0;
                chat_waits_trash_ms_ = 0;
                doubt.step(*closer, now_ms());
                if (doubt.active()) continue;   // nothing else while the line may be open
            }
            // A line GoBot could not judge or close (2026.11.24): its doubt
            // over -- or none, the line seen shut at once -- the next try a
            // whole line interval from now, not sooner: each such try may leave
            // the line in doubt again, every key held until it is seen shut.
            if (gap_after_doubt != 0) {
                const std::uint64_t now = now_ms();
                if (schedule.due_at() < now + gap_after_doubt) schedule.postpone(now, gap_after_doubt);
                gap_after_doubt = 0;
            }
            if (chat_stop_ || chat_oneshot_) break;
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
                // Sent, and then would not close (r.trouble says so, "— GoBot
                // thử lại ở lượt sau"): no longer a stop (2026.11.24). The line
                // open is what the fighting keys would be typed into, so its
                // doubt holds every key until it is seen shut, as ever, and the
                // next line waits a whole line interval from then. Until
                // 2026.11.24 Tự chat stopped here.
                if (r.stop) gap_after_doubt = chat_retry_ms(true, settings);
            } else if (r.aborted && !r.stop) {
                // Ended part way for something that is no fault of the line's --
                // HP, a dialog, the game losing the front -- and rubbed out. Its
                // turn kept and tried again soon; not a failure, or a long fight
                // would count against it (until 2026.11.24, stop Tự chat by
                // itself). Several in a row wait longer.
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
            } else if (chat_quit_ || chat_stop_) {
                // Cut by DỪNG, F12 or an unticking after its Enter (the
                // 2026.11.24 review): put off, not a failure -- never counted,
                // nor the phone's "GoBot vẫn thử lại" just as the player
                // stopped Tự chat.
                schedule.postpone(end, 3000);
                tell_now_and_then("Câu " + std::to_string(index + 1) + ": " + r.trouble);
            } else {
                // Its turn kept, tried again after a pause -- never stopped for
                // good (2026.11.24, the owner: "Kiểm tra các phần khác xem có
                // phần nào không làm được thì dừng không, cần thay cơ chế này
                // bằng 1 cơ chế khác"; until then a bar like neither picture,
                // or a line that would not close, stopped Tự chat for the
                // session, and so did max_failures lines in a row). 30 s, as
                // ever; a line GoBot could not judge or close (r.stop) a whole
                // line interval, counted from when its doubt ends -- each such
                // try may leave the line in doubt again (chat_retry_ms). The tab
                // and the phone are told once, as the count reaches
                // max_failures; a line sent starts it again (ChatSchedule::sent).
                const std::uint64_t retry_ms = chat_retry_ms(r.stop, settings);
                schedule.failed(end, retry_ms);
                if (r.stop) gap_after_doubt = retry_ms;
                tell("Không gửi được câu " + std::to_string(index + 1) + ": " + r.trouble +
                     bar_numbers(r.last));
                if (schedule.failures() == settings.max_failures) {
                    const std::string told = alert_text::chat_stopped(schedule.failures());
                    tell(told);
                    alert("chat_stopped", told);
                }
            }
            chat_next_ms_ = schedule.due_at();
        }

        // Told to quit -- DỪNG, F12, closing GoBot: the player has taken over.
        // A line still in doubt is handed over rather than waited on: its
        // hold stays on until tick() has stopped everything else that presses
        // keys, and nothing of that starts again until a look shows the line
        // shut (keys_may_start).
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
    // What the game's packets say of HP and MP, and whether they go over
    // these bars now (packets_vitals.cpp).
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

// ---------------------------------------- the game's window, the detector

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
    detector_.set_target_window(input_.target_window());
    detector_.set_tuning(detect_tuning_);

    // Read the name plates rather than guessing at them, when the game's own
    // font is available to read them with. Loaded once, on the first start.
    if (!font_.loaded()) {
        const std::string dir = GlyphFont::default_dir();
        if (!dir.empty() && font_.load(dir))
            set_status("Reading name plates with the game's own font", 5000);
    }
    if (font_.loaded()) detector_.set_font(&font_);
    detector_.set_character_name(hunt_config_.character_name);

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

void GuiApp::stop_detector() {
    if (!detector_running_) return;
    // Marked stopped first. The hunt's end below lets the detector go by
    // itself (release_detector), and finds it stopped already: it is stopped
    // once, here, and never through a call of this from in there.
    detector_running_ = false;
    stop_hunt();                  // nothing to hunt with the eyes shut
    detector_.stop();
}

void GuiApp::release_detector() {
    if (!detector_running_ || detector_wanted()) return;
    GB_INFO("Mob detector: nothing looks through it now -- stopped until the next hunt or look");
    stop_detector();
}

bool GuiApp::detector_wanted() const {
    return hunting_ || monster_look_running() || jump_test_running();
}

void GuiApp::start_hunt() {
    vitals_for_job();       // HP and MP at the run's pace before any key
    if (hunting_) return;
    spot_refused_vi_.clear();
    if (!input_.window_valid()) {
        set_status("Pick the game window first", 6000);
        return;
    }
    // A refusal below said -- the card, the status line, the log -- unless
    // tick() is asking again for a start owed (owed_tick, 2026.11.24) and meets
    // the reason it met last: asked every 30 s while Npcap is not up, the same
    // words would flash the card each time. A new reason is said; a start asked
    // any other way -- BẮT ĐẦU, F11, a tick, the bin's doubt over -- says it
    // always.
    const auto say = [this](const char* why) {
        const bool again = owed_asking_ && owed_why_ == why;
        owed_why_ = why;
        return !again;
    };
    // Your character is found by its name and nothing else: a plate with bars
    // may be yours or another player's. Started without one, the hunter only
    // stood there -- after F11 had brought the game forward -- or, before the
    // detector stopped taking the nearest player for you, walked and hit
    // relative to a stranger.
    if (!hunt_named()) {
        if (say("no character name typed")) {
            simple_note("Chưa gõ tên nhân vật nên chưa bật Tự đánh quái. Gõ tên ở phần \"Trước khi "
                        "bắt đầu\" của thẻ Đánh quái.", 12000);
            if (!simple_view_)
                set_status("Type your character's name first: the hunter finds you by it", 12000);
            GB_WARN("Hunting not started: no character name typed, so there is no 'you' to walk from");
        }
        return;
    }
    // "Chỗ đánh quái" is where the game's packets put the character. With no
    // packets to be had -- no Npcap, or the positions switched off -- there is
    // no spot to take or walk back to, and the hunter would only ever stand:
    // refused, and said why, as the missing name is. The rest that was ticked
    // starts all the same, and the card under BẮT ĐẦU says it in red -- and,
    // the run on, that GoBot asks again every 30 s (hunt_owed_): Npcap's
    // service up late after the PC's start no longer costs the night's fight.
    if (const char* why = spot_refusal(spot_refused_vi_)) {
        if (say(why)) {
            simple_note("Chưa bật Tự đánh quái: " + spot_refused_vi_, 15000);
            if (!simple_view_)
                set_status("The hunting spot (spot=true) needs the game's packets: Npcap installed and running, and "
                           "positions from packets not Off",
                           12000);
            GB_WARN("Hunting not started: the hunting spot (spot=true) needs the game's packets, and %s", why);
        }
        return;
    }
    if (!keys_may_start()) return;
    if (!detector_running_) start_detector();
    if (!detector_running_) return;

    hunter_->set_config(hunt_config_);
    // A ↓ or Ctrl a drop left down in Windows let go of, as at GoBot's start;
    // and what the drop or a jump said in the last hunt, gone.
    if (hunt_config_.drop_down) input_.release_stray_drop_keys();
    // A high jump's ↑ and Ctrl the same way, with "Nhảy lên bục trên" on.
    if (hunt_config_.jump_up && !hunt_config_.drop_down) input_.release_stray_drop_keys();
    drop_note_vi_.clear();

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
    // Owed no longer, by whatever asked (2026.11.24); and a later refusal is
    // said again, whatever its reason.
    hunt_owed_ = false;
    owed_why_.clear();

    // The capture the positions come from, unless they are switched off: it
    // runs all session, and is asked for again here after a hand's stop. A
    // capture that will not start leaves the hunter on the screen, said once.
    position_capture_off_ = false;
    position_start_failed_ = false;
    position_backoff_ms_ = 0;
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
    // F11 is the player's own hand on the hunt (2026.11.24): a stop stays a
    // stop -- nothing owed is asked for it afterwards. A start F11 asks for
    // and that is refused (Npcap up late, say) is owed like any other (the
    // 2026.11.24 review): clearing it there ended the 30 s retry for good,
    // the player's very press to START being what stopped it. owe_start does
    // nothing with the run off or a cold start under way.
    if (hunting_) {
        hunt_owed_ = false;
        stop_hunt();
        return;
    }
    start_hunt();
    if (!hunting_ && hunt_config_.enabled) owe_start(hunt_owed_, "Hunting");
}

// ------------------------------------------- the hunter's drop (Ctrl + ↓)

const char* GuiApp::drop_veto_now() const {
    // On the hunter's thread: tick()'s code, and HP read live.
    switch (drop_veto_.load()) {
        case kDropVetoMacro:  return "the macro is held: the character is not in the world";
        case kDropVetoKeeper: return "the keeper is crossing";
        case kDropVetoHotkey: return "one of GoBot's own keys is Ctrl or Down, or Up with jumps up on";
        default:              break;
    }
    // HP low no longer says no (2026.11.12b, the owner: the drops and the
    // jumps up -- a monster's and the way back's -- at any HP): nothing holds
    // the macro's potions through a drop or a jump, which take the keys only
    // for the length of their press. Until 2026.11.12 HP under the chat's
    // floor did, and a fall out of the area under the HP potion's mark found
    // no way back. HP not known while an HP potion key is on still does: the
    // character may not be in the world. And the character down -- HP read
    // under bag::kJobDownHpPct, as the bag's jobs, the quest and the popups
    // have it (the 2026.11.12b review): a press on a dead character moves
    // nothing, and would only be judged a press that did nothing -- its place
    // skipped a while for no reason (2026.11.24; until then three such went
    // toward a pause, and before 2026.11.15 off for the run). A short re-check
    // each pass, never a stop.
    const float hp = hp_percent_now();
    if (hp >= 0.0f && hp < static_cast<float>(bag::kJobDownHpPct)) return "the character is down";
    if (hp < 0.0f && drop_hp_watch_.load()) return "HP is not known";
    return nullptr;
}

void GuiApp::drop_tick() {
    // What only this thread may read, for the hunter's veto: worked out with
    // the drop on, none with it off.
    int veto = kDropVetoNone;
    bool hp_watch = false;
    // The jumps up (2026.11.05b) go by the same veto: HP, the macro held, the
    // keeper crossing.
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

    // A press that changed the map: said once, and the hunt goes on
    // (2026.11.24, the owner: "không, 2. Nhảy xuống (Ctrl+↓) khi hỏng thì thử
    // lại luôn ... chứ không phải là dừng"). Both ends of it are learned as
    // gates -- no drop or jump near them again this session -- and the hunter
    // hunts on where the character landed. Nothing is stopped, nothing
    // unticked, hunt.ini not saved. Until 2026.11.24 (the player's answer 3 of
    // 2026-11-04) the hunt stopped here, Nhảy xuống and/or Nhảy lên were
    // unticked and saved, and a key-up stuck stopped it too (that one is the
    // tick's own since, every key let go of and the hunt going on).
    if (!hunting_ || !hunter_) return;
    const Hunter::PressGate gate = hunter_->take_press_gate();
    if (gate == Hunter::PressGate::None) return;
    SYSTEMTIME at{};
    GetLocalTime(&at);
    char clock[16];
    std::snprintf(clock, sizeof clock, "%02u:%02u:%02u", static_cast<unsigned>(at.wHour),
                  static_cast<unsigned>(at.wMinute), static_cast<unsigned>(at.wSecond));
    // A drop's, a jump up's, or both under watch as it changed (either may
    // have done it, 2026.11.05b).
    const char* what = gate == Hunter::PressGate::Drop ? "nhảy xuống" : gate == Hunter::PressGate::Jump ? "nhảy lên"
                                                                                                          : "nhảy";
    drop_note_vi_ = std::string("Lần ") + what + " lúc " + clock +
                    " làm đổi bản đồ — chỗ đó đã được nhớ là cửa, GoBot không nhảy ở gần đó nữa và vẫn đánh quái tiếp.";
    simple_note(drop_note_vi_, 15000);
    if (!simple_view_)
        set_status("A press changed the map -- learned as a gate; the hunt goes on, Nhảy xuống / Nhảy lên unchanged",
                   15000);
    GB_INFO("Hunter: a %s changed the map -- learned as a gate; the hunt goes on, the ticks unchanged",
            gate == Hunter::PressGate::Drop   ? "drop (Ctrl+Down)"
            : gate == Hunter::PressGate::Jump ? "jump up"
                                              : "drop or a jump up (both under watch)");
}

// ------------------------------------------------- positions from the packets

void GuiApp::position_say(const std::string& what) {
    if (what == position_said_) return;
    position_said_ = what;
    GB_WARN("Positions: %s", what.c_str());
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
    // A capture already running is used as it is -- the Capture tab's, or
    // this one's from before -- and never stopped or started again from here.
    // Only its wrappers are opened now.
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

    // A dead capture of ours (2026.11.24, position_reattach): stopped here,
    // on the worker -- a stop joins its threads -- and nothing more. A later
    // look's job starts another once its wait is over (position_started),
    // the world afresh and the adapter looked up again: the game's traffic
    // may go another way now, and what came while it was deaf is not known.
    if (in.stop_first) {
        if (capture_.running() && capture_.dead()) {
            capture_.stop();
            iface_by_ip_.clear();
            out.stopped = true;
        }
        return done();
    }

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
    // Started again for the game's TCP gone quiet (2026.11.24): its traffic
    // may go by another adapter now -- a VPN up or down -- so the adapter is
    // looked up afresh, not taken from what the address gave before.
    if (out.why.find("no game TCP") != std::string::npos) iface_by_ip_.erase(local_ip);
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
    // A dead capture the job stopped (2026.11.24, the owner: "cần thay cơ chế
    // này bằng 1 cơ chế khác"): started again as after a start the capture
    // refused -- for the same game link once position_backoff_ms_ is over
    // (10 s, 20 s, then every 30 s while it keeps failing), a new game link
    // at once. Until then it stayed counted as running, deaf, until the
    // game's process or connection changed. Said once a row.
    if (out.stopped) {
        capture_running_ = false;
        position_capture_ = false;
        position_tried_ms_ = now_ms();
        position_start_failed_ = true;
        position_backoff_ms_ = GameLink::refused_wait_after(position_backoff_ms_);
        const LogLevel level = position_dead_said_ ? LogLevel::Debug : LogLevel::Warn;
        position_dead_said_ = true;
        Logger::instance().logf(level,
                                "Positions: the capture for positions stopped hearing the game -- its read failed (the "
                                "line above says why); stopped, and started again in %llu s, every %llu s at most "
                                "while it keeps failing -- the hunter goes by the screen meanwhile",
                                static_cast<unsigned long long>(position_backoff_ms_ / 1000),
                                static_cast<unsigned long long>(GameLink::kRefusedMaxMs / 1000));
        return;
    }
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
        // The next silence restart waits for TCP heard again -- or, quiet
        // still, GameLink::kReattachSilentAgainMs after this start, the
        // adapter looked up afresh (position_reattach; 2026.11.24, once per
        // quiet spell until then).
        const bool silence = out.why.find("no game TCP") != std::string::npos;
        if (silence && !silence_restart_armed_ && !silence_repeat_said_) {
            silence_repeat_said_ = true;
            GB_INFO("Positions: starting the capture again -- %s, and none heard since the last start for it either "
                    "(again every %llu s while the game holds its connection, said once), the adapter looked up afresh",
                    out.why.c_str(), static_cast<unsigned long long>(GameLink::kReattachSilentAgainMs / 1000));
        } else if (silence && !silence_restart_armed_)
            GB_DEBUG("Positions: starting the capture again -- %s (quiet still)", out.why.c_str());
        else
            GB_INFO("Positions: starting the capture again -- %s", out.why.c_str());
        if (silence) silence_restart_armed_ = false;
    }
    if (!out.started) {
        // Refused by the capture itself -- an adapter it cannot use, no
        // rights. The same sockets would be refused the same way, the
        // capture's own lines in the log again (said once a row since
        // 2026.11.24, PacketCapture's trouble_said_): tried again at once
        // only for a new process, connection or UDP port, and otherwise after
        // 10 s, 20 s, then every 30 s (GameLink::refused_wait_after,
        // position_reattach; twice as long each time up to 10 min until
        // 2026.11.24).
        capture_running_ = false;
        position_capture_ = false;
        position_start_failed_ = true;
        position_backoff_ms_ = GameLink::refused_wait_after(position_backoff_ms_);
        position_say("the capture for positions would not start (the lines above say why) -- the hunter goes "
                     "by the screen, and it is tried again every 30 s at most");
        return;
    }
    // Started. The row of refusals and deaths ends once it has heard the
    // game's TCP (position_reattach), not here: one that dies at once waits
    // longer each time (2026.11.24).
    position_start_failed_ = false;
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
    constexpr float kSlack = 64.0f;     // a little off the file's edge
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
    // hitters met since HUNT ON.
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
    // capture -- the Capture tab's -- is theirs, as it is.
    if (position_capture_off_) return;
    if (capture_.running() && !position_capture_) return;
    // One job at a time: the one under way says how it went, and the next
    // look asks again.
    if (capture_worker_.busy()) return;
    PositionJobIn in = position_job_in(now);
    const std::uint64_t since_try = in.since_try;
    if (!in.running) {
        // None runs. The last start found no game, no connection or no
        // Npcap, which logs nothing new: said once, and tried again at the
        // next look, to attach as soon as the game connects. Or the capture
        // itself refused it -- or died, and was stopped (2026.11.24) -- and
        // every try puts its lines in the log again (once a row since
        // 2026.11.24): then at once only for a new process, connection or UDP
        // port -- the job asks should_reattach -- and otherwise after 10 s,
        // 20 s, then every 30 s (up to 10 min until 2026.11.24).
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
    } else if (capture_.dead()) {
        // Ours, and dead (2026.11.24, PacketCapture::dead): its thread ended
        // on a read that failed -- the adapter gone, its driver stopped -- and
        // it hears nothing more while counted as running. The job stops it,
        // on the worker (a stop joins its threads), and does nothing more; a
        // later look starts another as after a start the capture refused
        // (position_started). Until 2026.11.24 it stayed so, deaf, until the
        // game's process or connection changed.
        in.stop_first = true;
    } else {
        // Silent since its last segment: the game's traffic has gone
        // somewhere else -- another adapter, a connection the table does not
        // show yet. One that has heard none since it started is started again
        // every kReattachSilentAgainMs as well (the 2026.11.24 review; it
        // was not, and stayed deaf for the session). After a start again for
        // it, not again until the game's TCP has been heard -- or, none heard
        // still, every
        // GameLink::kReattachSilentAgainMs after that start, the adapter
        // looked up afresh each time (2026.11.24, the owner: "cần thay cơ chế
        // này bằng 1 cơ chế khác"; once per quiet spell until then). The job
        // starts it again only while the game holds a connection to its
        // server (should_reattach).
        const PacketCapture::Stats st = capture_.stats();
        // Heard the game since it started: a row of refusals or deaths is over
        // -- the next waits 10 s again, and is said again (2026.11.24).
        if (st.last_tcp_ms != 0) {
            position_backoff_ms_ = 0;
            position_dead_said_ = false;
            silence_repeat_said_ = false;
        }
        if (!silence_restart_armed_ && st.last_tcp_ms != 0) silence_restart_armed_ = true;
        // One that has heard nothing since it started -- a hunt start, a new
        // process or connection, a dead or refused capture started again --
        // is taken as one started again for silence (the 2026.11.24 review):
        // a wrong adapter from the default_interface() fallback, or an Npcap
        // hiccup at the start, stayed deaf the whole session. Every
        // kReattachSilentAgainMs from its start, while the game holds a
        // connection (should_reattach), the adapter looked up afresh.
        if (silence_restart_armed_ && st.last_tcp_ms != 0) {
            if (now > st.last_tcp_ms) in.silent = now - st.last_tcp_ms;
        } else if (since_try >= GameLink::kReattachSilentAgainMs) {
            in.silent = since_try;
        }
    }
    // The job reads the socket tables, decides and starts the capture again
    // if it must, on the worker; positions_tick takes in what it did.
    capture_worker_.post([this, in] { position_job(in); });
}

void GuiApp::save_hunt_config() {
    const std::string path = exe_relative("configs/hunt.ini");
    // The detector's tuning goes with it, into [Detect], as it was read: one
    // edited by hand in the file is kept, and one following the window too.
    if (hunt_config_.save(path) && detect_tuning_.save(path)) {
        hunter_->set_config(hunt_config_);
        set_status("Saved " + path);
    } else {
        set_status("FAILED to save " + path, 8000);
    }
}

/// The hunter's settings, under the Attack feature.
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
                    "650 px above for Ctrl+Up (Up looks 245 px up, Up Up 413). A monster above the upper bar within "
                    "one Up gets one Up in place, attacked in the air (10 a minute, apart from these). 4 a minute, 2 "
                    "per monster; 3 that did not get up in a row, or 5 in a run with more failed than worked, "
                    "pause it for 30 s, then 60 s, then 120 s, and it goes again by itself (2026.11.15). A map "
                    "change within 10 s of a jump stops hunting and switches this "
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
        help_marker("While anything runs -- the hunter, the macro, or whatever else the simple screen's "
                    "stop button would stop -- the game is brought back to the front "
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
    // as the simple screen offers it -- on its "Tự dùng kỹ năng" row and in
    // its "Kỹ năng" window, the same switch: on as shipped since 2026.11.16b,
    // with the hunter too, and kept by Apply now and Save macro like the rest
    // of this tab.
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
        help_marker("While anything runs -- the macro, the hunter, or whatever else the simple screen's "
                    "stop button would stop -- the game is brought back to the "
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
    text_dim("Narrowing this cuts noise sharply.");
    text_dim("Find it with:  netstat -n -b    while the game is connected.");
    ImGui::Dummy(ImVec2(0, 4));

    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Server IP", server_ip_, sizeof(server_ip_));
    help_marker("Leave blank to capture all TCP on the interface. "
                "Setting it is strongly recommended: a capture mixing several "
                "connections will not tile.");

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

// ------------------------------------------------------------------ heal

void GuiApp::draw_heal_panel() {
    ImGui::SeparatorText("Potions");
    ImGui::TextWrapped("Potions are the macro's keys: \"HP below\", \"MP below\" or \"pet HP below\" under "
                       "Timed keys, or the potion rows on the simple screen -- set them there. They drink "
                       "while the macro runs.");
    bool any = false;
    for (const MacroKeyKind kind : { MacroKeyKind::HpPotion, MacroKeyKind::MpPotion, MacroKeyKind::PetPotion })
        for (const std::size_t i : macro_keys_of_kind(macro_config_.keys, kind)) {
            const MacroKey& k = macro_config_.keys[i];
            any = true;
            text_dim("%s: key %s, under %d%%%s", k.label.c_str(), InputManager::key_name(k.keycode).c_str(),
                     k.threshold_pct, k.enabled ? "" : "  (off)");
        }
    if (!any) text_dim("No potion keys yet.");
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
