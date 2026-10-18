// GoBot's own keys as the window reads them -- F10, F11, F12, "Dọn túi ngay"
// and "Vào game ngay" -- and the two boxes the new ones are set in.
//
// Which window has to be in front for each, and what each does then, is the
// library's (gobot/hotkeys.hpp). Here is what only the window knows: which
// window is in front, what the rest of GoBot is doing, and the trash worker
// and the keeper the two new keys hand their job to.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/auto_login.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/hotkeys.hpp"
#include "gobot/logger.hpp"
#include "gobot/quick_quest.hpp"

#include "imgui.h"

#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

namespace gobot {
namespace {

using namespace simple_ui;

const char* const kOnlyInGame = "Chỉ có tác dụng khi cửa sổ game ở trên cùng. Chỉ dùng phím F1–F12.";

} // namespace

void GuiApp::load_hotkeys() {
    IniFile ini;
    if (ini.load(exe_relative("configs/app.ini"))) hotkeys_.load(ini);
}

HotkeyActions GuiApp::hotkeys_tick() {
    HotkeySet set;
    set.toggle = macro_config_.toggle_key;
    set.hunt = hunt_config_.toggle_key;
    set.panic = macro_config_.panic_key;
    set.trash_now = hotkeys_.trash_now;
    set.login_now = hotkeys_.login_now;
    const HotkeyEdges edges = hotkey_edges_.update(set, [](std::uint32_t vk) {
        return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    });
    if (!edges.any()) return {};

    HotkeyState s;
    const HWND front = GetForegroundWindow();
    s.front = front == nullptr                                     ? HotkeyFront::None
            : front == static_cast<HWND>(input_.target_window()) ? HotkeyFront::Game
            : front == static_cast<HWND>(main_hwnd_)             ? HotkeyFront::GoBot
                                                                 : HotkeyFront::Other;
    s.listening = simple_listening_ != 0;
    s.held = coord_.critical_active();
    // The "Dọn ngay" button's own gates, and a pass already under way.
    s.trash_busy = trash_pass_ || (trash_running_ && (trash_oneshot_ || trash_request_ != 0));
    s.chat_doubt = chat_doubt_;
    s.game_window = input_.window_valid();
    s.trash_list_empty = simple_clean_loaded_ && simple_trash_.items.empty();
    s.world = !vitals_running_                 ? HotkeyWorld::Unknown
            : vitals_.hp_percent() >= 0.0f     ? HotkeyWorld::Yes
                                               : HotkeyWorld::No;
    {
        std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
        s.crossing = keeper_flow_ != nullptr;
        // Asked for and not yet taken up by the keeper counts as this key's
        // crossing too: pressed again then, it is withdrawn, not asked twice.
        s.key_crossing = keeper_key_crossing_ || keeper_login_now_;
    }
    // The two keys against every key GoBot presses itself, as their boxes
    // check them: the full screen's macro table and a file edited by hand do
    // not. Only when one of them went down -- the list reads three files.
    if (edges.trash_now || edges.login_now) {
        const std::vector<NamedKey> taken = game_keys_in_use();
        if (const NamedKey* k = key_taken(hotkeys_.trash_now, taken)) s.trash_key_use = k->what_vi;
        if (const NamedKey* k = key_taken(hotkeys_.login_now, taken)) s.login_key_use = k->what_vi;
    }

    const HotkeyActions a = hotkey_action(edges, s);
    // Said once, and again only ten minutes on: GoBot's own presses of the key
    // come every few seconds.
    const auto say_taken = [&](bool trash) {
        const std::uint32_t vk = trash ? hotkeys_.trash_now : hotkeys_.login_now;
        const std::string& use = trash ? s.trash_key_use : s.login_key_use;
        const std::string note = hotkey_text::key_taken_vi(InputManager::key_name(vk), use, trash);
        const std::uint64_t now = now_ms();
        if (note == hotkey_taken_said_ && now - hotkey_taken_ms_ < 10 * 60'000) return;
        hotkey_taken_said_ = note;
        hotkey_taken_ms_ = now;
        GB_WARN("Hotkeys: %s is also a key GoBot presses itself (%s), so it is not heard as the %s key",
                InputManager::key_name(vk).c_str(), use.c_str(), trash ? "trash-now" : "login-now");
        if (trash) say_trash(note);
        else       say_keeper("login key: also a key GoBot presses itself, so it is not heard");
        simple_note(note, 10000);
    };
    if (a.trash_taken) say_taken(true);
    if (a.login_taken) say_taken(false);
    if (a.trash) {
        const std::string key = InputManager::key_name(hotkeys_.trash_now);
        GB_INFO("Hotkeys: %s pressed in the game -- one trash pass", key.c_str());
        say_trash("Phím " + key + ": dọn túi ngay.");
        request_trash(false);
    } else if (!a.trash_note_vi.empty()) {
        say_trash("Phím " + InputManager::key_name(hotkeys_.trash_now) + ": " + a.trash_note_vi);
        simple_note(a.trash_note_vi);
    }
    if (a.login == HotkeyActions::Login::Start) {
        GB_INFO("Hotkeys: %s pressed in the game -- one login crossing",
                InputManager::key_name(hotkeys_.login_now).c_str());
        login_now();
    } else if (a.login == HotkeyActions::Login::Cancel) {
        GB_INFO("Hotkeys: %s pressed again -- the crossing it started is cancelled",
                InputManager::key_name(hotkeys_.login_now).c_str());
        cancel_login_now();
    } else if (!a.login_note_vi.empty()) {
        simple_note(a.login_note_vi);
        say_keeper(a.login_note_vi == hotkey_text::kInWorld ? "login key: already in the world"
                                                            : "login key: a crossing is already under way");
    }
    return a;
}

void GuiApp::login_now() {
    // The keeper's own checks, before anything is started: a crossing that
    // could not click anything, or has pages it cannot tell, would only end
    // in a note anyway.
    if (!elevated_) {
        simple_note("GoBot không chạy bằng quyền Administrator nên không bấm được vào VTCZone/game.",
                    10000);
        return;
    }
    {
        AutoLogin::Settings login;
        if (login.load(exe_relative("configs/login.ini")) && !login.unidentified().empty()) {
            simple_note("Chưa cài đặt xong các màn hình vào game trong configs/login.ini, nên chưa vào "
                        "game được.", 10000);
            return;
        }
    }
    keeper_key_cancel_ = false;
    keeper_login_now_ = true;
    // A keeper watching already takes it on its next look, a few seconds
    // from now. With none, one just for this crossing, which ends in the
    // world or when the crossing gives up.
    if (keeper_running_ && !keeper_stop_) {
        simple_note("Đang vào game (phím vào game ngay)…");
        return;
    }
    start_keeper(true);
    say_keeper_vi("Đang vào game (phím vào game ngay)…");
    simple_note("Đang vào game (phím vào game ngay)…");
}

void GuiApp::cancel_login_now() {
    std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
    // A request the keeper has not taken up yet is simply withdrawn: nothing
    // starts. One under way is stopped -- only the key's own: a crossing the
    // keeper started after a crash is not this key's to stop.
    const bool withdrawn = keeper_login_now_.exchange(false);
    if (keeper_key_crossing_) {
        keeper_key_cancel_ = true;
        if (keeper_flow_) keeper_flow_->cancel();
    } else if (!withdrawn) {
        return;
    }
    simple_note("Đã huỷ vào game.");
}

std::vector<NamedKey> GuiApp::game_keys_in_use() {
    std::vector<NamedKey> k = {
        { macro_config_.toggle_key, "bật/tắt bot" },
        { macro_config_.panic_key, "dừng ngay" },
        { hunt_config_.toggle_key, "bật/tắt đánh quái" },
        { hunt_config_.attack_key, "đánh" },
        { hunt_config_.loot_key, "nhặt đồ" },
        { hunt_config_.jump_key, "nhảy" },
        { hunt_config_.left_key, "đi sang trái" },
        { hunt_config_.right_key, "đi sang phải" },
    };
    for (const MacroKey& m : macro_config_.keys) {
        if (m.keycode == 0) continue;
        const char* what = "phím kỹ năng";
        switch (macro_key_kind(m)) {
            case MacroKeyKind::HpPotion: what = "bình HP"; break;
            case MacroKeyKind::MpPotion: what = "bình MP"; break;
            case MacroKeyKind::Buff:     what = "buff"; break;
            case MacroKeyKind::Skill:    what = "phím kỹ năng"; break;
        }
        k.push_back({ m.keycode, what });
    }
    for (const MacroStep& s : macro_config_.sequence)
        if (s.keycode != 0) k.push_back({ s.keycode, "chuỗi phím của bot" });
    // The bag's key, the quest window's and the healing keys the crossing
    // presses on arrival, from their own files: the tabs that set them may
    // not have been opened. GoBot presses each of them itself.
    {
        CleanSettings clean;
        k.push_back({ clean.load(exe_relative("configs/trash.ini")) ? clean.bag_key : simple_clean_.bag_key,
                      "mở túi đồ" });
        QuestSettings quest;
        quest.load(exe_relative("configs/quest.ini"));
        k.push_back({ quest.quest_key, "mở cửa sổ Nhiệm Vụ" });
        AutoLogin::Settings login;
        if (login.load(exe_relative("configs/login.ini"))) {
            k.push_back({ login.heal_hp_key, "bình HP khi vào game" });
            k.push_back({ login.heal_mp_key, "bình MP khi vào game" });
        }
    }
    return k;
}

std::string GuiApp::own_key_clash_vi(std::uint32_t got, std::uint32_t mine) const {
    // The bot's own switches cannot double as game keys: pressing a potion
    // key would stop the bot -- and GoBot's own press of one of the two
    // one-job keys would empty the bag or start a crossing.
    HotkeySet own;
    own.toggle = macro_config_.toggle_key;
    own.panic = macro_config_.panic_key;
    own.hunt = hunt_config_.toggle_key;
    own.trash_now = hotkeys_.trash_now;
    own.login_now = hotkeys_.login_now;
    const char* use = own_key_use_vi(got, mine, own);
    if (use == nullptr) return {};
    return "Phím " + InputManager::key_name(got) + " dùng để " + use + ". Hãy chọn phím khác.";
}

void GuiApp::simple_hotkey_box(bool trash) {
    const char* const key_name = trash ? "trash_now" : "login_now";
    std::uint32_t& vk = trash ? hotkeys_.trash_now : hotkeys_.login_now;
    label(trash ? "Phím dọn ngay" : "Phím vào game ngay");
    ImGui::SameLine();
    std::uint32_t key = vk;
    // The box itself refuses GoBot's own switches and the other new key.
    if (simple_key_box(key_name, key, true, false, kOnlyInGame)) {
        const NamedKey* clash = key_taken(key, game_keys_in_use());
        if (clash != nullptr) {
            simple_note("Phím " + InputManager::key_name(key) + " đang dùng cho " + clash->what_vi +
                        ". Hãy chọn phím khác.", 8000);
        } else if (!one_job_key_allowed(key)) {
            // A letter or digit is also typed into the game's chat line: the
            // player writing it there would empty the bag mid-sentence. Enter
            // and Esc, which answer dialogs, are no function keys either.
            simple_note("Hãy chọn phím F1–F12: phím chữ, số hay dấu cũng là chữ gõ vào ô chat của game.",
                        8000);
        } else {
            vk = key;
            if (!HotkeyConfig::save_key(exe_relative("configs/app.ini"), key_name, key))
                simple_note("Không lưu được configs/app.ini.", 8000);
            else
                GB_INFO("Hotkeys: %s is now %s", key_name,
                        key ? InputManager::key_name(key).c_str() : "no key");
        }
    }
    ImGui::SameLine();
    hint(trial_hint(kTrialHotkeys, kOnlyInGame).c_str());
}

std::string GuiApp::simple_hotkeys_line() const {
    std::string line;
    if (hotkeys_.trash_now != 0)
        line += "     ·     " + InputManager::key_name(hotkeys_.trash_now) + ": dọn túi ngay";
    if (hotkeys_.login_now != 0)
        line += "     ·     " + InputManager::key_name(hotkeys_.login_now) + ": vào game ngay";
    return line;
}

} // namespace gobot
