// The party HUD as the party worker and the Tổ đội tab use it: invites only
// when they can work, the tick "Chỉ mời khi đang làm chủ tổ đội", and the
// state card's "Tổ đội: …" with its look-only "Thử xem".
//
// What the HUD says, and whether an invite can work by it, is the library's
// (gobot/party_hud.hpp). Here is what only the window knows: the party
// worker's frame and pictures, the holds, and the tab.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/auto_login.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"
#include "gobot/party_hud.hpp"
#include "gobot/screen_capture.hpp"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <mutex>
#include <string>

#include <windows.h>
#include <dwmapi.h>

namespace gobot {
namespace {

using namespace simple_ui;

/// How often the party worker reads the HUD for the Tổ đội tab, at most. The
/// read is a search of the left of the screen, a tenth of a second or so.
constexpr std::uint64_t kHudEveryMs = 10'000;

/// GoBot's own window in front, and over the part of the game the party is
/// read from (PartyHudReader::kArea): what keeps a frame read off the screen
/// from being trusted for the party while the player looks at this tab.
bool gobot_over_party(void* game, void* self) {
    const HWND g = static_cast<HWND>(game), s = static_cast<HWND>(self);
    if (g == nullptr || s == nullptr || !IsWindowVisible(s) || IsIconic(s) || GetForegroundWindow() != s)
        return false;
    RECT client{};
    if (!GetClientRect(g, &client)) return false;
    const ScreenRect& a = PartyHudReader::kArea;
    POINT tl{ a.x, a.y };
    POINT br{ std::min<LONG>(client.right, a.right()), std::min<LONG>(client.bottom, a.bottom()) };
    if (br.x <= tl.x || br.y <= tl.y || !ClientToScreen(g, &tl) || !ClientToScreen(g, &br)) return false;
    // Where it is drawn, not its rectangle: its invisible resizing border
    // reaches past what is seen of it, and beside the game is not over it.
    RECT mine{};
    if (FAILED(DwmGetWindowAttribute(s, DWMWA_EXTENDED_FRAME_BOUNDS, &mine, sizeof mine)) &&
        !GetWindowRect(s, &mine))
        return false;
    const RECT part{ tl.x, tl.y, br.x, br.y };
    RECT hit{};
    return IntersectRect(&hit, &part, &mine) != FALSE;
}

/// This program's own file name, as AutoLogin::part_seen names the program
/// of a window over the game.
std::string own_program() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const std::wstring full(path, n);
    const std::wstring file = full.substr(full.find_last_of(L"\\/") + 1);
    const int size = WideCharToMultiByte(CP_UTF8, 0, file.c_str(), static_cast<int>(file.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(0, size)), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, file.c_str(), static_cast<int>(file.size()), out.data(), size, nullptr,
                            nullptr);
    return out;
}

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
    // What GoBot itself has done to the party: the worker's own answers and
    // invites. Shared with Tự chat's thread and "Thử xem", and safe to be.
    PartyEvents    events;
};

GuiApp::PartyHudUi* GuiApp::make_party_hud_ui() { return new PartyHudUi; }
void GuiApp::PartyHudUiFree::operator()(PartyHudUi* ui) const { delete ui; }

PartyEvents& GuiApp::party_events() { return party_hud_ui_->events; }

PartyMembership GuiApp::party_from_game() const {
    // P0-3: the party as the game's packets give it -- joined, left, who
    // leads -- goes here, and a change in it to party_events() as it comes.
    // Until then nothing reads it, and the screen decides (read_party).
    return PartyMembership{};
}

namespace {

/// The reader, from the worker's store, once; false when the pictures are
/// not all there, which is said once.
bool reader_ready(PartyHudReader& reader, bool& loaded, const TemplateStore& store) {
    if (!loaded) {
        loaded = true;
        std::string missing;
        if (!reader.load(store, &missing))
            GB_WARN("Party: the party HUD cannot be read -- %s is missing; inviting as before, but nobody while "
                    "\"only while leading\" is ticked, as the player's own crown cannot be seen",
                    missing.c_str());
    }
    return reader.ready();
}

} // namespace

