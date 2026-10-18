// The keeper's look between crashes: "Vào game ngay" asked for with its key,
// and the watchdog for a game that runs and answers with nobody in the world.
//
// keeper_loop asks keeper_wake() on every look, with what CrashWatch made of
// it, and goes on as it says: ends the client and crosses back in when the
// watchdog says so, as for a crash. What a stall is, is the library's
// (gobot/stall_watch.hpp). Here is what only the window knows: the key's
// request and its cancel, the one-shot keeper the key starts when no keeper
// is watching, the holds the watchdog waits out, and the option on the Tự
// đăng nhập tab.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/alerts.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/hotkeys.hpp"
#include "gobot/logger.hpp"
#include "gobot/stall_watch.hpp"

#include "imgui.h"

#include <algorithm>
#include <mutex>
#include <string>

#include <windows.h>

namespace gobot {
namespace {

using namespace simple_ui;

/// How often the keeper reads the watchdog's settings again, so the tab's
/// tick counts while it runs.
constexpr std::uint64_t kStallSettingsMs = 60'000;

} // namespace

/// What the keeper keeps for the watchdog: the keeper thread's alone, made
/// afresh for each keeper (start_keeper).
struct GuiApp::KeeperUi {
    StallKeeper   stall;
    std::uint64_t settings_read_ms = 0;
};

void GuiApp::KeeperUiFree::operator()(KeeperUi* ui) const { delete ui; }

void GuiApp::keeper_stall_restart() {
    if (keeper_ui_) keeper_ui_->stall.restart(now_ms());
}

GuiApp::KeeperWake GuiApp::keeper_wake(CrashWatch::Move move, CrashWatch& watch,
                                       const CrashWatch::Settings& watch_settings,
                                       const AutoLogin::Settings& login, TemplateStore& store,
                                       CrashWatch::Look& seen) {
    if (!keeper_ui_) {
        keeper_ui_.reset(new KeeperUi);
        // A keeper just started: the game may only now be coming up.
        keeper_ui_->stall.restart(now_ms());
    }
    KeeperUi& ui = *keeper_ui_;
    const bool fine = move == CrashWatch::Move::Watch || move == CrashWatch::Move::Follow;

    // A keeper started for the key alone ends no client: the player asked
    // for a crossing, not for a crashed or hung game to be ended.
    if (!fine) {
        if (!keeper_oneshot_) return KeeperWake::None;
        GB_WARN("Keeper: the login key's crossing is not made -- %s", seen.detail.c_str());
        say_keeper("login key: not crossing -- " + seen.detail);
        say_keeper_vi("Game đang không chạy bình thường nên GoBot không vào game. Hãy xem lại game.",
                      "Game đang không chạy bình thường nên GoBot không vào game. Hãy xem lại game.");
        return KeeperWake::Quit;
    }

    // The key's request taken up, and from this moment its crossing: the key
    // pressed again while the crossing gets going -- a frame grabbed, login.ini
    // read, the pictures loaded -- cancels it rather than asking again.
    bool asked = false;
    {
        std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
        asked = keeper_login_now_.exchange(false);
        if (asked) keeper_key_crossing_ = true;
    }
    if (asked) {
        const bool go_on = keeper_key_crossing(watch, watch_settings, login, store);
        {
            std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
            keeper_key_crossing_ = false;
        }
        // In the world, by the key or as it was found: the count starts afresh.
        ui.stall.restart(now_ms());
        return go_on ? KeeperWake::None : KeeperWake::Quit;
    }
    // A one-shot keeper with nothing asked of it any more has done its job.
    if (keeper_oneshot_) return KeeperWake::Quit;

    // The watchdog, when its tick on the Tự đăng nhập tab is on.
    if (now_ms() >= ui.settings_read_ms) {
        ui.settings_read_ms = now_ms() + kStallSettingsMs;
        IniFile ini;
        StallSettings s;
        if (ini.load(exe_relative("configs/app.ini"))) s.load(ini);
        ui.stall.set_settings(s, exe_relative("configs/templates.ini"));
    }
    // A game a cold start found up already: the HUD may come later.
    if (move == CrashWatch::Move::Follow) {
        ui.stall.restart(now_ms());
        return KeeperWake::None;
    }
    // GoBot's own hands on the game count as nobody looking: a hold, a trash
    // pass or its doubt, a chat line in doubt.
    const bool held = coord_.critical_active() || trash_pass_ || trash_hold_ || trash_left_held_ || chat_doubt_;
    // The window GoBot follows, looked at only while it is the game's own by
    // where its program runs from: it may have been picked by hand.
    if (!ui.stall.check(input_.target_window(), watch_settings.game_process, held, login, store, now_ms(),
                        seen, exe_relative("keeper_debug")))
        return KeeperWake::None;
    return KeeperWake::Stalled;
}

void GuiApp::simple_stall_option() {
    const std::string cfg = exe_relative("configs/app.ini");
    if (!stall_tab_loaded_) {
        stall_tab_loaded_ = true;
        IniFile ini;
        StallSettings s;
        if (ini.load(cfg)) s.load(ini);
        stall_tab_on_ = s.enabled;
        stall_tab_min_ = s.stall_min;
    }
    ImGui::Indent(mark_indent());
    // It works only inside the keeper, so only with "Tự vào lại game" on.
    ImGui::BeginDisabled(!simple_opts_.keep_alive);
    if (ImGui::Checkbox("Cũng vào lại khi game vẫn chạy mà không thấy nhân vật quá", &stall_tab_on_)) {
        if (!IniFile::set_in_place(cfg, "Crash", "stall_enabled", stall_tab_on_ ? "true" : "false"))
            simple_note("Không lưu được configs/app.ini.", 8000);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("##stall_min", &stall_tab_min_, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        stall_tab_min_ = std::clamp(stall_tab_min_, StallSettings::kMinMinutes, StallSettings::kMaxMinutes);
        if (!IniFile::set_in_place(cfg, "Crash", "stall_min", stall_tab_min_))
            simple_note("Không lưu được configs/app.ini.", 8000);
    }
    ImGui::SameLine();
    label("phút");
    ImGui::EndDisabled();
    hint(trial_hint(kTrialStall,
                    "Không bao giờ vào lại vì: nhân vật chết, cửa sổ game bị che hay thu nhỏ, hoặc GoBot đang "
                    "làm việc khác trong game. Game đang tải thì chờ tới 15 phút mới vào lại. Mỗi lần vào lại "
                    "như vậy được tính vào số lần vào lại tối đa. Chỉ chạy khi bật \"Tự vào lại game\".").c_str());
    ImGui::Unindent(mark_indent());
}

bool GuiApp::keeper_key_crossing(CrashWatch& watch, const CrashWatch::Settings& watch_settings,
                                 const AutoLogin::Settings& login, TemplateStore& store) {
    // Stopping for a person, as keeper_loop does: the English for the log and
    // the panel, the Vietnamese for the simple screen, kept after it stops.
    const auto give_up = [this](const std::string& english, const std::string& vi) {
        GB_WARN("Keeper: %s", english.c_str());
        say_keeper(english);
        say_keeper_vi(vi, vi);
    };
    // What comes after: a one-shot keeper ends, one that watches goes on.
    // Asked each time, not once: BẮT ĐẦU pressed meanwhile turns a one-shot
    // keeper into the run's own (start_keeper).
    const auto watching = [this] { return !keeper_oneshot_; };

    // In the world already? Then there is nothing to cross. The HP reader
    // cannot always say -- it may not be running -- so the game is looked at.
    {
        Frame landmark;
        const bool have_landmark =
            TemplateStore::load_one("hpmp_indicator2", exe_relative("configs/templates.ini"), landmark);
        ScreenCapture capture;
        Frame f;
        if (capture.grab(input_.target_window(), f) && f.valid() &&
            AutoLogin::in_world(f, have_landmark ? &landmark : nullptr)) {
            GB_INFO("Keeper: login key -- the character is in the world already");
            say_keeper("login key: the character is in the world already");
            say_keeper_vi(hotkey_text::kInWorld);
            return watching();
        }
    }

    // The keeper's own checks, as for any crossing; login.ini read afresh,
    // as keeper_loop reads it for every crossing.
    AutoLogin::Settings fresh = login;
    {
        AutoLogin::Settings again;
        if (again.load(exe_relative("configs/login.ini"))) fresh = again;
    }
    if (!fresh.unidentified().empty()) {
        std::string pages;
        for (const AutoLogin::Page* page : fresh.unidentified())
            pages += (pages.empty() ? "" : ", ") + page->key;
        give_up("the login key's crossing is not set up -- see configs/login.ini",
                AutoLogin::advice(AutoLogin::Why::NotSetUp, pages));
        return false;
    }
    if (!elevated_) {
        give_up("GoBot is not running as administrator, so its clicks cannot reach VTCZone or the game",
                "GoBot không chạy bằng quyền Administrator nên không bấm được vào VTCZone/game.");
        return false;
    }
    // Pictures named since the keeper loaded its own: these, for this one.
    TemplateStore own;
    TemplateStore* pictures = &store;
    if (AutoLogin::picture_names(fresh) != AutoLogin::picture_names(login)) {
        TemplateStore all;
        AutoLogin::load_pictures(all, exe_relative("configs/templates.ini"));
        own = AutoLogin::pictures_for(fresh, all);
        pictures = &own;
    }

    // The crossing, as keeper_loop makes one -- without ending the client
    // first, or counting it as a restart: nothing fell over. Only if the
    // crossing itself ends a client stuck on a screen it does not know is
    // that a restart -- and only by a keeper the player ticked "Tự vào lại
    // game" for. A keeper started for the key alone never ends the game: the
    // player asked for a crossing, not for the client to be closed, and a
    // stuck attempt there simply fails.
    GB_INFO("Keeper: the login key -- crossing now");
    say_keeper("login key: crossing now");
    AutoLogin flow;
    flow.set_settings(fresh);
    flow.set_templates(pictures);
    if (watching())
        flow.set_end_client(
            [this, &watch] {
                ++keeper_restarts_;
                return watch.shut_down(CrashWatch::Look{});
            },
            watch_settings.cooldown_s);
    const std::string doing = "Đang vào game (phím vào game ngay): ";
    flow.on_step([this, doing, &flow](AutoLogin::Step s, const std::string& detail) {
        say_keeper(std::string(AutoLogin::step_name(s)) + ": " + detail);
        if (s == AutoLogin::Step::Blocked || s == AutoLogin::Step::InWorld) return;
        std::string vi = doing + AutoLogin::step_name_vi(s) + "…";
        if (flow.holding())
            vi += " " + (flow.hiding() ? AutoLogin::hidden_advice(flow.held_by())
                                       : AutoLogin::advice(AutoLogin::Why::Covered, flow.held_by()));
        say_keeper_vi(vi);
    });
    {
        std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
        keeper_flow_ = &flow;
        keeper_key_crossing_ = true;
        // Pressed again in the moment between asking and this: stopped.
        if (keeper_key_cancel_) flow.cancel();
    }
    const bool in = !keeper_stop_ && flow.run();
    {
        std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
        keeper_flow_ = nullptr;
        keeper_key_crossing_ = false;
    }
    if (keeper_stop_) return false;
    if (keeper_key_cancel_.exchange(false)) {
        GB_INFO("Keeper: the login key's crossing was cancelled by the key");
        say_keeper("login key: cancelled");
        say_keeper_vi("Đã huỷ vào game.");
        return watching();
    }
    if (!in) {
        // As any crossing that gave up: its tries are used, and the next is
        // for a person to decide.
        give_up("could not get in: " + flow.trouble(), flow.advice());
        alert("keeper_gave_up", alert_text::gave_up_crossing(flow.attempts_made(), flow.why()));
        return false;
    }
    GB_INFO("Keeper: in the world, by the login key");
    say_keeper("login key: in the world");
    say_keeper_vi(watching() ? "Đã vào game. Đang theo dõi game." : "Đã vào game.");
    keeper_reattach_ = true;
    return watching();
}

} // namespace gobot
