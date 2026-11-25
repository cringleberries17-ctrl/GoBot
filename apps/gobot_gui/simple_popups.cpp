// "Tự đóng thông báo" -- closing the few popups that get in the way of a run --
// as the GUI does it: the worker that looks every 1 to 3 s during a run, and
// its row on the Đánh quái tab with the look-only "Thử xem". The check-in
// board ("Sự kiện và điểm danh", after every login and at 00:00) is on the
// list with the rest, closed with ESC (the owner's choice, 2026-11-09): no
// tick of its own.
//
// What is on the allow-list, what keeps everything shut, and how a popup is
// closed and checked is the library's (gobot/popup_closer.hpp). Here is what
// only the window knows: the holds, the trash cleaner, Tự chat, the keeper,
// the mouse, whether the player is using the machine -- and the pictures of
// the board kept for the owner to send (CheckinShots), on this worker.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/auto_login.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"
#include "gobot/popup_closer.hpp"
#include "gobot/session_manager.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

namespace gobot {
namespace {

using namespace simple_ui;

/// The hold a close is made under, by name.
const char* const kPopupHold = "đóng thông báo";

/// Where the pictures of the board and the probe's are kept, beside the
/// program as trash_debug and quest_debug are.
const char* const kPopupDebug = "popup_debug";

/// The PC's date as yyyymmdd and the second of its day, for CheckinShots.
struct LocalTime {
    int day = 0;
    int second = 0;
};
LocalTime local_time() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    return LocalTime{ (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday,
                      tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec };
}

/// How long nobody may have touched the mouse or keyboard, with everything
/// held off, before a popup is clicked: GoBot presses nothing then, so what
/// Windows counts is a person's.
constexpr int kQuietMs = 400;
/// How long a key press of GoBot's own already under way is waited for once
/// the hold is taken -- a potion or a skill, a few dozen milliseconds. Longer,
/// and the popup is put off: the hold is not to last past
/// PopupCloser::kHoldMs, and this wait is part of it.
constexpr int kKeyUnderWayMs = 500;
/// How long the game may be behind another window, with the check-in board up
/// on the last look, before the log says the board gets no ESC for it.
constexpr int kBoardUnfocusedSayMs = 10'000;

DWORD last_input() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof info;
    return GetLastInputInfo(&info) ? info.dwTime : 0;
}

} // namespace

/// Everything the GUI keeps for the popup closer.
struct GuiApp::PopupsUi {
    bool enabled = false;               ///< [Simple] close_popups
    bool loaded = false;
    std::thread       thread;
    std::atomic<bool> running{ false };
    std::atomic<bool> stop{ false };
    std::atomic<int>  closed{ 0 };
    std::atomic<int>  boards{ 0 };      ///< of `closed`, the check-in board

    // What the worker says, for the tab; guarded by worker_mutex_.
    std::vector<std::string> log;

    /// "Thử xem": one capture and one look, off the drawing thread.
    std::future<std::string> probe;
    std::string probe_said;
};

void GuiApp::PopupsUiFree::operator()(PopupsUi* ui) const { delete ui; }

bool GuiApp::popups_running() const { return popups_ui_ && popups_ui_->running; }

void GuiApp::say_popups(const std::string& vi) {
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        std::vector<std::string>& log = popups_ui_->log;
        log.push_back(vi);
        if (log.size() > 100) log.erase(log.begin());
    }
    request_redraw();       // shown at once, as say_party's
}

void GuiApp::popups_load() {
    if (!popups_ui_) popups_ui_.reset(new PopupsUi);
    PopupsUi& ui = *popups_ui_;
    if (ui.loaded) return;
    ui.loaded = true;
    IniFile ini;
    if (ini.load(exe_relative("configs/app.ini"))) ui.enabled = ini.get_bool("Simple", "close_popups", false);
}

