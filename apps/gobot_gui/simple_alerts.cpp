// The alerts, as the GUI does them: the one hook every part of GoBot raises an
// alert through, the watches that notice what no worker says out loud, the
// beep and the Windows notification on this machine, and the Thông báo tab.
//
// The sending, the merging and the stored webhook are the library's
// (gobot/alerts.hpp). Nothing here touches the network or DPAPI: the tab hands
// the service a pasted webhook and the service's own thread encrypts it, so the
// window never waits on either.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/alerts.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include "imgui.h"
#include "imgui_internal.h"     // ImGui's own copies of a pasted webhook, to wipe them

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>
#include <shellapi.h>           // Shell_NotifyIconW

namespace gobot {
namespace {

using namespace simple_ui;

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/// The beep and the Windows notification, from GoBot's own window.
///
/// The notification is a balloon from an icon of GoBot's in the notification
/// area, which Windows 10 shows as a toast. It takes no focus -- the game stays
/// in front and keeps its keys -- and neither does anything else here: no
/// message box, no window brought forward. The icon goes in the first time it
/// is needed and comes out when GoBot closes.
class WindowsNotifier final : public LocalNotifier {
public:
    explicit WindowsNotifier(void* hwnd) : hwnd_(static_cast<HWND>(hwnd)) {}
    ~WindowsNotifier() override { remove(); }

    WindowsNotifier(const WindowsNotifier&) = delete;
    WindowsNotifier& operator=(const WindowsNotifier&) = delete;

    /// The system's own warning sound, played asynchronously, as the sound
    /// scheme has it -- muted with everything else when Windows is.
    void beep() override { MessageBeep(MB_ICONWARNING); }

    void toast(const std::string& title, const std::string& text) override {
        if (hwnd_ == nullptr) return;
        NOTIFYICONDATAW nid = base();
        nid.uFlags = NIF_INFO;
        copy(nid.szInfoTitle, title);
        copy(nid.szInfo, text);
        // The sound is the beep's, which keeps to its own limit.
        nid.dwInfoFlags = NIIF_WARNING | NIIF_NOSOUND;
        if (!added_ || !Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            // Not there yet -- or gone with an Explorer that restarted.
            added_ = add();
            if (added_) Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        // The taskbar button flashes until GoBot is next looked at. It never
        // comes to the front by itself.
        FLASHWINFO flash{};
        flash.cbSize = sizeof flash;
        flash.hwnd = hwnd_;
        flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        FlashWindowEx(&flash);
    }

    void remove() {
        if (!added_) return;
        NOTIFYICONDATAW nid = base();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }

private:
    NOTIFYICONDATAW base() const {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof nid;
        nid.hWnd = hwnd_;
        nid.uID = kIconId;
        return nid;
    }

    bool add() {
        NOTIFYICONDATAW nid = base();
        nid.uFlags = NIF_ICON | NIF_TIP;
        nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));    // IDI_APPLICATION
        copy(nid.szTip, "GoBot");
        return Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }

    /// As UTF-16, cut to fit with its terminator, never between the two
    /// halves of a pair.
    template <std::size_t N>
    static void copy(wchar_t (&dst)[N], const std::string& utf8) {
        std::wstring w = to_wide(utf8);
        if (w.size() > N - 1) {
            w.resize(N - 1);
            if (!w.empty() && w.back() >= 0xD800 && w.back() <= 0xDBFF) w.pop_back();
        }
        std::copy(w.begin(), w.end(), dst);
        dst[w.size()] = L'\0';
    }

