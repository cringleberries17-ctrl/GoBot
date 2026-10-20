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
#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"
#include "gobot/party_hud.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/screen_capture.hpp"

#include "imgui.h"

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace gobot {
namespace {

using namespace simple_ui;

/// A line passed over is said so once a minute at most, each line on its own.
constexpr std::uint64_t kSaidEveryMs = 60'000;
/// A ticked line whose turn finds the game behind another window waits this
/// long before it is looked at again: what every line does then (chat_gate).
constexpr std::uint64_t kHiddenWaitMs = 5000;
/// Between a ticked line's two looks at the party HUD, the game is looked at
/// this often for an invitation: one up for longer than this is seen.
constexpr std::uint64_t kWatchEveryMs = 250;

const char* const kTickTip = "Không gửi câu này khi bạn đang trong tổ đội (GoBot nhìn khung tổ đội trên màn hình "
                             "game). Khi không đọc được khung tổ đội, hoặc chưa chắc là chưa có tổ đội (có cửa sổ, "
                             "hộp thoại hay khung chú thích trên màn hình game, hoặc vừa có lời mời vào tổ đội), "
                             "thì cũng chưa gửi. Trước khi gửi, GoBot nhìn khung tổ đội hai lần, cách nhau 3 giây.";

} // namespace

/// What the window keeps for the tick.
struct GuiApp::ChatAloneUi {
    // Tự chat's thread's own reader, made from its pictures on first need,
    // and its own capture, to know whether a frame came off the screen; and
    // the invitation's buttons, found as the party worker finds them.
    PartyHudReader reader;
    bool           loaded = false;
    ScreenCapture  capture;
    PartyInvite    invite;
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
        // The invitation's buttons where the party worker's settings put them.
        IniFile party;
        if (party.load(exe_relative("configs/party.ini"))) {
            PartyInvite::Settings geometry;
            geometry.load(party);
            ui.invite.set_settings(geometry);
        }
    }
    void* const game = input_.target_window();

    // Whether an invitation is up on a frame, as the party worker finds one
    // -- what every look notes for every decision on "alone" after it
    // (PartyEvents::invitation): Tự chat is GoBot's only eye on the game
    // whenever the party worker looks for none, and one answered by hand
    // leaves a party the game may take a while to draw. The trash bin's own
    // dialogs wear the same buttons and are none; any other dialog with them
    // is taken for one, the safe way round.
    const std::function<bool(const Frame&)> invitation_up = [&](const Frame& f) {
        return ui.invite.look(f, store, nullptr).up && !bag::bin_asking(f, store);
    };
    const auto note_invitation = [&](const Frame& f, std::uint64_t at) {
        party_events().invitation(at, invitation_up(f));
    };
    // A look of its own, taken now: the frame the line would go on is this
    // one, not one from the last interval -- trusted and whole as they were
    // at the grab, as the party worker takes its look -- and its reading.
    // Nothing is held or pressed for it.
    const auto look_once = [&](PartyHud& hud) -> AloneLook {
        hud = PartyHud{};
        PartyLook look;
        if (!ui.reader.ready()) {
            hud.why = "the party pictures are missing";
            return AloneLook::Unread;
        }
        if (!input_.window_valid() || !take_party_look(party_sight(ui.capture, game), look)) {
            hud.why = "no picture of the game";
            return AloneLook::Unread;
        }
        const AloneLook how = alone_look(look.trusted, input_.window_focused());
        if (how == AloneLook::Read) {
            // Alone counts only surely, as the tick's invite goes by it: all
            // of the game seen, no window, dialog or tooltip on it, and no
            // party change, nor a party perhaps joined, a moment ago
            // (read_party).
            note_invitation(look.frame, look.grabbed_ms);
            hud = read_party(ui.reader, look, party_events(), party_from_game());
        } else if (how == AloneLook::Unread) {
            // In front, and its left side under another window or past the
            // screen's edge: no reading, as the HUD not understood -- the
            // line is passed over and the other lines go on (PA-R3).
            hud.why = "the party's panels are not in view (another window over the game's left side, or the "
                      "game past the screen's edge)";
        }
        return how;
    };

    PartyHud hud;
    AloneLook seen = look_once(hud);
    if (seen == AloneLook::Read && alone_for_chat(hud)) {
        // Surely alone on this look -- and then on one more, kSettleMs on,
        // with the game looked at for an invitation every kWatchEveryMs in
        // between (watch_invitations): one answered by hand a moment before
        // the first look, or just after it, leaves a party the game has not
        // drawn yet. Seen up, or gone, meanwhile, the second look is not
        // surely alone (read_party); a party drawn by then is a party.
        const bool watched = watch_invitations(
            party_sight(ui.capture, game), invitation_up,
            [](std::uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); },
            [this] { return chat_stop_.load() || chat_quit_.load() || !input_.window_valid(); }, party_events(),
            now_ms() + PartyEvents::kSettleMs, static_cast<std::uint32_t>(kWatchEveryMs));
        if (chat_stop_ || chat_quit_) return true;          // nothing goes now anyway
        if (watched) {
            seen = look_once(hud);
            if (seen == AloneLook::Read && alone_for_chat(hud))
                GB_INFO("Chat: line %d -- surely alone on two looks %llu s apart, with no invitation seen between",
                        index + 1, static_cast<unsigned long long>(PartyEvents::kSettleMs / 1000));
        } else {
            hud = PartyHud{};
            hud.why = "the game could not be looked at between the two looks at the party HUD";
            seen = AloneLook::Unread;
        }
    }
    if (seen == AloneLook::KeepTurn) {
        // The game behind another window: no line goes until it is in front
        // again (chat_gate), and this one keeps its turn.
        schedule.postpone(now, kHiddenWaitMs);
        const auto last = ui.said.find(index);
        if (last == ui.said.end() || now - last->second >= kSaidEveryMs) {
            ui.said[index] = now;
            GB_INFO("Chat: line %d waits -- the game is not in front, so its party HUD is not read", index + 1);
            say_chat("Câu số " + std::to_string(index + 1) + " chỉ gửi khi chưa có tổ đội — chờ cửa sổ "
                     "game lên trên cùng để xem khung tổ đội.");
        }
        return true;
    }

    std::vector<bool> typeable;
    for (const std::string& line : s.lines)
        typeable.push_back(!prepare_chat(line, s.method, s.strip_diacritics, s.max_length).empty());
    // Unsure is not alone: the line waits -- passed over for its next turn,
    // or put off a minute when no other line could go -- and says why.
    const AloneTurn turn = alone_turn(s.only_alone, typeable, index, alone_for_chat(hud));
    if (turn == AloneTurn::Send) return false;

    const bool unread = hud.state == PartyState::Unknown;
    const bool unsure = hud.state == PartyState::Alone;
    const auto last = ui.said.find(index);
    if (last == ui.said.end() || now - last->second >= kSaidEveryMs) {
        ui.said[index] = now;
        const std::string why_en = unread   ? "the party HUD could not be read: " + hud.why
                                   : unsure ? "read as alone, but not surely: " + hud.alone_doubt
                                            : std::string("in a party");
        const std::string why_vi = unread   ? std::string(party_text::kUnread)
                                   : unsure ? "chưa chắc là chưa có tổ đội (" + hud.alone_doubt_vi + ")"
                                            : std::string("đang trong tổ đội");
        GB_INFO("Chat: line %d skipped (%s)%s", index + 1, why_en.c_str(),
                turn == AloneTurn::Wait ? "; every line is ticked so, so it waits a minute" : "");
        say_chat("Câu số " + std::to_string(index + 1) + " chỉ gửi khi chưa có tổ đội — " + why_vi +
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
