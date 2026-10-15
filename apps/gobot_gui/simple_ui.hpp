#pragma once

// The simple screen's look, for the tabs that live in files of their own.
//
// The same colours, sizes and cards simple_view.cpp draws its tabs with, so
// that a tab written elsewhere cannot be told apart from the others. They are
// copies of that file's own, which keeps them: it is shared with work going on
// beside this, and moving them out of it would make every change there clash.
// Change one, change both.

#include "imgui.h"

#include <cstddef>
#include <string>
#include <vector>

namespace gobot::simple_ui {

inline const ImVec4 kGood (0.35f, 0.82f, 0.42f, 1.0f);
inline const ImVec4 kBad  (0.92f, 0.35f, 0.35f, 1.0f);
inline const ImVec4 kWait (0.95f, 0.72f, 0.25f, 1.0f);
inline const ImVec4 kDim  (0.62f, 0.64f, 0.68f, 1.0f);

inline constexpr float kSmall   = 15.0f;
inline constexpr float kHeading = 21.0f;

inline void coloured(const ImVec4& c, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/// A line of explanation under something: smaller, greyer, wrapped.
inline void hint(const char* text) {
    ImGui::PushFont(nullptr, kSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/// Text that sits on the same line as buttons and lines up with their labels.
inline void label(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
}

/// A heading inside a card.
inline void heading(ImFont* strong, const char* text) {
    ImGui::PushFont(strong, kHeading);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
}

/// How far in a line under a checkbox starts, to sit under its words.
inline float mark_indent() {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
}

/// A card: a bordered panel sized to what is in it, a shade lighter than the
/// window so the cards read as separate steps.
inline bool begin_card(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    const bool open = ImGui::BeginChild(id, ImVec2(0, 0),
                                        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                                        ImGuiChildFlags_AlwaysUseWindowPadding);
    // The panel's own colour is for the panel; anything nested inside it
    // would otherwise paint it again.
    ImGui::PopStyleColor();
    return open;
}

inline void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::Dummy(ImVec2(0, 6));
}

/// The last few lines a worker wrote, small and grey.
inline void last_lines(const std::vector<std::string>& lines, std::size_t n) {
    for (std::size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); ++i)
        hint(lines[i].c_str());
}

// ---- what has not run on the real game yet ----
//
// These have passed every test on the machine they were written on and have
// never run on the real game. Each is marked where the player meets it: a
// whole new tab on its first line, a new switch at the start of its hint. The
// words are here, once, and so is the switch for each: a live test that
// settles one turns its mark off with a one-line change.
inline constexpr bool kTrialChat   = true;   ///< Tự chat
inline constexpr bool kTrialLogin  = true;   ///< Tự đăng nhập, with no account
inline constexpr bool kTrialQuest  = true;   ///< Nhiệm vụ
inline constexpr bool kTrialAlerts = true;   ///< Thông báo
inline constexpr bool kTrialBuffs  = true;   ///< Tự buff, seconds per key
inline constexpr bool kTrialRevive = true;   ///< Tự hồi sinh, with a revive item
inline constexpr bool kTrialInvite = true;   ///< Tự mời người chat từ khoá, at 1280x720

inline constexpr const char* kTrialTabText =
    "Thử nghiệm: phần này chưa chạy trên game thật. Thử từng bước và ngồi xem; thấy lạ thì bấm F12.";
inline constexpr const char* kTrialRowText = "(Thử nghiệm) ";

/// A tab's first line, when its switch is on.
inline void trial_line(bool on) {
    if (!on) return;
    ImGui::PushFont(nullptr, kSmall);
    coloured(kWait, kTrialTabText);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
}

/// A row's hint, with the mark in front when its switch is on.
inline std::string trial_hint(bool on, const char* text) {
    return on ? std::string(kTrialRowText) + text : std::string(text);
}

} // namespace gobot::simple_ui
