// "Tự đóng thông báo" -- closing the few popups that get in the way of a run --
// as the GUI does it: the worker that looks once a second during a run, and
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
#include <mutex>
#include <string>
#include <thread>
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
    PopupsUi& ui = *popups_ui_;
    const auto leave = [&](const std::string& why) {
        if (!why.empty()) say_popups(why);
        ui.running = false;
    };

    TemplateStore store;
    if (!store.load_best(exe_relative("configs/templates.ini"))) {
        leave("Không có hình giao diện của game, nên không nhận ra được thông báo nào.");
        return;
    }
    PopupEyes eyes;
    eyes.load(store, exe_relative("configs"));
    std::string why;
    if (!eyes.ready(&why)) {
        GB_WARN("Popups: not starting -- a picture is missing: %s", why.c_str());
        leave("Thiếu hình để nhận ra thông báo (" + why + "), nên không tự đóng gì.");
        return;
    }
    Frame landmark;
    const bool have_landmark =
        TemplateStore::load_one("hpmp_indicator2", exe_relative("configs/templates.ini"), landmark);

    ScreenCapture capture;
    capture.set_label("popups");
    Frame last;                         // the closer's last look, for the HUD gate
    const auto grab = [&](Frame& f) {
        if (!capture.grab(input_.target_window(), f) || !f.valid()) return false;
        last = f;
        return true;
    };

    PopupCloser closer(input_, eyes, grab);
    closer.on_report([](const std::string& line) { GB_INFO("%s", line.c_str()); });

    // HP too low to hold the potions off for a close: the hold refuses the
    // macro's presses for as long as it lasts, a few seconds at worst. Tự
    // chat's rule, which starts a line only some way over the potion's mark,
    // so that HP hovering at the mark in a fight never waits on a popup.
    const auto hp_low = [this] {
        const float hp = hp_percent_now();
        const int floor = chat_hp_floor_.load();
        return hp >= 0.0f && hp < static_cast<float>(std::max(floor, std::min(95, floor + 10)));
    };

    // What must hold before anything is pressed: the game in front with the
    // character in the world, nothing of GoBot's holding the game, HP well
    // over the potion's mark, and nobody at the machine.
    PopupCloser::Guard guard;
    guard.gates = [&]() -> std::string {
        if (!input_.window_valid()) return "no game window";
        if (!input_.window_focused()) return "the game is not in front";
        if (hp_low()) return "HP is low";
        // The Level plate first: finding the bars writes to the log each time.
        if (!(have_landmark && AutoLogin::plate_at(last, &landmark)) && !SessionManager::hud_present(last))
            return "the HUD is not up";
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
            "ESC menu; a look every 1 s%s",
            board ? "" : " -- no picture of the check-in board's title, so the board is not looked for");
    say_popups(board ? "Đang để ý thông báo nhận quà điểm danh, bảng Sự kiện và điểm danh (bấm ESC), Yêu Quái Bách "
                       "Khoa và menu ESC do GoBot mở."
                     : "Đang để ý thông báo nhận quà điểm danh, Yêu Quái Bách Khoa và menu ESC do GoBot mở. Thiếu "
                       "hình tiêu đề bảng Sự kiện và điểm danh, nên không tắt được bảng đó.");
    std::string board_said;             // the board's last line on the tab
    while (!ui.stop) {
        for (int i = 0; i < 10 && !ui.stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (ui.stop) break;
        // Nothing is closed with another window in front, so nothing is
        // looked for then either: a frame read off the screen would be that
        // window's, and the look is the costly part of a turn.
        if (!input_.window_valid() || !input_.window_focused()) continue;
        const PopupCloser::Turn turn = closer.turn();
        const bool about_board = closer.last_about(PopupKind::CheckinBoard);
        if (turn == PopupCloser::Turn::Closed) {
            ++ui.closed;
            if (about_board) ++ui.boards;
            board_said.clear();
            say_popups(closer.last_vi());
        } else if (about_board && closer.last_vi() != board_said) {
            // The board tried and still up, put off, kept shut by something
            // else or left alone: said on the tab as it changes.
            board_said = closer.last_vi();
            say_popups(board_said);
        }
        if (!closer.up(PopupKind::CheckinBoard) && !about_board) board_said.clear();
        const LocalTime lt = local_time();
        for (const std::string& shot : shots.due(now_ms(), lt.day, lt.second, closer.up(PopupKind::CheckinBoard),
                                                 turn == PopupCloser::Turn::Closed && about_board,
                                                 crossed_in_ms_.load())) {
            // The closer's last look: this turn's, or the last after its press.
            const std::string kept = keep_popup_frame(debug_dir, last, shot, false);
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
        "- bảng Sự kiện và điểm danh, lúc vào game và lúc qua 00:00: bấm ESC sau khi bảng đứng yên 3 giây "
        "(thông báo nhận quà, nếu có, được đóng trước; không bấm vào bảng);\n"
        "- Yêu Quái Bách Khoa: nút X của nó, hoặc ESC;\n"
        "- menu ESC do chính GoBot mở.\n"
        "Không bấm gì khi đang có cửa sổ khác, khi máu thấp hoặc khi bạn đang dùng chuột/bàn phím. Bot dừng "
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
                TemplateStore store;
                if (!store.load_best(exe_relative("configs/templates.ini")))
                    return std::string("Không có hình giao diện của game.");
                PopupEyes eyes;
                eyes.load(store, exe_relative("configs"));
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
                    "Chỉ đóng: thông báo nhận quà điểm danh, bảng Sự kiện và điểm danh (bấm ESC, lúc vào game và "
                    "lúc qua 00:00), Yêu Quái Bách Khoa, và menu ESC do GoBot mở. Không bao giờ đóng hộp thoại "
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