bool GuiApp::party_may_invite(const std::string& who, const PartyLook& look, const TemplateStore& store,
                              bool always_leader) {
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
    // Without its pictures the reader reads nothing at all, which the gate
    // takes as a HUD not read. The look as it was grabbed -- trusted, then --
    // and held up against what GoBot has done to the party since: a look
    // from before GoBot's last answer or invite is no reading for an invite
    // at all (read_party).
    reader_ready(ui.reader, ui.loaded, store);
    const PartyHud hud = read_party(ui.reader, look, ui.events, party_from_game());
    if (ui.reader.ready()) {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        ui.line_vi = party_hud_line_vi(hud);
    }
    ui.read_ms = now_ms();
    const PartyGate gate = party_invite_gate(hud, always_leader);
    if (gate.allowed) {
        // A HUD not read lets an invite through only with the tick off, as
        // before the HUD was read; the log says so.
        if (hud.state == PartyState::Unknown)
            GB_INFO("Party: %s asked; the party HUD could not be read (%s) -- inviting as before", who.c_str(),
                    hud.why.c_str());
        // With the tick, an invite from alone starts a party: said, so that
        // the log shows what it went by.
        if (always_leader && hud.state == PartyState::Alone)
            GB_INFO("Party: %s asked; the game shows no party, so alone -- inviting, to start a party -- %s",
                    who.c_str(), party_hud_text(hud).c_str());
        return true;
    }
    // Why, in English for the log and in Vietnamese on the Tổ đội tab.
    GB_INFO("Party: %s asked; not invited: %s -- %s", who.c_str(), gate.reason_en.c_str(),
            party_hud_text(hud).c_str());
    say_party("Không mời " + who + ". " + gate.reason_vi + ".");
    return false;
}

void GuiApp::party_hud_tick(const PartyLook& look, const TemplateStore& store) {
    PartyHudUi& ui = *party_hud_ui_;
    const std::uint64_t now = now_ms();
    if (ui.read_ms != 0 && now - ui.read_ms < kHudEveryMs) return;
    ui.read_ms = now;
    if (!reader_ready(ui.reader, ui.loaded, store)) return;
    const PartyHud hud = read_party(ui.reader, look, ui.events, party_from_game());
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
    // Alone is what the game shows (the player's rule of 2026-10-21): no
    // member's panel, no crown (read_party, party_invite_gate).
    ImGui::SetItemTooltip("Chỉ tự động mời khi thấy vương miện chủ tổ đội ở ô của bạn (góc trái trên màn hình "
                          "game), hoặc khi chưa có tổ đội: khung tổ đội của game không có ô thành viên nào, không "
                          "có vương miện. Lời mời lúc chưa có tổ đội lập tổ đội mới do bạn làm chủ, như GoC. Một "
                          "cửa sổ của game che các ô tổ đội thì GoBot cũng thấy như chưa có tổ đội: khi cần, đóng "
                          "cửa sổ đó. Đang là thành viên, hoặc không đọc được khung tổ đội, thì không mời; thẻ "
                          "này ghi lý do.");
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
            // One look, trusted as it was at the grab -- what lies over the
            // game then, to say so -- and held up against what GoBot has done
            // to the party since: as the party worker takes and reads its own
            // (take_party_look, read_party).
            ScreenCapture capture;
            PartyLook look;
            void* const game = input_.target_window();
            if (!take_party_look(party_sight(capture, game), look))
                return std::string("Không chụp được màn hình game.");
            const PartyHud hud = read_party(reader, look, party_events(), party_from_game());
            const std::string& who = look.in_the_way;
            GB_INFO("Party: Thử xem -- the party HUD reads %s%s", party_hud_text(hud).c_str(),
                    who.empty() ? "" : (" -- in the way: " + who).c_str());
            std::string said = party_hud_line_vi(hud) + ".";
            if (hud.state == PartyState::Unknown) {
                if (!look.trusted && !who.empty() && who == own_program())
                    said += " Cửa sổ GoBot đang che góc trái của game: dời GoBot sang bên rồi thử lại.";
                else if (!look.trusted && who == "off the screen")
                    said += " Góc trái của game nằm ngoài màn hình.";
                else if (!look.trusted && !who.empty())
                    said += " Cửa sổ " + who + " đang che góc trái của game.";
                else
                    said += " Cần thấy khung nhân vật ở góc trái trên màn hình game, không bị cửa sổ nào che.";
            }
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
    // GoBot's own window over the game's left side, as it is whenever it is
    // in front there: nothing read off the screen is trusted for the party
    // then, and the card says why, as the other tabs do.
    if (input_.window_valid() && gobot_over_party(input_.target_window(), main_hwnd_)) {
        coloured(kBad, "Cửa sổ GoBot đang che góc trái của game, nên không đọc được khung tổ đội.");
        if (ImGui::Button("Dời GoBot sang bên##party")) move_clear_of_game();
    }
}

} // namespace gobot
