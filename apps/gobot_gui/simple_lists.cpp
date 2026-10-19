// Đánh quái's monster lists: the "Lọc quái" row of "Bot sẽ làm gì", and the
// "Danh sách quái" window it opens.
//
// Two lists of monster names -- "Không đánh", never attacked, and "Đánh
// trước", gone for first -- picked from the names the bot is reading off the
// screen ("Quái đang thấy") or typed in. GoC has its server sort monsters by
// kind ("Loại mob", "Ưu tiên đánh Boss", "Bỏ qua quái bay"); here the name on
// the plate is all there is. What the lists do in a fight is the hunter's
// (gobot/hunter.hpp); how names are matched is gobot/monster_list.hpp.
//
// Look only: nothing here presses a key, clicks or brings the game forward.
// "Nhìn quanh" starts the monster detector, which reads the screen and that
// is all. In a file of its own so that simple_view.cpp gets only the calls.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/common.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/monster_list.hpp"
#include "gobot/navigator.hpp"

#include "imgui.h"

#include <algorithm>
#include <string>
#include <vector>

namespace gobot {

/// The lists' window, and what it has seen.
struct GuiApp::ListsUi {
    bool             open_lists = false;     ///< asked for; opened on the next draw
    MonsterSightings sightings;              ///< "Quái đang thấy"
    std::uint64_t    seen_frames = 0;        ///< the detector's frame last taken in
    /// A look started from the window ("Nhìn quanh"): when it ends, or 0.
    std::uint64_t    look_until_ms = 0;
    bool             font_tried = false;     ///< the glyph set looked for, for the red line
    char             skip_typed[96] = "";
    char             first_typed[96] = "";
};

void GuiApp::ListsUiFree::operator()(ListsUi* ui) const { delete ui; }

namespace {

using namespace simple_ui;

/// How long "Nhìn quanh" looks for, at most.
constexpr std::uint64_t kLookMs = 60'000;

const char* const kListsTitle = "Danh sách quái";

/// "Chỉ đánh những con trong danh sách Đánh trước" with that list empty: the
/// hunter then attacks nothing at all.
const char* const kFirstOnlyEmptyVi =
    "Đang chọn chỉ đánh những con trong Đánh trước, nhưng danh sách Đánh trước trống: bot sẽ không "
    "đánh con nào.";

/// Take `name` off `list`: every entry it is a reading of, or that is a
/// reading of it. True if anything went.
bool take_off(std::vector<std::string>& list, const std::string& name) {
    const std::size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const std::string& n) {
                                  return monster_name_matches(name, n) ||
                                         monster_name_matches(n, name);
                              }),
               list.end());
    return list.size() != before;
}

/// Put `name` on `to`, taking it off `from`: a monster is on one list or the
/// other. Nothing more if `to` already has it, or is full. True if anything
/// changed.
bool put_on(std::vector<std::string>& to, std::vector<std::string>& from,
            const std::string& name) {
    bool changed = take_off(from, name);
    if (!monster_on_list(to, name) && to.size() < kMaxMonsterNames) {
        to.push_back(name);
        changed = true;
    }
    return changed;
}

/// A button drawn lit while `on`: which list a name is on.
bool toggle_button(const char* text, bool on) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, kWait);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kWait);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    }
    const bool clicked = ImGui::Button(text);
    if (on) ImGui::PopStyleColor(3);
    return clicked;
}

} // namespace

void GuiApp::simple_monster_row() {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    label("Lọc quái");
    ImGui::TableNextColumn();

    const std::size_t skip = hunt_config_.skip_names.size();
    const std::size_t first = hunt_config_.first_names.size();
    std::string text = skip == 0 && first == 0
        ? std::string("Chưa chọn")
        : "Không đánh: " + std::to_string(skip) + "  ·  Đánh trước: " + std::to_string(first);
    if (hunt_config_.first_only && first > 0) text += " (chỉ đánh những con này)";
    label(text.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Sửa danh sách…")) {
        if (!lists_ui_) lists_ui_.reset(new ListsUi);
        lists_ui_->open_lists = true;
    }
    ImGui::SetItemTooltip("Con quái nào không đánh, con nào đánh trước: chọn từ những con bot "
                          "đang thấy, hoặc gõ tên.");
    // "Only those" with none of "those": nothing at all is attacked. Said
    // here, by the switch, not only on the state card once the bot runs.
    if (hunt_config_.first_only && first == 0) coloured(kWait, kFirstOnlyEmptyVi);
    hint(trial_hint(kTrialLists, "Không đánh: bot không bao giờ đánh những con này. Đánh trước: "
                                 "bot đánh chúng trước những con khác.").c_str());
    ImGui::Dummy(ImVec2(0, 3));
}