    static constexpr UINT kIconId = 0x60B0;
    HWND hwnd_;
    bool added_ = false;
};

/// Zero ImGui's own copies of what was in the box `id`: the text it edits, the
/// copy it keeps of a box just left, and what it last read off the clipboard,
/// which is where a pasted webhook came from. Only while the box is not being
/// typed in. What the clipboard itself holds is the player's.
void wipe_imgui_copies(ImGuiID id) {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr || id == 0 || ctx->ActiveId == id) return;
    const auto zero = [](ImVector<char>& v) {
        if (v.Data != nullptr && v.Capacity > 0) SecureZeroMemory(v.Data, static_cast<SIZE_T>(v.Capacity));
    };
    ImGuiInputTextState& edit = ctx->InputTextState;
    if (edit.ID == id) {
        zero(edit.TextA);
        zero(edit.TextToRevertTo);
        zero(edit.CallbackTextBackup);
        edit.TextLen = 0;
        edit.ID = 0;        // set up afresh the next time the box is clicked
    }
    ImGuiInputTextDeactivatedState& left = ctx->InputTextDeactivatedState;
    if (left.ID == id || left.ID == 0) {
        zero(left.TextA);
        left.TextA.resize(0);
        left.ID = 0;
    }
    zero(ctx->ClipboardHandlerData);
    ctx->ClipboardHandlerData.resize(0);
}

std::string clock_of(std::int64_t wall) {
    return alert_hhmm(AlertClock::system().local(wall));
}

} // namespace

/// Everything the GUI keeps for the alerts.
struct GuiApp::AlertsUi {
    explicit AlertsUi(void* hwnd) : notifier(hwnd), service(SecretStore::default_path()) {}

    // The notifier first, so that it outlives the service that calls it.
    WindowsNotifier  notifier;
    AlertService     service;
    /// The tab's copy of configs/alerts.ini.
    AlertSettings    settings;
    char             webhook[512] = {};
    ImGuiID          webhook_id = 0;
    /// The last frame the tab was drawn in: a webhook pasted and then left
    /// unsaved, for another tab, is wiped once the tab is not drawn.
    int              drawn_frame = -1;
    bool             changing = false;      ///< Đổi webhook: the box is up again
    std::string      refused;               ///< why the last paste was not taken
    /// Room for the longest label in the widest letters UTF-8 has: a
    /// Vietnamese "ữ" is three bytes, and a box cut short mid-letter saved a
    /// broken one.
    char             label[AlertSettings::kLongestLabel * 4 + 1] = {};
    int              merge_s = 60;
    int              per_minute = 5;
    int              away_min = 3;

    // What is noticed by watching, looked at four times a second.
    std::uint64_t    looked_ms = 0;
    AwayWatch        away{ 3 * 60 * 1000 };
    DeathByHp        dead{ 3000 };
    AlertWatch       trash{ kAlertStuckMs };
    AlertWatch       chat{ kAlertStuckMs };
    /// A chat line handed over in doubt. Half a second, so that the two flags
    /// it is read from are not caught in the middle of changing.
    AlertWatch       chat_left{ 500 };
    unsigned         note_seen = 0;
};

void GuiApp::AlertsUiFree::operator()(AlertsUi* ui) const { delete ui; }

// ------------------------------------------------------------------ the hook

void GuiApp::alert(const char* event, const std::string& text_vi) {
    // The only place an ALERT line is written. The log says what was raised
    // whatever the tab says should be sent where -- with alerts off too.
    GB_WARN("ALERT %s: %s", event, text_vi.c_str());
    if (alerts_ui_) alerts_ui_->service.raise(event, text_vi, now_ms());
}

void GuiApp::alert_party(bool joined, const std::string& who) {
    // Another player's name is theirs: it goes only when the player ticked it.
    const bool names = alerts_ui_ && alerts_ui_->service.settings().party_names;
    alert("party_change", joined ? alert_text::party_joined(who, names)
                                 : alert_text::party_invited(who, names));
}

