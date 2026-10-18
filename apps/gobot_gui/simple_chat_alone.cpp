// Tự chat's "Chỉ khi chưa có tổ đội": the tick after each line on the Tự chat
// tab, and the look at the party HUD before a ticked line goes -- GoC's
// "Party Request: only send when NOT in a party", for the advert that asks
// people to join.
//
// Which line goes, and when, is the library's (gobot/auto_chat.hpp,
// alone_turn), and what the HUD says is gobot/party_hud.hpp's. Here is what
// only the window knows: Tự chat's frame, pictures and schedule, and the tab.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/auto_chat.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/party_hud.hpp"
#include "gobot/screen_capture.hpp"

#include "imgui.h"

#include <map>
#include <string>
#include <vector>

namespace gobot {
namespace {

using namespace simple_ui;

/// A line passed over is said so once a minute at most, each line on its own.
constexpr std::uint64_t kSaidEveryMs = 60'000;
/// A ticked line whose turn finds the game behind another window waits this
/// long before it is looked at again: what every line does then (chat_gate).
constexpr std::uint64_t kHiddenWaitMs = 5000;

const char* const kTickTip = "Không gửi câu này khi bạn đang trong tổ đội (GoBot nhìn khung tổ đội trên màn hình "
                             "game). Khi không đọc được khung tổ đội thì cũng không gửi.";

} // namespace

/// What the window keeps for the tick.
struct GuiApp::ChatAloneUi {
    // Tự chat's thread's own reader, made from its pictures on first need,
    // and its own capture, to know whether a frame came off the screen.
    PartyHudReader reader;
    bool           loaded = false;
    ScreenCapture  capture;
    std::map<int, std::uint64_t> said;      ///< line, and when it was last said passed over
    // The drawing thread's: whether the HUD's pictures are here, asked once.
    int            available = -1;
    std::string    missing;
};

GuiApp::ChatAloneUi* GuiApp::make_chat_alone_ui() { return new ChatAloneUi; }
void GuiApp::ChatAloneUiFree::operator()(ChatAloneUi* ui) const { delete ui; }

bool GuiApp::chat_alone_put_off(const ChatSettings& s, int index, const TemplateStore& store,
                                ChatSchedule& schedule, std::uint64_t now) {
    if (!s.alone_only(index)) return false;
    ChatAloneUi& ui = *chat_alone_ui_;
    if (!ui.loaded) {
        ui.loaded = true;
        std::string missing;
        if (!ui.reader.load(store, &missing))
            GB_WARN("Chat: the party HUD cannot be read -- %s is missing; lines ticked \"only when alone\" "
                    "are not said", missing.c_str());
    }

    // A look of its own, taken now: the frame the line would go on is this
    // one, not one from the last interval. Nothing is held or pressed for it.
    PartyHud hud;
    Frame f;
    void* const game = input_.target_window();
    if (!ui.reader.ready()) {
        hud.why = "the party pictures are missing";
    } else if (!input_.window_valid() || !ui.capture.grab(game, f) || !f.valid()) {
        hud.why = "no picture of the game";
    } else {
        switch (alone_look(party_frame_trusted(ui.capture.from_screen(), game), input_.window_focused())) {
            case AloneLook::Read:
                hud = ui.reader.read(f, true);
                break;
            case AloneLook::KeepTurn: {
                // The game behind another window: no line goes until it is in
                // front again (chat_gate), and this one keeps its turn.
                schedule.postpone(now, kHiddenWaitMs);
                const auto last = ui.said.find(index);
                if (last == ui.said.end() || now - last->second >= kSaidEveryMs) {
                    ui.said[index] = now;
                    GB_INFO("Chat: line %d waits -- the game is not in front, so its party HUD is not read",
                            index + 1);
                    say_chat("Câu số " + std::to_string(index + 1) + " chỉ gửi khi chưa có tổ đội — chờ cửa sổ "
                             "game lên trên cùng để xem khung tổ đội.");
                }
                return true;
            }
            case AloneLook::Unread:
                // In front, and its left side under another window or past
                // the screen's edge: no reading, as the HUD not understood --
                // the line is passed over and the other lines go on (PA-R3).
                hud.why = "the party's panels are not in view (another window over the game's left side, or the "
                          "game past the screen's edge)";
                break;
        }
    }

    std::vector<bool> typeable;
    for (const std::string& line : s.lines)
        typeable.push_back(!prepare_chat(line, s.method, s.strip_diacritics, s.max_length).empty());
    const AloneTurn turn = alone_turn(s.only_alone, typeable, index, hud.state == PartyState::Alone);
    if (turn == AloneTurn::Send) return false;

    const bool unread = hud.state == PartyState::Unknown;
    const auto last = ui.said.find(index);
    if (last == ui.said.end() || now - last->second >= kSaidEveryMs) {
        ui.said[index] = now;
        GB_INFO("Chat: line %d skipped (%s)%s", index + 1,
                unread ? ("the party HUD could not be read: " + hud.why).c_str() : "in a party",
                turn == AloneTurn::Wait ? "; every line is ticked so, so it waits a minute" : "");
        say_chat("Câu số " + std::to_string(index + 1) + " chỉ gửi khi chưa có tổ đội — " +
                 (unread ? std::string(party_text::kUnread) : std::string("đang trong tổ đội")) +
                 (turn == AloneTurn::Wait ? ", chờ 1 phút." : ", bỏ qua."));
    }
    if (turn == AloneTurn::Skip) schedule.skip(now);
    else                         schedule.postpone(now, kAloneWaitMs);
    return true;
}

void GuiApp::simple_chat_alone_tick(const std::vector<std::string>& lines, std::size_t i) {
    ChatAloneUi& ui = *chat_alone_ui_;
    if (ui.available < 0) {
        TemplateStore pictures;
        ui.available =
            PartyHudReader::load_pictures(pictures, exe_relative("configs/templates.ini"), &ui.missing) ? 1 : 0;
    }
    // The ticks as the box is now: each line's from the saved list, by its
    // words, so a line being edited shows what it will keep.
    const std::vector<bool> ticks = ChatSettings::carry_ticks(simple_chat_.lines, simple_chat_.only_alone, lines);
    bool on = i < ticks.size() && ticks[i];
    const char* const tick_label = "Chỉ khi chưa có tổ đội";
    ImGui::PushID(static_cast<int>(i));
    ImGui::PushFont(nullptr, kSmall);
    // Beside its line when there is room; under it, indented, when the line
    // wrapped or reaches the card's edge: after a wrapped row the tick would
    // land past the edge, where it can be neither seen nor clicked.
    const float tick_w = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                         ImGui::CalcTextSize(tick_label).x;
    ImGui::SameLine(0, 12);
    if (ImGui::GetContentRegionAvail().x < tick_w) {
        ImGui::NewLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24);
    }
    ImGui::BeginDisabled(ui.available == 0);
    const bool changed = ImGui::Checkbox(tick_label, &on);
    ImGui::EndDisabled();
    ImGui::PopFont();
    if (ui.available == 0)
        ImGui::SetItemTooltip("Chưa dùng được: GoBot thiếu hình khung tổ đội của game (%s).", ui.missing.c_str());
    else
        ImGui::SetItemTooltip("%s", trial_hint(kTrialChatAlone, kTickTip).c_str());
    ImGui::PopID();
    if (!changed || i >= ticks.size()) return;

    // Saved at once, lines and ticks together in one write, with the box as
    // it is now: the worker reads the file every second.
    std::vector<bool> fresh = ticks;
    fresh[i] = on;
    simple_chat_.lines = lines;
    simple_chat_.only_alone = fresh;
    if (!ChatSettings::save_lines(exe_relative("configs/chat.ini"), lines, fresh))
        simple_note("Không lưu được configs/chat.ini.", 8000);
}

void GuiApp::simple_chat_save_lines(const std::string& cfg) {
    const std::vector<std::string> lines = ChatSettings::split_lines(simple_chat_lines_);
    const std::vector<bool> ticks = ChatSettings::carry_ticks(simple_chat_.lines, simple_chat_.only_alone, lines);
    simple_chat_.lines = lines;
    simple_chat_.only_alone = ticks;
    if (!ChatSettings::save_lines(cfg, lines, ticks)) simple_note("Không lưu được configs/chat.ini.", 8000);
}

} // namespace gobot
