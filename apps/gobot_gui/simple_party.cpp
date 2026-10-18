// The party HUD as the party worker and the Tổ đội tab use it: invites only
// when they can work, the tick "Chỉ mời khi đang làm chủ tổ đội", and the
// state card's "Tổ đội: …" with its look-only "Thử xem".
//
// What the HUD says, and whether an invite can work by it, is the library's
// (gobot/party_hud.hpp). Here is what only the window knows: the party
// worker's frame and pictures, the holds, and the tab.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"
#include "gobot/party_hud.hpp"
#include "gobot/screen_capture.hpp"

#include "imgui.h"

#include <chrono>
#include <future>
#include <mutex>
#include <string>

namespace gobot {
namespace {

using namespace simple_ui;

/// How often the party worker reads the HUD for the Tổ đội tab, at most. The
/// read is a search of the left of the screen, a tenth of a second or so.
constexpr std::uint64_t kHudEveryMs = 10'000;

} // namespace

/// What the window keeps of the party HUD.
struct GuiApp::PartyHudUi {
    // The party worker's own, made from its pictures on its first look.
    PartyHudReader reader;
    bool           loaded = false;
    std::uint64_t  read_ms = 0;
    // What the state card shows; under worker_mutex_.
    std::string    line_vi;
    // "Thử xem": one capture and one read, off the drawing thread.
    std::future<std::string> probe;
    std::string    probe_said;
};

GuiApp::PartyHudUi* GuiApp::make_party_hud_ui() { return new PartyHudUi; }
void GuiApp::PartyHudUiFree::operator()(PartyHudUi* ui) const { delete ui; }

namespace {

/// The reader, from the worker's store, once; false when the pictures are
/// not all there, which is said once.
bool reader_ready(PartyHudReader& reader, bool& loaded, const TemplateStore& store) {
    if (!loaded) {
        loaded = true;
        std::string missing;
        if (!reader.load(store, &missing))
            GB_WARN("Party: the party HUD cannot be read -- %s is missing; inviting as before", missing.c_str());
    }
    return reader.ready();
}

} // namespace

bool GuiApp::party_may_invite(const std::string& who, const Frame& frame, bool from_screen,
                              const TemplateStore& store, bool always_leader) {
    PartyHudUi& ui = *party_hud_ui_;
    // GoC's "Coordinator Denied (Another action running)": nothing while
    // something else of GoBot's holds the game -- a chat line being typed, a
    // quest round, a revive, a trash pass. The party worker takes no hold of
    // its own, so any hold is somebody else's.
    if (coord_.critical_active()) {
        const std::string held = coord_.critical_reason();
        GB_INFO("Party: %s asked while GoBot was busy (%s); not invited", who.c_str(), held.c_str());
        say_party(who + " asked while GoBot was busy (" + held + "); not invited");
        return false;
    }
    if (!reader_ready(ui.reader, ui.loaded, store)) return true;

    const PartyHud hud = ui.reader.read(frame, party_frame_trusted(from_screen, input_.target_window()));
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        ui.line_vi = party_hud_line_vi(hud);
    }
    ui.read_ms = now_ms();
    if (hud.state == PartyState::Unknown) {
        // Invited as before the HUD was read: only a party seen stops one.
        GB_INFO("Party: %s asked; the party HUD could not be read (%s) -- inviting as before", who.c_str(),
                hud.why.c_str());
        return true;
    }
    const PartyGate gate = party_invite_gate(hud, always_leader);
    if (gate.allowed) return true;
    GB_INFO("Party: %s asked; not invited: %s -- %s", who.c_str(), gate.reason_en.c_str(),
            party_hud_text(hud).c_str());
    say_party(who + " asked; not invited: " + gate.reason_en);
    return false;
}

void GuiApp::party_hud_tick(const Frame& frame, bool from_screen, const TemplateStore& store) {
    PartyHudUi& ui = *party_hud_ui_;
    const std::uint64_t now = now_ms();
    if (ui.read_ms != 0 && now - ui.read_ms < kHudEveryMs) return;
    ui.read_ms = now;
    if (!reader_ready(ui.reader, ui.loaded, store)) return;
    const PartyHud hud = ui.reader.read(frame, party_frame_trusted(from_screen, input_.target_window()));
    std::lock_guard<std::mutex> lock(worker_mutex_);
    const std::string line = party_hud_line_vi(hud);
    if (line != ui.line_vi) GB_INFO("Party: the party HUD reads %s", party_hud_text(hud).c_str());
    ui.line_vi = line;
}

void GuiApp::simple_party_leader_option() {
    // GoC's "Luôn làm chủ party" ("Chỉ tự động mời khi bản thân đang làm chủ
    // party"), party_always_leader in the same file as GoC's.
    if (ImGui::Checkbox("Chỉ mời khi đang làm chủ tổ đội", &simple_party_.always_leader)) {
        if (!IniFile::set_in_place(exe_relative("configs/party.ini"), "Party", "party_always_leader",
                                   simple_party_.always_leader ? "true" : "false"))
            simple_note("Không lưu được configs/party.ini.", 8000);
    }
    ImGui::SetItemTooltip("Chỉ tự động mời khi bạn đang làm chủ tổ đội. GoBot nhìn khung tổ đội trên màn "
                          "hình game; khi không đọc được thì vẫn mời như trước.");
    hint(trial_hint(kTrialPartyHud, "Tổ đội đã đủ 6 người thì không mời, dù có đánh dấu ô này hay không.")
             .c_str());
}

void GuiApp::simple_party_hud_line() {
    PartyHudUi& ui = *party_hud_ui_;
    if (party_running_) {
        std::string line;
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            line = ui.line_vi;
        }
        if (!line.empty()) coloured(kDim, line.c_str());
    }
    const bool busy = ui.probe.valid();
    ImGui::BeginDisabled(busy || !input_.window_valid());
    if (ImGui::Button("Thử xem##partyhud")) {
        ui.probe_said.clear();
        ui.probe = std::async(std::launch::async, [this] {
            TemplateStore pictures;
            std::string missing;
            if (!PartyHudReader::load_pictures(pictures, exe_relative("configs/templates.ini"), &missing))
                return "Thiếu hình khung tổ đội (" + missing + ").";
            PartyHudReader reader;
            if (!reader.load(pictures, &missing)) return "Thiếu hình khung tổ đội (" + missing + ").";
            ScreenCapture capture;
            Frame f;
            if (!capture.grab(input_.target_window(), f) || !f.valid())
                return std::string("Không chụp được màn hình game.");
            const PartyHud hud = reader.read(f, party_frame_trusted(capture.from_screen(), input_.target_window()));
            GB_INFO("Party: Thử xem -- the party HUD reads %s", party_hud_text(hud).c_str());
            std::string said = party_hud_line_vi(hud) + ".";
            if (hud.state == PartyState::Unknown)
                said += " Cần thấy khung nhân vật ở góc trái trên màn hình game, không bị cửa sổ nào che.";
            return said + " Không bấm gì.";
        });
    }
    ImGui::EndDisabled();
    if (ui.probe.valid() && ui.probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        ui.probe_said = ui.probe.get();
    ImGui::SetItemTooltip("Chụp màn hình game một lần và đọc khung tổ đội -- không bấm gì cả.");
    if (!ui.probe_said.empty()) {
        ImGui::SameLine();
        hint(ui.probe_said.c_str());
    }
}

} // namespace gobot
