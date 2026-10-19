// Đánh quái's skills that follow the fight: "Chỉ khi đang đánh quái" and
// "Chi tiết…" on the "Tự dùng kỹ năng" row, the "Kỹ năng" window, the key
// sequence said where a player sees it, and what the state card says of
// skills held back.
//
// The rules are the macro's (MacroConfig::skills_follow_fight,
// gobot/macro.hpp) and whether there is a fight is the hunter's
// (Hunter::engaged). Here is only what the window draws. The full screen's
// macro table edits the same keys, and both save the whole macro.ini: every
// change here goes through simple_changed, like the other rows. In a file of
// its own so that simple_view.cpp gets only the calls.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/common.hpp"
#include "gobot/macro.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <windows.h>    // the VK_ codes keys are stored as

namespace gobot {

/// The "Kỹ năng" window, asked for.
struct GuiApp::SkillsUi {
    bool open = false;
};

void GuiApp::SkillsUiFree::operator()(SkillsUi* ui) const { delete ui; }

namespace {

using namespace simple_ui;

const char* const kSkillsTitle = "Kỹ năng";

/// The box of a skill that keeps its own timer.
const ImVec4 kOwnTimer(0.28f, 0.40f, 0.62f, 1.0f);

/// A key's name as the key boxes show it.
std::string key_text(std::uint32_t vk) {
    switch (vk) {
        case VK_UP:    return "↑";
        case VK_DOWN:  return "↓";
        case VK_LEFT:  return "←";
        case VK_RIGHT: return "→";
        default:       break;
    }
    return InputManager::key_name(vk);
}

/// The sequence's keys as a player reads them, "Z, X, C"; empty for a
/// sequence of pauses only.
std::string sequence_keys(const MacroConfig& cfg) {
    std::string out;
    for (const MacroStep& s : cfg.sequence)
        if (s.keycode != 0) out += (out.empty() ? "" : ", ") + key_text(s.keycode);
    return out;
}

void small_note(const ImVec4& colour, const std::string& text) {
    ImGui::PushFont(nullptr, kSmall);
    coloured(colour, text.c_str());
    ImGui::PopFont();
}

} // namespace

GuiApp::SkillsUi& GuiApp::skills_ui() {
    if (!skills_ui_) skills_ui_.reset(new SkillsUi);
    return *skills_ui_;
}

bool GuiApp::attack_key_is_skill() const {
    const std::uint32_t attack = hunt_config_.attack_key;
    if (attack == 0) return false;
    for (const MacroKey& k : macro_config_.keys)
        if (k.enabled && k.keycode == attack && macro_key_kind(k) == MacroKeyKind::Skill) return true;
    return false;
}

void GuiApp::simple_skills_line() {
    SkillsUi& ui = skills_ui();
    MacroConfig& cfg = macro_config_;

    // Beside the row's "+".
    ImGui::SameLine();
    if (ImGui::Button("Chi tiết…##skills")) ui.open = true;
    ImGui::SetItemTooltip("Từng kỹ năng: hồi chiêu, tầm, luôn bấm theo giờ. Và dùng skill tuần tự, mức "
                          "MP không dùng kỹ năng.");

    if (ImGui::Checkbox("Chỉ khi đang đánh quái", &cfg.skills_follow_fight)) simple_changed();
    ImGui::SetItemTooltip("Như GoC: chỉ bấm kỹ năng khi bot đang đánh một con quái trong tầm và đã "
                          "quay về phía nó.\nBỏ dấu: bấm theo giờ như trước.");

    if (!cfg.skills_follow_fight) {
        hint("Bấm các phím kỹ năng liên tục, mỗi phím theo giờ hồi chiêu của nó.");
    } else if (!hunt_config_.enabled) {
        hint("Cần bật Tự đánh quái thì kỹ năng mới theo trận đánh; không thì bấm theo giờ như thường.");
    } else {
        std::string text = cfg.sequential_skills
                               ? "Chỉ dùng kỹ năng khi đang đánh quái, lần lượt từng phím, mỗi phím chờ "
                                 "hồi chiêu."
                               : "Chỉ dùng kỹ năng khi đang đánh quái, mỗi lần một phím đã hồi chiêu.";
        if (cfg.skill_mp_floor_pct > 0)
            text += " MP dưới " + std::to_string(cfg.skill_mp_floor_pct) +
                    "% thì không dùng kỹ năng.";
        hint(trial_hint(kTrialSkills, text.c_str()).c_str());
        if (attack_key_is_skill())
            small_note(kWait, key_text(hunt_config_.attack_key) +
                                  " vừa là phím đánh vừa là kỹ năng: khi đánh quái bot dùng nó làm phím "
                                  "đánh. Nên đặt Phím đánh là Space (Đánh thường).");
    }
    simple_sequence_note();
}

void GuiApp::simple_sequence_note() {
    MacroConfig& cfg = macro_config_;
    if (!cfg.sequence_enabled) return;
    const std::string keys = sequence_keys(cfg);
    if (keys.empty()) return;           // pauses only: it presses nothing
    // Set on the full screen, and pressing whenever the macro runs: said
    // here, and switched off here, but never changed behind the player's back.
    small_note(kWait, "Chuỗi phím (màn hình Nâng cao) đang bật: " + keys +
                          " — bot bấm cả khi Tự dùng kỹ năng không được đánh dấu.");
    if (ImGui::SmallButton("Tắt##sequence")) {
        cfg.sequence_enabled = false;
        simple_changed();
    }
    ImGui::SetItemTooltip("Tắt chuỗi phím. Bật lại ở màn hình Nâng cao, thẻ Macro.");
    if (cfg.skills_follow_fight) {
        ImGui::SameLine();
        hint("Với Chỉ khi đang đánh quái, chuỗi phím cũng chỉ bấm khi đang đánh quái.");
    }
}

std::string GuiApp::simple_skills_note(bool no_player) {
    if (!macro_config_.skills_follow_fight || !macro_running_) return {};
    if (no_player) return " Kỹ năng chờ cho tới khi thấy nhân vật.";
    const int floor_pct = macro_config_.skill_mp_floor_pct;
    const float mp = vitals_running_ ? vitals_.mp_percent() : -1.0f;
    if (floor_pct > 0 && mp >= 0.0f && mp < static_cast<float>(floor_pct))
        return " (MP dưới " + std::to_string(floor_pct) + "%: không dùng kỹ năng)";
    return {};
}

void GuiApp::simple_skills_popup() {
    SkillsUi& ui = skills_ui();
    if (ui.open) {
        ui.open = false;
        ImGui::OpenPopup(kSkillsTitle);
    }
    ImGui::SetNextWindowSize(ImVec2(720, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kSkillsTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    MacroConfig& cfg = macro_config_;
    bool changed = false;
    trial_line(kTrialSkills);

    changed |= ImGui::Checkbox("Chỉ khi đang đánh quái", &cfg.skills_follow_fight);
    ImGui::SetItemTooltip("Chỉ bấm kỹ năng khi bot đang đánh một con quái trong tầm và đã quay về phía "
                          "nó. Cần bật Tự đánh quái.\nBỏ dấu: mỗi phím bấm theo giờ như trước.");
    changed |= ImGui::Checkbox("Dùng skill tuần tự", &cfg.sequential_skills);
    ImGui::SetItemTooltip("Như GoC. Có dấu: mọi kỹ năng đã hồi chiêu được bấm lần lượt theo thứ tự, "
                          "cách nhau 50 ms.\nBỏ dấu: mỗi lần chỉ bấm kỹ năng đầu tiên đã hồi chiêu, "
                          "300 ms một lần.");
    label("Khi MP dưới");
    ImGui::SameLine();
    int floor_pct = cfg.skill_mp_floor_pct;
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("##mpfloor", &floor_pct, 1, 5)) {
        cfg.skill_mp_floor_pct = std::clamp(floor_pct, 0, MacroConfig::kSkillMaxMpFloor);
        changed = true;
    }
    ImGui::SetItemTooltip("0: luôn dùng kỹ năng. GoC dùng 10. Đòn đánh thường của bot vẫn tiếp tục.");
    ImGui::SameLine();
    label("% không dùng kỹ năng");
    if (!cfg.skills_follow_fight)
        hint("Các mục dưới đây (trừ phím, hồi chiêu và bật/tắt) chỉ có tác dụng khi \"Chỉ khi đang đánh "
             "quái\" có dấu.");

    ImGui::Dummy(ImVec2(0, 4));
    const std::vector<std::size_t> skills = macro_keys_of_kind(cfg.keys, MacroKeyKind::Skill);
    if (skills.empty()) hint("Chưa có phím kỹ năng nào. Thêm bằng nút + ở dòng Tự dùng kỹ năng.");
    if (!skills.empty() &&
        ImGui::BeginTable("##skilltable", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Bật");
        ImGui::TableSetupColumn("Phím");
        ImGui::TableSetupColumn("Hồi chiêu (giây)");
        ImGui::TableSetupColumn("Tầm (px)");
        ImGui::TableSetupColumn("Luôn bấm theo giờ");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const std::size_t i : skills) {
            MacroKey& k = cfg.keys[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(k.keycode == 0);
            changed |= ImGui::Checkbox("##on", &k.enabled);
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            if (k.always_timer) ImGui::PushStyleColor(ImGuiCol_Button, kOwnTimer);
            const std::uint32_t before = k.keycode;
            if (simple_key_box("key", k.keycode, true, !k.enabled,
                               k.always_timer ? "Luôn bấm theo giờ, kể cả khi không đánh quái."
                                              : nullptr)) {
                if (before == 0 && k.keycode != 0) k.enabled = true;
                if (k.keycode == 0) k.enabled = false;
                changed = true;
            }
            if (k.always_timer) ImGui::PopStyleColor();

            // In tenths of a second, from half a second to 59: a minute or
            // more and the key would be a buff and leave this list.
            ImGui::TableNextColumn();
            float secs = static_cast<float>(k.interval_ms) / 1000.0f;
            ImGui::SetNextItemWidth(130);
            if (ImGui::InputFloat("##cd", &secs, 0.1f, 1.0f, "%.1f")) {
                const long ms = std::lround(static_cast<double>(std::max(secs, 0.0f)) * 1000.0);
                k.interval_ms = clamp_skill_ms(static_cast<std::uint32_t>(ms));
                changed = true;
            }
            ImGui::SetItemTooltip("Hồi chiêu của kỹ năng này, %.1f–%.0f giây.",
                                  static_cast<double>(kSkillMinMs) / 1000.0,
                                  static_cast<double>(kSkillMaxMs) / 1000.0);

            ImGui::TableNextColumn();
            int range = k.range_px;
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("##range", &range, 10, 100)) {
                k.range_px = std::clamp(range, 0, kSkillMaxRangePx);
                changed = true;
            }
            ImGui::SetItemTooltip("Quái lệch sang bên xa hơn số này thì kỹ năng chờ. 0 = mọi khoảng cách.");

            ImGui::TableNextColumn();
            changed |= ImGui::Checkbox("##own", &k.always_timer);
            ImGui::SetItemTooltip("Luôn bấm theo giờ, kể cả khi không đánh quái -- cho kỹ năng tự buff.");

            ImGui::TableNextColumn();
            // Emptied, it goes from macro.ini with the next frame of the row
            // (drop_empty_keys), as a cleared box does.
            if (ImGui::SmallButton("Xóa")) {
                k.keycode = 0;
                k.enabled = false;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (attack_key_is_skill())
        small_note(kWait, key_text(hunt_config_.attack_key) +
                              " vừa là phím đánh vừa là kỹ năng: khi đánh quái bot dùng nó làm phím đánh. "
                              "Nên đặt Phím đánh là Space (Đánh thường).");
    simple_sequence_note();

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("Đóng")) ImGui::CloseCurrentPopup();
    // Saved at once, and a running macro takes it on its next pass.
    if (changed) simple_changed();
    ImGui::EndPopup();
}

} // namespace gobot