void GuiApp::simple_lists_tick() {
    if (!lists_ui_) lists_ui_.reset(new ListsUi);
    ListsUi& ui = *lists_ui_;
    const std::uint64_t now = now_ms();

    // The names read lately, from every new frame -- here rather than where
    // the window is drawn, so that they are taken in while the game covers
    // this window, which is when a player looks at the monsters, and so that
    // the window opens on the last ten seconds of a hunt rather than empty.
    if (detector_running_) {
        const std::uint64_t frames = detector_.stats().frames;
        if (frames != ui.seen_frames) {
            ui.seen_frames = frames;
            ui.sightings.saw(detector_.plates(), now);
        }
    }

    // A look started from the window ends after a minute, whatever is drawn
    // meanwhile -- unless hunting has started and needs the detector itself,
    // or it has been stopped from elsewhere. Only the detector is stopped,
    // and only when nothing else is using it: stop_detector() stops the hunt
    // and the walk too.
    if (ui.look_until_ms != 0) {
        if (hunting_ || !detector_running_) {
            ui.look_until_ms = 0;
        } else if (now >= ui.look_until_ms) {
            ui.look_until_ms = 0;
            if (!(navigator_ && navigator_->running())) stop_detector();
        }
    }
}

void GuiApp::simple_monster_lists() {
    if (!lists_ui_) lists_ui_.reset(new ListsUi);
    ListsUi& ui = *lists_ui_;
    const std::uint64_t now = now_ms();

    if (ui.open_lists) {
        ui.open_lists = false;
        // Whether names can be read at all, for the red line. Loaded here as
        // start_detector() loads it, once: a glyph set that is there but not
        // yet needed is not missing. Never while the detector runs, which
        // reads with it.
        if (!font_.loaded() && !detector_running_ && !ui.font_tried) {
            ui.font_tried = true;
            const std::string dir = GlyphFont::default_dir();
            if (!dir.empty()) font_.load(dir);
        }
        ImGui::OpenPopup(kListsTitle);
    }

    ImGui::SetNextWindowSize(ImVec2(660, 640), ImGuiCond_Appearing);
    const bool open =
        ImGui::BeginPopupModal(kListsTitle, nullptr, ImGuiWindowFlags_NoSavedSettings);

    // The window closed with a look still going: the look goes with it.
    if (!open) {
        if (ui.look_until_ms != 0 && !hunting_ && detector_running_ &&
            !(navigator_ && navigator_->running()))
            stop_detector();
        ui.look_until_ms = 0;
        return;
    }

    bool changed = false;
    auto& skip = hunt_config_.skip_names;
    auto& first = hunt_config_.first_names;

    hint("Bot đọc tên quái trên màn hình, có thể sai một chữ hoặc mất dấu, nên tên được so gần "
         "đúng: giống 80% chữ cái là đủ. Tên 4 chữ trở xuống phải đúng hết.");
    if (!font_.loaded())
        coloured(kBad, "Không đọc được tên quái (thiếu bộ chữ bitmap_font) — danh sách sẽ không "
                       "có tác dụng.");

    // ---- what is in view ----
    ImGui::Dummy(ImVec2(0, 4));
    heading(strong_font_, "Quái đang thấy");
    if (ui.look_until_ms != 0) {
        const std::uint64_t left = ui.look_until_ms > now ? ui.look_until_ms - now : 0;
        label(("Đang nhìn quanh… còn " + std::to_string((left + 999) / 1000) + " giây.").c_str());
        ImGui::SameLine();
        if (ImGui::Button("Thôi")) {
            ui.look_until_ms = 0;
            if (!hunting_ && !(navigator_ && navigator_->running())) stop_detector();
        }
    } else if (detector_running_) {
        hint("Bot đang nhìn màn hình game.");
    } else {
        const bool have_game = input_.window_valid();
        ImGui::BeginDisabled(!have_game);
        if (ImGui::Button("Nhìn quanh (60 giây)")) {
            // Looking only: the detector reads the screen and that is all --
            // no key, no click, and the game is not brought to the front.
            start_detector();
            if (detector_running_) {
                ui.look_until_ms = now + kLookMs;
                ui.seen_frames = 0;
            }
        }
        ImGui::EndDisabled();
        if (!have_game) ImGui::SetItemTooltip("Chưa tìm thấy game.");
        hint("Đứng gần quái rồi bấm: bot chỉ nhìn màn hình game trong 60 giây, không bấm phím "
             "nào.");
    }

    const std::vector<std::string> seen = ui.sightings.names(now);
    if (seen.empty())
        hint(detector_running_ ? "Chưa đọc được tên con quái nào." : "Chưa thấy con quái nào.");
    for (std::size_t i = 0; i < seen.size(); ++i) {
        const std::string& name = seen[i];
        const bool on_skip = monster_on_list(skip, name);
        const bool on_first = !on_skip && monster_on_list(first, name);
        ImGui::PushID(static_cast<int>(i));
        label(name.c_str());
        ImGui::SameLine(260);
        if (toggle_button("Không đánh", on_skip))
            changed |= on_skip ? take_off(skip, name) : put_on(skip, first, name);
        ImGui::SetItemTooltip(on_skip ? "Đang trong Không đánh. Bấm lần nữa để bỏ ra."
                                      : "Không bao giờ đánh con này.");
        ImGui::SameLine();
        if (toggle_button("Đánh trước", on_first))
            changed |= on_first ? take_off(first, name) : put_on(first, skip, name);
        ImGui::SetItemTooltip(on_first ? "Đang trong Đánh trước. Bấm lần nữa để bỏ ra."
                                       : "Đánh con này trước những con khác.");
        ImGui::PopID();
    }

    // ---- the two lists ----
    const auto list_box = [&](const char* title, const char* id, std::vector<std::string>& list,
                              std::vector<std::string>& other, char* typed, std::size_t typed_size) {
        ImGui::Dummy(ImVec2(0, 6));
        heading(strong_font_, title);
        ImGui::PushID(id);
        if (list.empty()) hint("Chưa có con nào.");
        for (std::size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            label(list[i].c_str());
            ImGui::SameLine(260);
            const bool gone = ImGui::SmallButton("Bỏ");
            ImGui::PopID();
            if (gone) {
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                break;
            }
        }
        const bool full = list.size() >= kMaxMonsterNames;
        ImGui::SetNextItemWidth(240);
        const bool entered = ImGui::InputTextWithHint("##typed", "gõ tên quái", typed, typed_size,
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(full);
        const bool add = ImGui::Button("Thêm");
        ImGui::EndDisabled();
        if (full)
            ImGui::SetItemTooltip("Tối đa %d tên.", static_cast<int>(kMaxMonsterNames));
        if ((add || entered) && !full) {
            for (const std::string& name : split_monster_names(typed))
                changed |= put_on(list, other, name);
            typed[0] = '\0';
        }
        ImGui::PopID();
    };
    list_box("Không đánh", "skip", skip, first, ui.skip_typed, sizeof ui.skip_typed);
    hint("Quái bay không muốn đánh: thêm vào Không đánh. Boss: thêm vào Đánh trước.");
    list_box("Đánh trước", "first", first, skip, ui.first_typed, sizeof ui.first_typed);
    if (ImGui::Checkbox("Chỉ đánh những con trong danh sách Đánh trước", &hunt_config_.first_only))
        changed = true;
    ImGui::SetItemTooltip("Như \"Loại mob\" của GoC: bỏ qua mọi quái khác.");
    if (hunt_config_.first_only && hunt_config_.first_names.empty()) coloured(kWait, kFirstOnlyEmptyVi);

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("Đóng")) ImGui::CloseCurrentPopup();

    // Saved at once, and a running hunter takes it on its next look.
    if (changed) simple_changed();
    ImGui::EndPopup();
}

} // namespace gobot