void GuiApp::alerts_init() {
    alerts_ui_.reset(new AlertsUi(main_hwnd_));
    AlertsUi& ui = *alerts_ui_;
    // No file is alerts off, which is what the defaults say.
    ui.settings.load(exe_relative("configs/alerts.ini"));
    if (ui.settings.webhook_in_file)
        GB_WARN("Alerts: configs/alerts.ini has a webhook line, which is ignored -- the webhook is "
                "pasted on the Thông báo tab");
    std::snprintf(ui.label, sizeof ui.label, "%s", ui.settings.label.c_str());
    ui.merge_s = ui.settings.merge_s;
    ui.per_minute = ui.settings.max_per_minute;
    ui.away_min = ui.settings.not_in_world_min;
    ui.away.set_ms(static_cast<std::uint64_t>(ui.away_min) * 60'000);

    ui.service.set_notifier(&ui.notifier);
    ui.service.set_settings(ui.settings);
    // The saved webhook is decrypted on the service's thread: DPAPI has no
    // business on the one that draws the window. Seeing whether there is a
    // file is only a look at the disk.
    const bool saved = SecretStore(SecretStore::default_path()).exists();
    if (ui.settings.enabled || saved) ui.service.start();
    if (saved) ui.service.load_webhook();
}

void GuiApp::alerts_shutdown() {
    if (!alerts_ui_) return;
    // Ends a request under way and forgets the webhook's address.
    alerts_ui_->service.stop();
    alerts_ui_->service.set_notifier(nullptr);
    alerts_ui_->notifier.remove();
    // A webhook pasted and never saved goes with it, rather than lying in
    // freed memory. ImGui's own copies have gone with its context already.
    SecureZeroMemory(alerts_ui_->webhook, sizeof alerts_ui_->webhook);
    alerts_ui_.reset();
}

bool GuiApp::alerts_dot() const {
    if (!alerts_ui_ || !alerts_ui_->settings.enabled) return false;
    return alerts_ui_->service.status().discord != AlertStatus::Discord::Disabled;
}

// ---------------------------------------------------------------- watching

void GuiApp::alerts_tick() {
    if (!alerts_ui_) return;
    AlertsUi& ui = *alerts_ui_;
    const std::uint64_t now = now_ms();

    // Beeps and notifications, here on the window's own thread, which never
    // waits for the network: a request hanging on the service's thread does
    // not make them late.
    ui.service.step_local(now);

    // A webhook pasted and then left unsaved -- another tab opened -- is not
    // kept until GoBot closes: the box and ImGui's copies of it are wiped once
    // a frame has been drawn without the tab. Not while the box is still the
    // one being typed in, which it cannot be for long once it is not drawn.
    if (ui.webhook[0] != '\0') {
        const ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (ctx != nullptr && ctx->FrameCount > ui.drawn_frame && ctx->ActiveId != ui.webhook_id) {
            SecureZeroMemory(ui.webhook, sizeof ui.webhook);
            wipe_imgui_copies(ui.webhook_id);
        }
    }

    if (now - ui.looked_ms < 250) return;
    ui.looked_ms = now;

    // Discord refused something: said under the button too, not only on the
    // tab, which the player may not have open.
    {
        const AlertStatus st = ui.service.status();
        if (st.note_seq != ui.note_seen) {
            ui.note_seen = st.note_seq;
            if (!st.note.empty()) simple_note(st.note, 15000);
        }
    }

    const bool running = simple_running();
    bool crossing = false;
    {
        std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
        crossing = keeper_flow_ != nullptr;
    }

    // ---- not in the world ----
    // During a run, with the HP/MP reader on -- the same signal that holds the
    // macro -- the bars gone for minutes, having been there. Not while the
    // keeper is crossing back in: it says what it is doing itself.
    const float hp_pct = vitals_running_ ? vitals_.hp_percent() : -1.0f;
    switch (ui.away.step(running, vitals_running_, crossing, hp_pct, now)) {
        case AwayWatch::Say::Away:
            alert("not_in_world", alert_text::not_in_world(ui.settings.not_in_world_min));
            break;
        case AwayWatch::Say::Back:
            alert("not_in_world", alert_text::in_world_again());
            break;
        case AwayWatch::Say::Nothing:
            break;
    }

    // ---- a death nothing else will tell of ----
    // With Tự hồi sinh on, its worker sees the death dialog and says so.
    // Without it, the HP figure read as 0 of something for 3 s.
    const bool by_hp = running && vitals_running_ && !revive_running_;
    const VitalsReader::Vitals v = by_hp ? vitals_.values() : VitalsReader::Vitals{};
    if (ui.dead.step(by_hp, v.hp, v.hp_max, now)) alert("death", alert_text::death(false));

    // ---- the bin's dialog, held ----
    // A trash pass could not give an item back and has held everything off
    // since: two minutes is longer than any pass, and the player has not
    // answered the dialog.
    if (ui.trash.due(trash_hold_.load(), now))
        alert("trash_stuck", alert_text::trash_stuck(false));

    // ---- the chat line ----
    // Watched by a Tự chat worker, with everything held off, for two minutes;
    // or handed over by a stop with nobody watching it, which has stopped
    // everything (tick() does that) -- said at once.
    if (ui.chat.due(chat_doubt_ && chat_doubt_watched_, now))
        alert("chat_stuck", alert_text::chat_stuck(false));
    if (ui.chat_left.due(chat_doubt_ && !chat_doubt_watched_, now))
        alert("chat_stuck", alert_text::chat_stuck(true));
}

// --------------------------------------------------------------------- tab

void GuiApp::simple_alerts_tab() {
    if (!alerts_ui_) return;
    AlertsUi& ui = *alerts_ui_;
    ui.drawn_frame = ImGui::GetFrameCount();
    const std::string path = exe_relative("configs/alerts.ini");
    const AlertStatus st = ui.service.status();
    const std::uint64_t now = now_ms();

    // Every change written at once, key by key -- the file's comments stay --
    // and handed to the service, which applies it at once too.
    const auto put = [&](const char* section, const std::string& key, const std::string& value) {
        if (!AlertSettings::save_key(path, section, key, value))
            simple_note("Không lưu được configs/alerts.ini.", 8000);
        ui.service.set_settings(ui.settings);
    };
    const auto put_bool = [&](const char* section, const std::string& key, bool value) {
        put(section, key, value ? "true" : "false");
    };

    // ---- on or off ----
    if (!begin_card("##alerts")) { end_card(); return; }
    heading(strong_font_, "Thông báo");
    if (ImGui::Checkbox("Bật thông báo", &ui.settings.enabled)) {
        put_bool("Alerts", "enabled", ui.settings.enabled);
        if (ui.settings.enabled) ui.service.start();
    }
    hint("Khi có chuyện cần người — nhân vật chết, mất kết nối, bot tự dừng — GoBot gửi một tin vào "
         "kênh Discord của bạn và kêu trên máy này. GoBot không gửi tên tài khoản, mật khẩu, tên "
         "nhân vật, vị trí hay tên người chơi khác.");
    if (ui.settings.webhook_in_file)
        coloured(kWait, "Webhook phải dán ở thẻ Thông báo, không ghi trong file.");
    end_card();

    // ---- Discord ----
    if (!begin_card("##discord")) { end_card(); return; }
    heading(strong_font_, "Discord");
    if (st.busy) coloured(kDim, "Đang xử lý webhook…");
    if (st.webhook && !ui.changing) {
        coloured(kGood, ("Đã lưu webhook (" + st.masked + "). Đường dẫn được mã hoá bằng Windows, "
                         "chỉ tài khoản Windows này trên máy này đọc được.").c_str());
        if (ImGui::Button("Đổi webhook")) {
            ui.changing = true;
            ui.refused.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Xoá webhook")) ImGui::OpenPopup("Xoá webhook?");
        if (ImGui::BeginPopupModal("Xoá webhook?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted("Xoá webhook đã lưu? GoBot thôi gửi vào Discord cho đến khi bạn dán "
                                   "webhook mới.");
            if (ImGui::Button("Xoá")) {
                ui.service.start();
                ui.service.clear_webhook();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Thôi")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    } else {
        if (st.unreadable && !ui.changing)
            coloured(kBad, "Không đọc được webhook đã lưu — hãy dán lại.");
        label("Đường dẫn webhook Discord");
        // Dots, not the address: whoever has the address can post in the
        // channel, and the screen may be shared or photographed.
        const float buttons = ImGui::CalcTextSize("Lưu").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                              (ui.changing ? ImGui::CalcTextSize("Thôi").x +
                                                 ImGui::GetStyle().FramePadding.x * 2.0f +
                                                 ImGui::GetStyle().ItemSpacing.x
                                           : 0.0f) +
                              ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - buttons));
        ImGui::InputText("##webhook", ui.webhook, sizeof ui.webhook, ImGuiInputTextFlags_Password);
        ui.webhook_id = ImGui::GetItemID();
        const auto wipe_box = [&] {
            SecureZeroMemory(ui.webhook, sizeof ui.webhook);
            wipe_imgui_copies(ui.webhook_id);
        };
        ImGui::SameLine();
        ImGui::BeginDisabled(ui.webhook[0] == '\0' || st.busy);
        if (ImGui::Button("Lưu")) {
            std::string pasted(ui.webhook);
            WebhookUrl check;
            if (WebhookUrl::parse(pasted, check)) {
                // Encrypted and written on the service's thread, which wipes
                // this text once it has done so.
                ui.service.start();
                ui.service.save_webhook(std::move(pasted));
                ui.changing = false;
                ui.refused.clear();
            } else {
                ui.refused = kWebhookRefused;
            }
            check.wipe();
            wipe_string(pasted);
            wipe_box();
        }
        ImGui::EndDisabled();
        if (ui.changing) {
            ImGui::SameLine();
            if (ImGui::Button("Thôi##webhook")) {
                ui.changing = false;
                ui.refused.clear();
                wipe_box();
            }
        }
        if (!ui.refused.empty()) coloured(kBad, ui.refused.c_str());
        hint("Tạo webhook: trong Discord mở Cài đặt máy chủ → Tích hợp → Webhook → Webhook mới → Sao "
             "chép URL webhook, rồi dán vào đây. Cài Discord trên điện thoại và bật thông báo cho "
             "kênh đó.");
    }

    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::Checkbox("Gửi vào Discord", &ui.settings.discord))
        put_bool("Alerts", "discord", ui.settings.discord);
    ImGui::SameLine(0, 24);
    const bool cooling = st.test_ready_ms != 0 && now < st.test_ready_ms;
    ImGui::BeginDisabled(cooling || st.busy);
    if (ImGui::Button("Gửi thử")) {
        ui.service.start();
        if (ui.service.send_test(now) && !st.webhook)
            simple_note("Chưa có webhook: chỉ thử tiếng kêu và thông báo trên máy này.", 8000);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Gửi một tin thử vào Discord và hiện thông báo trên máy này. Hai lần gửi thử "
                          "cách nhau ít nhất 30 giây.");

    // One line: what became of the last message.
    if (!ui.settings.discord) {
        coloured(kDim, "Không gửi vào Discord: chỉ kêu và hiện thông báo trên máy này.");
    } else {
        switch (st.discord) {
            case AlertStatus::Discord::None:
                coloured(kDim, "Chưa có webhook: thông báo chỉ hiện trên máy này.");
                break;
            case AlertStatus::Discord::Disabled:
                coloured(kBad, ("Webhook không còn dùng được (Discord trả lời " + std::to_string(st.code) +
                                ") — hãy tạo webhook mới và dán lại.").c_str());
                break;
            case AlertStatus::Discord::Retrying:
                coloured(kWait, "Chưa gửi được: không có mạng hoặc Discord đang lỗi — GoBot sẽ thử lại.");
                break;
            case AlertStatus::Discord::Waiting:
                coloured(kWait, ("Discord bảo gửi chậm lại — gửi tiếp lúc " + clock_of(st.resume_wall) +
                                 ".").c_str());
                break;
            case AlertStatus::Discord::Sent:
                coloured(kGood, ("Đã gửi lúc " + clock_of(st.sent_wall) + ".").c_str());
                break;
            case AlertStatus::Discord::Ready:
                coloured(kDim, "Chưa gửi tin nào.");
                break;
        }
        if (st.capped)
            coloured(kWait, "Đang giới hạn số tin: các tin được gộp lại và gửi dần.");
        if (st.queued > 0)
            hint(("Đang chờ gửi: " + std::to_string(st.queued) + " tin.").c_str());
        if (st.dropped > 0)
            hint(("Đã bỏ " + std::to_string(st.dropped) + " thông báo vì hàng chờ đầy.").c_str());
    }
    end_card();

    // ---- which events ----
    if (!begin_card("##alertwhen")) { end_card(); return; }
    heading(strong_font_, "Báo khi");
    for (const AlertEventInfo& e : alert_events()) {
        if (e.label_vi == nullptr) continue;
        ImGui::PushID(e.key);
        bool on = ui.settings.event_on(e.key);
        if (ImGui::Checkbox(e.label_vi, &on)) {
            ui.settings.events[e.key] = on;
            put_bool("Events", e.key, on);
        }
        if (std::strcmp(e.key, "not_in_world") == 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::InputInt("##away", &ui.away_min, 0, 0);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ui.away_min = std::clamp(ui.away_min, 1, AlertSettings::kMaxAway);
                ui.settings.not_in_world_min = ui.away_min;
                ui.away.set_ms(static_cast<std::uint64_t>(ui.away_min) * 60'000);
                put("Alerts", "not_in_world_min", std::to_string(ui.away_min));
            }
            ImGui::SameLine();
            label("phút");
        }
        if (std::strcmp(e.key, "party_change") == 0) {
            ImGui::Indent(mark_indent());
            ImGui::BeginDisabled(!on);
            if (ImGui::Checkbox("Kèm tên người chơi khác", &ui.settings.party_names))
                put_bool("Events", "party_names", ui.settings.party_names);
            ImGui::EndDisabled();
            ImGui::Unindent(mark_indent());
        }
        ImGui::PopID();
    }
    end_card();

    // ---- on this machine ----
    if (!begin_card("##alertlocal")) { end_card(); return; }
    heading(strong_font_, "Trên máy này");
    if (ImGui::Checkbox("Phát tiếng kêu", &ui.settings.sound))
        put_bool("Alerts", "sound", ui.settings.sound);
    if (ImGui::Checkbox("Hiện thông báo của Windows", &ui.settings.toast))
        put_bool("Alerts", "toast", ui.settings.toast);
    hint("Không cướp chuột hay bàn phím khỏi game. Tiếng kêu nhiều nhất 10 giây một lần.");
    end_card();

    // ---- limits ----
    if (!begin_card("##alertlimits")) { end_card(); return; }
    heading(strong_font_, "Giới hạn");
    label("Gộp tin giống nhau trong");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputInt("##merge", &ui.merge_s, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ui.merge_s = std::clamp(ui.merge_s, AlertSettings::kMinMerge, AlertSettings::kMaxMerge);
        ui.settings.merge_s = ui.merge_s;
        put("Alerts", "merge_s", std::to_string(ui.merge_s));
    }
    ImGui::SameLine();
    label("giây");

    label("Tối đa");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputInt("##perminute", &ui.per_minute, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ui.per_minute = std::clamp(ui.per_minute, 1, AlertSettings::kMaxPerMinute);
        ui.settings.max_per_minute = ui.per_minute;
        put("Alerts", "max_per_minute", std::to_string(ui.per_minute));
    }
    ImGui::SameLine();
    label("tin mỗi phút");

    label("Tên máy (không bắt buộc)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##label", ui.label, sizeof ui.label);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string clean = AlertSettings::clean_label(ui.label);
        SecureZeroMemory(ui.label, sizeof ui.label);
        std::snprintf(ui.label, sizeof ui.label, "%s", clean.c_str());
        ui.settings.label = clean;
        put("Alerts", "label", clean);
        // A webhook pasted here by mistake is dropped by clean_label; ImGui's
        // copies of what was typed go too.
        wipe_imgui_copies(ImGui::GetItemID());
    }
    hint("Hiện ở đầu mỗi tin, ví dụ Máy 1. GoBot không tự gửi tên nhân vật.");
    end_card();

    // ---- what happened ----
    if (!st.lines.empty()) {
        if (!begin_card("##alertlog")) { end_card(); return; }
        last_lines(st.lines, 4);
        end_card();
    }
}

} // namespace gobot