void GuiApp::start_popups() {
    popups_load();
    PopupsUi& ui = *popups_ui_;
    // Ticked or not on the Đánh quái tab: unticked, as it ships, nothing runs.
    if (!ui.enabled || ui.running) return;
    vitals_for_job();       // HP and MP at the run's pace before any click
    join_pumping(ui.thread);
    ui.stop = false;
    ui.running = true;
    ui.thread = std::thread([this] { popup_loop(); });
}

void GuiApp::stop_popups(bool wait) {
    if (!popups_ui_) return;
    PopupsUi& ui = *popups_ui_;
    ui.stop = true;
    if (!wait) return;      // told, not waited for: stop_jobs
    join_pumping(ui.thread);
    ui.running = false;
}

void GuiApp::popup_loop() {
    name_this_thread("popups");
    quiet_this_thread();
    PopupsUi& ui = *popups_ui_;
    const auto leave = [&](const std::string& why) {
        if (!why.empty()) say_popups(why);
        ui.running = false;
    };

    // The program's one set of pictures, not a copy of our own (2026-11-16).
    // None loaded, or one the eyes need missing: no return any more
    // (2026.11.24, the owner: "Kiểm tra các phần khác xem có phần nào không làm
    // được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác") -- the
    // worker waits, closing nothing, says so once for each thing missing, and
    // every kPicturesAgainMs asks for the set again (shared_best: a load that
    // failed is not kept, a pictures folder that changed is loaded afresh) and
    // loads the eyes from it again; it goes on as soon as they are ready. Its
    // stop -- unticking, DỪNG, F12 -- ends the wait at once. Until then Tự
    // đóng thông báo ended here until the next BẮT ĐẦU.
    constexpr int kPicturesAgainMs = 30'000;
    PopupEyes eyes;
    std::string said;
    for (;;) {
        std::shared_ptr<const TemplateStore> store = TemplateStore::shared_best(exe_relative("configs/templates.ini"));
        std::string why;
        if (store->size() == 0) {
            why = "hình giao diện";
        } else {
            eyes.load(std::move(store), exe_relative("configs"));
            if (eyes.ready(&why)) break;
        }
        if (why != said) {
            GB_WARN("Popups: a picture is missing (%s) -- nothing closed, looked for again every %d s", why.c_str(),
                    kPicturesAgainMs / 1000);
            say_popups("Thiếu hình giao diện của game (" + why + ") — GoBot thử lại sau " +
                       std::to_string(kPicturesAgainMs / 1000) + " giây.");
            said = why;
        }
        for (int slept = 0; slept < kPicturesAgainMs && !ui.stop; slept += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (ui.stop) {
            leave({});
            return;
        }
    }
    if (!said.empty()) GB_INFO("Popups: the pictures it needs are there now -- going on");
    Frame landmark;
    const bool have_landmark =
        TemplateStore::load_one("hpmp_indicator2", exe_relative("configs/templates.ini"), landmark);

    ScreenCapture capture;
    capture.set_label("popups");
    // The HUD gate and the board's pictures read the closer's own last look
    // (PopupCloser::last_look): no 3 MB copy of every grab here.
    const auto grab = [&](Frame& f) { return capture.grab(input_.target_window(), f) && f.valid(); };

    PopupCloser closer(input_, eyes, grab);
    closer.on_report([](const std::string& line) { GB_INFO("%s", line.c_str()); });
    // The check-in board's ESC (the owner, 2026-11-15: "vào tới màn hình game
    // thì sẽ chờ 30 giây rồi mới ấn ESC ... Trường hợp qua ngày thì chờ tới
    // 00:03 mới ấn ESC"): the keeper's last arrival in the world, on now_ms()'s
    // steady clock -- the closer's own -- and the PC's time of day
    // (PopupCloser::board_due). A login by hand has no arrival: the board's
    // first sight stands for it.
    {
        PopupCloser::Clock clock;
        clock.arrived_ms = [this] { return crossed_in_ms_.load(); };
        clock.local_second = [] { return local_second_of_day(); };
        closer.set_clock(clock);
    }

    // At any HP (2026.11.12b, the owner: Tự đóng thông báo at every HP level):
    // the hold refuses the macro's presses for as long as a close lasts, a
    // few seconds at worst. Only the character down -- HP read under
    // bag::kJobDownHpPct, as for the bag's jobs -- holds it back. Until
    // 2026.11.12 HP had to be 10 over the HP potion's mark.
    // But the check-in board -- closed by ESC, nothing clicked -- with the
    // character down when the reviver's last look saw no death panel
    // (2026.11.21, the audit's B10; checkin_down_esc): the board drawn over a
    // panel already up hid it from the reviver, and the two waited on each
    // other until a person pressed ESC. Asked as the closer's turn is about
    // the board (last_about is set before the gates are asked), so a notice's
    // click, Yêu Quái Bách Khoa's X, never goes with the character down --
    // only the ESC menu's X, should the board's own ESC bring that menu up.
    bool said_down_esc = false;
    const auto hp_low = [this, &closer, &said_down_esc] {
        const float hp = hp_percent_now();
        // Said once a down stretch, not once a run (the review of 2026.11.21):
        // GoBot runs for nights, and a harmless first time must not leave the
        // board over the death panel at 00:00 unsaid. HP read up, not unknown
        // for a moment.
        if (hp >= static_cast<float>(bag::kJobDownHpPct)) said_down_esc = false;
        if (!(hp >= 0.0f && hp < static_cast<float>(bag::kJobDownHpPct))) return false;
        if (checkin_down_esc(closer.last_about(PopupKind::CheckinBoard), revive_running_.load(),
                             revive_no_panel_ms_.load(), now_ms())) {
            if (!said_down_esc)
                GB_INFO("Popups: the character reads down, and the reviver's last look saw no death panel -- the "
                        "check-in board may be over it: its ESC allowed");
            said_down_esc = true;
            return false;
        }
        return true;
    };

    // What must hold before anything is pressed: the game in front with the
    // character in the world, nothing of GoBot's holding the game, the
    // character not down (2026.11.12b), and nobody at the machine.
    PopupCloser::Guard guard;
    guard.gates = [&]() -> std::string {
        if (!input_.window_valid()) return "no game window";
        if (!input_.window_focused()) return "the game is not in front";
        if (hp_low()) return "the character is down";
        // The Level plate first: finding the bars writes to the log each time.
        const Frame& last = closer.last_look();
        if (!(have_landmark && AutoLogin::plate_at(last, &landmark)) && !SessionManager::hud_present(last))
            return "the HUD is not up";
        // A dungeon run's boss fight or reward room (2026.11.21, the audit's
        // B6) is no gate here, as it is for the other features' timed jobs:
        // what this closes is a box over the game, which the fight and the
        // reward's NPC need gone, under a hold of PopupCloser::kHoldMs at most;
        // the expedition's own hold keeps it out while its keys go (below).
        if (coord_.critical_active()) return "a hold is on (" + coord_.critical_reason() + ")";
        if (trash_pass_ || trash_hold_ || trash_left_held_) return "the trash cleaner holds the game";
        if (chat_doubt_) return "the chat line may be open";
        if (keeper_cold_start_) return "the keeper is starting the game";
        {
            std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
            if (keeper_flow_ != nullptr) return "the keeper is crossing";
        }
        return {};
    };
    std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::defer_lock);
    bool held = false;
    guard.take = [&]() {
        if (!take_hold(kPopupHold)) return false;
        held = true;
        if (!mouse.try_lock() || trash_pass_) {
            if (mouse.owns_lock()) mouse.unlock();
            end_hold(kPopupHold);
            held = false;
            return false;
        }
        // Nothing of GoBot's presses anything now; wait for a key press under
        // way to finish, and let go of every key.
        for (int i = 0; i < kKeyUnderWayMs / 50 && coord_.held() && !ui.stop; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        input_.release_all();
        const DWORD quiet = last_input();
        std::this_thread::sleep_for(std::chrono::milliseconds(kQuietMs));
        // HP looked at again, the moment before the press: it may have fallen
        // while the hold was being taken.
        if (ui.stop || coord_.held() || last_input() != quiet || hp_low()) {
            mouse.unlock();
            end_hold(kPopupHold);
            held = false;
            return false;
        }
        return true;
    };
    guard.done = [&] {
        if (mouse.owns_lock()) mouse.unlock();
        if (held) end_hold(kPopupHold);
        held = false;
    };
    // F12, DỪNG or unticking it while the fresh frame is looked at: nothing
    // more pressed, and the window's join not kept waiting on a click.
    guard.stopped = [&] { return ui.stop.load(); };
    closer.set_guard(guard);

    // Pictures of the board for the owner to send, kept between turns -- never
    // between a hold and its press, which turn() lets go of before it returns.
    const std::string debug_dir = exe_relative(kPopupDebug);
    CheckinShots shots;
    {
        const LocalTime lt = local_time();
        shots.start(now_ms(), lt.day, crossed_in_ms_.load());
    }

    const bool board = eyes.sees_board();
    GB_INFO("Popups: watching for the check-in notice, the check-in board (ESC), Yêu Quái Bách Khoa and GoBot's own "
            "ESC menu; a look every %d s while one is up, every %d s after a look that saw nothing (%d s in light "
            "mode)%s",
            PopupCloser::kLookMs / 1000, PopupCloser::kIdleLookMs / 1000, PopupCloser::kIdleLookLightMs / 1000,
            board ? "" : " -- no picture of the check-in board's title, so the board is not looked for");
    say_popups(board ? "Đang để ý thông báo nhận quà điểm danh, bảng Sự kiện và điểm danh (bấm ESC), Yêu Quái Bách "
                       "Khoa và menu ESC do GoBot mở."
                     : "Đang để ý thông báo nhận quà điểm danh, Yêu Quái Bách Khoa và menu ESC do GoBot mở. Thiếu "
                       "hình tiêu đề bảng Sự kiện và điểm danh, nên không tắt được bảng đó.");
    std::string board_said;             // the board's last line on the tab
    // A look a second while anything is up, and after one that saw nothing
    // every 2 s -- 3 s in light mode, read each time from print_window_verdicts,
    // the copy of it any thread may read: it can be ticked at any moment
    // (PopupCloser::next_look_ms, 2026-11-16). Slept in 100 ms steps, so that
    // a stop is not kept waiting.
    int wait_ms = PopupCloser::kLookMs;
    // The check-in board up on the last look and no turn since, the game
    // behind another window: said once, after kBoardUnfocusedSayMs, so that a
    // board left up is never left so with nothing in the log (2026.11.17c).
    std::uint64_t unfocused_from = 0;
    bool said_unfocused = false;
    while (!ui.stop) {
        for (int slept = 0; slept < wait_ms && !ui.stop; slept += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (ui.stop) break;
        // Nothing is closed with another window in front, so nothing is
        // looked for then either: a frame read off the screen would be that
        // window's, and the look is the costly part of a turn.
        if (!input_.window_valid() || !input_.window_focused()) {
            if (closer.up(PopupKind::CheckinBoard) && !said_unfocused) {
                if (unfocused_from == 0) unfocused_from = now_ms();
                else if (now_ms() - unfocused_from >= static_cast<std::uint64_t>(kBoardUnfocusedSayMs)) {
                    said_unfocused = true;
                    GB_INFO("Popups: the check-in board is up and gets no ESC: the game has not been in front for %d s "
                            "-- nothing is looked at or pressed until it is",
                            kBoardUnfocusedSayMs / 1000);
                    say_popups("Bảng Sự kiện và điểm danh đang hiện, nhưng game không ở trên cùng: chưa bấm ESC.");
                }
            }
            continue;
        }
        unfocused_from = 0;
        const PopupCloser::Turn turn = closer.turn();
        wait_ms = PopupCloser::next_look_ms(turn, print_window_verdicts().light());
        const bool about_board = closer.last_about(PopupKind::CheckinBoard);
        if (turn == PopupCloser::Turn::Closed) {
            ++ui.closed;
            if (about_board) ++ui.boards;
            board_said.clear();
            say_popups(closer.last_vi());
        } else if (about_board && closer.last_vi() != board_said) {
            // The board tried and still up -- tried again 30 s on, as often as
            // it takes, since 2026.11.24, never left alone -- put off, or kept
            // shut by something else: said on the tab as it changes.
            board_said = closer.last_vi();
            say_popups(board_said);
        }
        if (!closer.up(PopupKind::CheckinBoard) && !about_board) {
            board_said.clear();
            said_unfocused = false;
        }
        // No look, no picture: kept for the next turn that has one.
        if (turn == PopupCloser::Turn::Blind) continue;
        const LocalTime lt = local_time();
        for (const std::string& shot : shots.due(now_ms(), lt.day, lt.second, closer.up(PopupKind::CheckinBoard),
                                                 turn == PopupCloser::Turn::Closed && about_board,
                                                 crossed_in_ms_.load())) {
            // The closer's last look: this turn's, or the last after its press.
            const std::string kept = keep_popup_frame(debug_dir, closer.last_look(), shot, false);
            if (kept.empty())
                GB_WARN("Popups: could not keep a picture (%s) in %s", shot.c_str(), debug_dir.c_str());
            else
                GB_INFO("Popups: kept %s (board %s)", kept.c_str(),
                        closer.up(PopupKind::CheckinBoard) ? "seen" : "not seen");
        }
    }
    leave({});
}

std::string GuiApp::simple_popups_line() const {
    if (!popups_running()) return {};
    const int n = popups_ui_->closed.load(), boards = popups_ui_->boards.load();
    return "Tự đóng thông báo: đang bật" + (n > 0 ? ", đã đóng " + std::to_string(n) : std::string()) +
           (boards > 0 ? " (bảng điểm danh: " + std::to_string(boards) + ")" : std::string()) + ".";
}

void GuiApp::simple_popups_row() {
    popups_load();
    PopupsUi& ui = *popups_ui_;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::Checkbox("Tự đóng thông báo", &ui.enabled)) {
        save_simple_option("close_popups", ui.enabled);
        if (ui.enabled && simple_running() && !simple_cold_start_) start_popups();
        if (!ui.enabled) stop_popups();
    }
    ImGui::SetItemTooltip("%s", trial_hint(kTrialPopups,
        "Trong lúc bot chạy, GoBot tự đóng vài thông báo hay chắn đường, sau khi chúng đứng yên 3 giây:\n"
        "- thông báo nhận quà điểm danh: bấm Xác Nhận của nó;\n"
        "- bảng Sự kiện và điểm danh, lúc vào game và lúc qua 00:00: bấm ESC 30 giây sau khi vào game; "
        "qua ngày thì chờ tới 00:03 mới bấm ESC (thông báo nhận quà, nếu có, được đóng trước; không bấm vào "
        "bảng);\n"
        "- Yêu Quái Bách Khoa: nút X của nó, hoặc ESC;\n"
        "- menu ESC do chính GoBot mở.\n"
        "Không bấm gì khi đang có cửa sổ khác, khi nhân vật đã gục hoặc khi bạn đang dùng chuột/bàn phím "
        "(đóng ở mọi mức máu). Bot dừng "
        "thì không bấm gì. Khi bật, GoBot lưu vài ảnh màn hình game vào thư mục popup_debug (15 giây sau khi "
        "GoBot tự vào lại game, 15 giây sau khi bấm BẮT ĐẦU, lúc qua 00:00 và khi thấy bảng) để kiểm tra bảng "
        "điểm danh.").c_str());
    ImGui::TableNextColumn();
    {
        const bool busy = ui.probe.valid();
        ImGui::BeginDisabled(busy || !input_.window_valid());
        if (ImGui::Button("Thử xem##popups")) {
            ui.probe_said.clear();
            // The tick as it is now, for what the board would get; the
            // picture is kept from the probe's own thread.
            const bool ticked = ui.enabled;
            ui.probe = std::async(std::launch::async, [this, ticked] {
                std::shared_ptr<const TemplateStore> store =
                    TemplateStore::shared_best(exe_relative("configs/templates.ini"));
                if (store->size() == 0) return std::string("Không có hình giao diện của game.");
                PopupEyes eyes;
                eyes.load(std::move(store), exe_relative("configs"));
                std::string why;
                if (!eyes.ready(&why)) return "Thiếu hình để nhận ra thông báo: " + why + ".";
                ScreenCapture capture;
                capture.set_label("popups");
                Frame f;
                if (!capture.grab(input_.target_window(), f) || !f.valid())
                    return std::string("Không chụp được màn hình game.");
                const std::string kept = keep_popup_frame(exe_relative(kPopupDebug), f, {}, true);
                if (!kept.empty()) GB_INFO("Popups: Thử xem kept %s", kept.c_str());
                const std::string saved = kept.empty() ? std::string() : " Đã lưu ảnh vào thư mục popup_debug.";
                const std::vector<PopupSeen> up = eyes.popups(f);
                if (up.empty())
                    return "Không thấy thông báo nào trong danh sách được đóng. Không bấm gì." + saved;
                std::string out = "Thấy: ";
                for (std::size_t i = 0; i < up.size(); ++i) {
                    // The menu seen here is anybody's -- most likely the
                    // player's -- so it is not called GoBot's.
                    out += (i ? ", " : "") + std::string(up[i].kind == PopupKind::EscMenu
                                                             ? "menu ESC"
                                                             : popup_kind_vi(up[i].kind));
                    if (up[i].kind == PopupKind::EscMenu) out += " (GoBot không đóng menu do bạn mở)";
                    else if (up[i].kind == PopupKind::CheckinNotice) out += " (sẽ bấm Xác Nhận của nó)";
                    else if (up[i].by_esc)
                        out += " ở " + std::to_string(up[i].at.x) + "," + std::to_string(up[i].at.y) +
                               (ticked ? " (sẽ bấm ESC)" : " (sẽ bấm ESC khi bật Tự đóng thông báo)");
                    else out += up[i].control ? " (sẽ bấm nút X của nó)" : " (sẽ bấm ESC nếu không có gì khác)";
                }
                std::vector<std::string> other;
                eyes.blockers(f, up, &other);
                if (!other.empty()) out += ". Nhưng lúc này sẽ không đóng gì: " + other.front();
                return out + ". Không bấm gì." + saved;
            });
        }
        ImGui::EndDisabled();
        if (ui.probe.valid() && ui.probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            ui.probe_said = ui.probe.get();
        ImGui::SetItemTooltip("Chụp màn hình game một lần và nói thấy gì, sẽ đóng gì -- không bấm gì cả. Ảnh "
                              "chụp được lưu vào thư mục popup_debug.");
    }
    // The check-in board ("Sự kiện và điểm danh") is on the list since
    // 2026.11.09, folded into this tick: closed with ESC, never by its X.
    hint(trial_hint(kTrialPopups,
                    "Chỉ đóng: thông báo nhận quà điểm danh, bảng Sự kiện và điểm danh (bấm ESC 30 giây sau khi vào "
                    "game; qua ngày thì sau 00:03), Yêu Quái Bách Khoa, và menu ESC do GoBot mở. Không bao giờ đóng hộp thoại "
                    "thùng rác, lời mời tổ đội, bảng hồi sinh, cửa sổ nhiệm vụ hay cửa sổ lạ.").c_str());
    if (!ui.probe_said.empty()) hint(ui.probe_said.c_str());
    std::string last;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (!ui.log.empty()) last = ui.log.back();
    }
    if (!last.empty()) hint(last.c_str());
}

} // namespace gobot
