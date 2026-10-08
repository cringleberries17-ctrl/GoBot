// The simple screen: what a player sees first.
//
// One screen, in Vietnamese, top to bottom in the order things have to
// happen -- get ready, choose what the bot does, start it. Every control here
// drives the same engines as the full window; nothing is a second
// implementation. The full window is one click away behind "Nâng cao" and has
// a button back.
//
// Laid out after the arrangement chosen for it:
//
//     Trước khi bắt đầu      ✓ game  ✓ character's name  ✓ HP/MP read
//     HP ██████████░░  8939 / 9232
//     MP ████████░░░░  7917 / 8902
//     Bot sẽ làm gì          [✓] fight  key [Z]   [✓] HP potion [1] <50%  ...
//     ►  BẮT ĐẦU  (F10)
//     what it is doing, and what it has done
//                                                            Nâng cao ►

#include "gui_app.hpp"

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <windows.h>    // the VK_ codes keys are stored as

namespace gobot {
namespace {

const ImVec4 kGood (0.35f, 0.82f, 0.42f, 1.0f);
const ImVec4 kBad  (0.92f, 0.35f, 0.35f, 1.0f);
const ImVec4 kWait (0.95f, 0.72f, 0.25f, 1.0f);
const ImVec4 kDim  (0.62f, 0.64f, 0.68f, 1.0f);
const ImVec4 kInk  (0.05f, 0.05f, 0.05f, 1.0f);

constexpr float kBody    = 18.0f;
constexpr float kSmall   = 15.0f;
constexpr float kHeading = 21.0f;
constexpr float kButton  = 27.0f;

/// Widest the column of controls is allowed to get. The window is sized for
/// the full panels; a checklist stretched across all of it reads as scattered.
constexpr float kColumn = 700.0f;

constexpr float kKeyBox = 58.0f;

enum class Mark { Good, Bad, Wait };

/// The round mark at the start of a checklist line: a tick, a cross, or
/// three dots for "not yet". Drawn rather than typed, because none of the
/// faces with Vietnamese in them has a tick.
void mark(Mark m) {
    const float d = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
    const float r = d * 0.40f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddCircleFilled(c, r, ImGui::GetColorU32(m == Mark::Good ? kGood
                                                 : m == Mark::Bad ? kBad : kWait));
    const ImU32 ink = ImGui::GetColorU32(kInk);
    const float t = std::max(2.0f, r * 0.24f);
    if (m == Mark::Good) {
        const ImVec2 tick[3] = {
            ImVec2(c.x - r * 0.50f, c.y + r * 0.02f),
            ImVec2(c.x - r * 0.14f, c.y + r * 0.38f),
            ImVec2(c.x + r * 0.50f, c.y - r * 0.36f),
        };
        dl->AddPolyline(tick, 3, ink, ImDrawFlags_None, t);
    } else if (m == Mark::Bad) {
        const float k = r * 0.40f;
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), ink, t);
        dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), ink, t);
    } else {
        for (int i = -1; i <= 1; ++i)
            dl->AddCircleFilled(ImVec2(c.x + static_cast<float>(i) * r * 0.48f, c.y),
                                r * 0.14f, ink);
    }
    ImGui::Dummy(ImVec2(d, d));
}

void coloured(const ImVec4& c, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/// A line of explanation under something: smaller, greyer, wrapped.
void hint(const char* text) {
    ImGui::PushFont(nullptr, kSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/// Text that sits on the same line as buttons and lines up with their labels.
void label(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
}

/// A heading inside a card.
void heading(ImFont* strong, const char* text) {
    ImGui::PushFont(strong, kHeading);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
}

/// Indent a hint so it lines up with the text after a checklist mark.
float mark_indent() {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
}

/// A key's name as a player would write it.
///
/// InputManager's names cover letters, digits and the function keys; the
/// punctuation keys come out of it as hex, which nobody reads as "the key
/// next to L".
std::string key_label(std::uint32_t vk) {
    switch (vk) {
        case VK_OEM_1:      return ";";
        case VK_OEM_PLUS:   return "=";
        case VK_OEM_COMMA:  return ",";
        case VK_OEM_MINUS:  return "-";
        case VK_OEM_PERIOD: return ".";
        case VK_OEM_2:      return "/";
        case VK_OEM_3:      return "`";
        case VK_OEM_4:      return "[";
        case VK_OEM_5:      return "\\";
        case VK_OEM_6:      return "]";
        case VK_OEM_7:      return "'";
        case VK_MULTIPLY:   return "Num *";
        case VK_ADD:        return "Num +";
        case VK_SUBTRACT:   return "Num -";
        case VK_DECIMAL:    return "Num .";
        case VK_DIVIDE:     return "Num /";
        case VK_INSERT:     return "Insert";
        case VK_DELETE:     return "Delete";
        case VK_HOME:       return "Home";
        case VK_END:        return "End";
        case VK_PRIOR:      return "PgUp";
        case VK_NEXT:       return "PgDn";
        default: break;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num " + std::to_string(vk - VK_NUMPAD0);
    return InputManager::key_name(vk);
}

/// The key pressed this frame, as the VK the game is sent, or 0.
///
/// Read from ImGui's own key events: the box is clicked first, so this window
/// has the keyboard, and ImGui already knows which key went down. Modifiers
/// map to the side-neutral codes, which is what the game's own key settings
/// use.
std::uint32_t pressed_vk() {
    struct Pair { ImGuiKey key; std::uint32_t vk; };
    static const Pair kNamed[] = {
        { ImGuiKey_Space, VK_SPACE },       { ImGuiKey_Enter, VK_RETURN },
        { ImGuiKey_Tab, VK_TAB },
        { ImGuiKey_LeftArrow, VK_LEFT },    { ImGuiKey_RightArrow, VK_RIGHT },
        { ImGuiKey_UpArrow, VK_UP },        { ImGuiKey_DownArrow, VK_DOWN },
        { ImGuiKey_Insert, VK_INSERT },     { ImGuiKey_Home, VK_HOME },
        { ImGuiKey_End, VK_END },           { ImGuiKey_PageUp, VK_PRIOR },
        { ImGuiKey_PageDown, VK_NEXT },
        { ImGuiKey_LeftCtrl, VK_CONTROL },  { ImGuiKey_RightCtrl, VK_CONTROL },
        { ImGuiKey_LeftShift, VK_SHIFT },   { ImGuiKey_RightShift, VK_SHIFT },
        { ImGuiKey_LeftAlt, VK_MENU },      { ImGuiKey_RightAlt, VK_MENU },
        { ImGuiKey_Semicolon, VK_OEM_1 },   { ImGuiKey_Equal, VK_OEM_PLUS },
        { ImGuiKey_Comma, VK_OEM_COMMA },   { ImGuiKey_Minus, VK_OEM_MINUS },
        { ImGuiKey_Period, VK_OEM_PERIOD }, { ImGuiKey_Slash, VK_OEM_2 },
        { ImGuiKey_GraveAccent, VK_OEM_3 }, { ImGuiKey_LeftBracket, VK_OEM_4 },
        { ImGuiKey_Backslash, VK_OEM_5 },   { ImGuiKey_RightBracket, VK_OEM_6 },
        { ImGuiKey_Apostrophe, VK_OEM_7 },
        { ImGuiKey_KeypadMultiply, VK_MULTIPLY }, { ImGuiKey_KeypadAdd, VK_ADD },
        { ImGuiKey_KeypadSubtract, VK_SUBTRACT }, { ImGuiKey_KeypadDecimal, VK_DECIMAL },
        { ImGuiKey_KeypadDivide, VK_DIVIDE },
    };

    for (int i = 0; i < 26; ++i)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + i), false))
            return static_cast<std::uint32_t>('A' + i);
    for (int i = 0; i < 10; ++i)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + i), false))
            return static_cast<std::uint32_t>('0' + i);
    for (int i = 0; i < 12; ++i)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1 + i), false))
            return static_cast<std::uint32_t>(VK_F1 + i);
    for (int i = 0; i < 10; ++i)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + i), false))
            return static_cast<std::uint32_t>(VK_NUMPAD0 + i);
    for (const Pair& p : kNamed)
        if (ImGui::IsKeyPressed(p.key, false)) return p.vk;
    return 0;
}

std::string clock_text(std::uint64_t ms) {
    const std::uint64_t s = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%llu:%02llu:%02llu",
                  static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>(s / 60 % 60),
                  static_cast<unsigned long long>(s % 60));
    return buf;
}

std::string seconds_text(std::uint32_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(ms) / 1000.0);
    std::string s = buf;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

bool any_key_on(const MacroConfig& cfg) {
    return std::any_of(cfg.keys.begin(), cfg.keys.end(),
                       [](const MacroKey& k) { return k.enabled && k.keycode != 0; });
}

/// A card: a bordered panel sized to what is in it, a shade lighter than the
/// window so the three of them read as separate steps.
bool begin_card(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    const bool open = ImGui::BeginChild(id, ImVec2(0, 0),
                                        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                                        ImGuiChildFlags_AlwaysUseWindowPadding);
    // The panel's own colour is for the panel. Anything nested inside it --
    // the table, the window list -- would otherwise paint it again.
    ImGui::PopStyleColor();
    return open;
}

void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::Dummy(ImVec2(0, 6));
}

} // namespace

// ------------------------------------------------------------------ frame

void GuiApp::draw_simple_view() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings;

    // A key box waiting for a key needs every key, including the ones ImGui
    // would otherwise take for moving between controls: Space, Enter and the
    // arrows are all things a skill can be bound to.
    ImGuiIO& io = ImGui::GetIO();
    if (simple_listening_ != 0) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    else                        io.ConfigFlags |=  ImGuiConfigFlags_NavEnableKeyboard;

    // The keys a player is most likely to look for are the potion ones, and
    // a config whose potion keys were deleted on the full screen would leave
    // those rows with nothing to show. Put them back, switched off; nothing is
    // written to disk unless something is changed.
    if (!simple_seeded_) {
        simple_seeded_ = true;
        const struct { MacroKeyKind kind; MacroTrigger trigger; std::uint32_t vk;
                       int threshold; const char* name; } kPotions[] = {
            { MacroKeyKind::HpPotion, MacroTrigger::HpBelow, '1', 50, "HP potion" },
            { MacroKeyKind::MpPotion, MacroTrigger::MpBelow, '2', 30, "MP potion" },
        };
        for (const auto& p : kPotions) {
            const bool have = std::any_of(macro_config_.keys.begin(), macro_config_.keys.end(),
                [&](const MacroKey& k) { return macro_key_kind(k) == p.kind; });
            if (have || macro_config_.keys.size() >= MacroConfig::kMaxKeys) continue;
            MacroKey k;
            k.label = p.name;
            k.keycode = p.vk;
            k.trigger = p.trigger;
            k.threshold_pct = p.threshold;
            k.interval_ms = 3000;
            k.jitter_pct = 15;
            macro_config_.keys.push_back(std::move(k));
        }
        apply_macro_config();
    }

    // Reading the bars is passive -- it looks at the screen and presses
    // nothing -- so it starts as soon as there is a game to look at, and the
    // checklist can say whether it works before anything is switched on.
    if (input_.window_valid() && !vitals_running_ && !simple_reader_tried_) {
        simple_reader_tried_ = true;
        start_vitals();
    }

    ImGui::Begin("GoBot##simple", nullptr, flags);
    if (text_font_) ImGui::PushFont(text_font_, kBody);
    else            ImGui::PushFont(nullptr, 0.0f);

    // One centred column.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float width = std::min(avail, kColumn);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));    // a column, not a panel
    ImGui::BeginChild("##column", ImVec2(width, 0), ImGuiChildFlags_None);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 4));
    simple_checklist();
    simple_jobs();
    simple_start_block();

    // The way to everything else, out of the way at the bottom.
    ImGui::Dummy(ImVec2(0, 4));
    {
        const char* adv = "Nâng cao ►";
        const float w = ImGui::CalcTextSize(adv).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        if (ImGui::Button(adv)) {
            simple_view_ = false;
            simple_listening_ = 0;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        }
        ImGui::SetItemTooltip("Mọi cài đặt chi tiết (tiếng Anh).\n"
                              "Nút \"◄ Giao diện đơn giản\" ở góc trên bên trái để quay lại.");
    }

    simple_window_list();

    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::End();
}

// -------------------------------------------------------------- checklist

void GuiApp::simple_checklist() {
    if (!begin_card("##ready")) { end_card(); return; }
    heading(strong_font_, "Trước khi bắt đầu");

    const bool have_game = input_.window_valid();

    // ---- the game ----
    mark(have_game ? Mark::Good : Mark::Bad);
    ImGui::SameLine();
    if (have_game) {
        label("Đã tìm thấy game");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("(%s)", app_config_.window_title.c_str());
        ImGui::PopStyleColor();
    } else {
        label("Chưa tìm thấy game");
        ImGui::SameLine();
        if (ImGui::Button("Tìm lại")) {
            windows_ = InputManager::enumerate_windows();
            window_list_loaded_ = true;
            input_.find_game_window(app_config_.window_title, app_config_.window_class,
                                    app_config_.process_name);
            if (input_.window_valid()) {
                apply_vitals_config();
                simple_reader_tried_ = false;     // look at the bars again
                simple_note("Đã tìm thấy game.");
            } else {
                simple_note("Vẫn chưa thấy game. Mở game và vào nhân vật, hoặc bấm "
                            "\"Chọn cửa sổ…\" để tự chọn.");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Chọn cửa sổ…")) simple_pick_window_ = true;
        ImGui::Indent(mark_indent());
        hint("Mở game và vào nhân vật, rồi bấm Tìm lại.");
        ImGui::Unindent(mark_indent());
    }

    // ---- something sitting on top of it ----
    //
    // Frames come off the screen, so whatever covers the game is what gets
    // read. Only shown when it is actually happening.
    if (have_game && overlap_fraction() > 0.02f) {
        mark(Mark::Bad);
        ImGui::SameLine();
        label("Cửa sổ GoBot đang che game");
        ImGui::SameLine();
        if (ImGui::Button("Dời GoBot sang bên")) {
            move_clear_of_game();
            simple_note(overlap_fraction() < 0.02f
                ? "Đã dời GoBot sang bên cạnh game."
                : "Không đủ chỗ bên cạnh game. Hãy tự kéo cửa sổ game hoặc GoBot ra chỗ khác.");
        }
        ImGui::Indent(mark_indent());
        hint("Bot nhìn màn hình game để đọc HP/MP và tìm quái, nên game phải hiện rõ, "
             "không bị gì che.");
        ImGui::Unindent(mark_indent());
    }

    // ---- the character's name ----
    //
    // Needed for fighting only: it is how the bot finds its own character
    // among the name plates. A cross when fighting is ticked, "not yet"
    // otherwise.
    {
        const bool named = !hunt_config_.character_name.empty();
        const bool unseen = hunting_ && hunter_ &&
                            hunter_->status().state == Hunter::State::NoPlayer;
        Mark m = Mark::Good;
        if (!named)      m = hunt_config_.enabled ? Mark::Bad : Mark::Wait;
        else if (unseen) m = Mark::Bad;
        mark(m);
        ImGui::SameLine();
        label("Tên nhân vật");
        ImGui::SameLine();

        char name[64];
        std::snprintf(name, sizeof name, "%s", hunt_config_.character_name.c_str());
        ImGui::SetNextItemWidth(240);
        if (ImGui::InputTextWithHint("##name", "gõ tên nhân vật", name, sizeof name)) {
            hunt_config_.character_name = name;
            detector_.set_character_name(hunt_config_.character_name);
            if (hunter_) hunter_->set_config(hunt_config_);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) simple_changed();

        ImGui::Indent(mark_indent());
        if (unseen)
            hint("Không thấy tên này trên màn hình. Kiểm tra lại chính tả, kể cả chữ hoa "
                 "chữ thường, và để nhân vật hiện rõ trong game.");
        else if (!named)
            hint(hunt_config_.enabled
                 ? "Gõ đúng tên hiện dưới chân nhân vật của bạn. Bot dùng nó để biết bạn "
                   "đang đứng ở đâu."
                 : "Chỉ cần khi bật Tự đánh quái.");
        ImGui::Unindent(mark_indent());
    }

    // ---- HP and MP ----
    {
        const bool hp = vitals_running_ && vitals_.hp_percent() >= 0.0f;
        const bool mp = vitals_running_ && vitals_.mp_percent() >= 0.0f;
        mark(hp && mp ? Mark::Good : hp || !have_game ? Mark::Wait : Mark::Bad);
        ImGui::SameLine();
        if (hp && mp)  label("Đọc được HP và MP");
        else if (hp)   label("Đọc được HP, chưa đọc được MP");
        else           label("Chưa đọc được HP và MP");

        if (have_game && !(hp && mp)) {
            ImGui::SameLine();
            ImGui::BeginDisabled(simple_find_bars_);
            if (ImGui::Button(simple_find_bars_ ? "Đang tìm…" : "Tự tìm")) {
                if (!vitals_running_) start_vitals();
                simple_find_bars_ = true;
                simple_find_bars_since_ms_ = now_ms();
            }
            ImGui::EndDisabled();
            ImGui::Indent(mark_indent());
            hint("Để game hiện rõ trên màn hình rồi bấm Tự tìm. Tốt nhất là lúc HP và MP "
                 "đang đầy.");
            ImGui::Unindent(mark_indent());
        }

        // The search runs on the first frame the reader has, since the button
        // may have been what started it.
        if (simple_find_bars_) {
            Frame frame;
            if (vitals_.latest_frame(frame) && frame.valid()) {
                simple_find_bars_ = false;
                BarSpec hp_found = macro_config_.hp_bar;
                BarSpec mp_found = macro_config_.mp_bar;
                const int n = VitalsReader::find_bars(frame, hp_found, mp_found);
                if (n == 0) {
                    simple_note("Không tìm thấy thanh HP/MP. Để game hiện rõ, không bị cửa "
                                "sổ nào che, rồi thử lại.");
                } else {
                    macro_config_.hp_bar = hp_found;
                    macro_config_.mp_bar = mp_found;
                    apply_vitals_config();
                    simple_changed();
                    simple_note(n == 2 ? "Đã tìm thấy thanh HP và MP."
                                       : "Mới tìm thấy một thanh. Kiểm tra lại số HP/MP bên dưới.");
                }
            } else if (now_ms() - simple_find_bars_since_ms_ > 4000) {
                simple_find_bars_ = false;
                simple_note("Chưa chụp được màn hình game. Để game hiện rõ rồi thử lại.");
            }
        }
    }

    // ---- whether keys reach it ----
    mark(input_injection_ok_ ? Mark::Good : Mark::Bad);
    ImGui::SameLine();
    if (input_injection_ok_) {
        label("Bot bấm phím vào game được");
    } else {
        label("Phím của bot không tới được game");
        if (!elevated_) {
            ImGui::SameLine();
            if (ImGui::Button("Mở lại bằng quyền Admin")) relaunch_elevated();
        }
        ImGui::Indent(mark_indent());
        hint(elevated_ ? "GoBot đã chạy bằng quyền Admin mà phím vẫn bị chặn. Tắt các phần "
                         "mềm macro khác rồi mở lại GoBot."
                       : "Thường là do game chạy bằng quyền Admin còn GoBot thì không.");
        ImGui::Unindent(mark_indent());
    }

    // ---- the bars themselves ----
    ImGui::Dummy(ImVec2(0, 4));
    simple_vitals();

    end_card();
}

void GuiApp::simple_vitals() {
    const auto v = vitals_.values();
    const struct { const char* name; int value, max; float pct; ImVec4 col; } kBars[] = {
        { "HP", v.hp, v.hp_max, vitals_running_ ? vitals_.hp_percent() : -1.0f,
          ImVec4(0.85f, 0.25f, 0.25f, 1.0f) },
        { "MP", v.mp, v.mp_max, vitals_running_ ? vitals_.mp_percent() : -1.0f,
          ImVec4(0.25f, 0.45f, 0.90f, 1.0f) },
    };
    for (const auto& bar : kBars) {
        label(bar.name);
        ImGui::SameLine(58);
        char text[64];
        if (bar.pct < 0.0f)
            std::snprintf(text, sizeof text, "chưa đọc được");
        else if (bar.value >= 0 && bar.max > 0)
            std::snprintf(text, sizeof text, "%d / %d   (%.0f%%)", bar.value, bar.max, bar.pct);
        else
            std::snprintf(text, sizeof text, "%.0f%%", bar.pct);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar.col);
        ImGui::ProgressBar(bar.pct < 0.0f ? 0.0f : bar.pct / 100.0f,
                           ImVec2(-1, ImGui::GetFrameHeight()), text);
        ImGui::PopStyleColor();
    }
}

// ------------------------------------------------------------------- jobs

void GuiApp::simple_jobs() {
    if (!begin_card("##jobs")) { end_card(); return; }
    heading(strong_font_, "Bot sẽ làm gì");

    auto& keys = macro_config_.keys;
    const auto of_kind = [&](MacroKeyKind kind) {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (macro_key_kind(keys[i]) == kind) out.push_back(i);
        return out;
    };
    const auto any_on = [&](const std::vector<std::size_t>& which) {
        for (std::size_t i : which)
            if (keys[i].enabled && keys[i].keycode != 0) return true;
        return false;
    };
    const auto set_all = [&](const std::vector<std::size_t>& which, bool on) {
        for (std::size_t i : which)
            keys[i].enabled = on && keys[i].keycode != 0;
    };

    // Empty boxes go once nothing is waiting to fill them. A box with no key
    // does nothing, and Delete on a key is how a player takes it off.
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    if (simple_listening_ == 0 && simple_listen_key_ == kNone) {
        const auto empty = [](const MacroKey& k) {
            const MacroKeyKind kind = macro_key_kind(k);
            return k.keycode == 0 && (kind == MacroKeyKind::Skill || kind == MacroKeyKind::Buff);
        };
        if (std::any_of(keys.begin(), keys.end(), empty)) {
            keys.erase(std::remove_if(keys.begin(), keys.end(), empty), keys.end());
            simple_changed();
        }
    }

    if (!ImGui::BeginTable("##jobtable", 2, ImGuiTableFlags_SizingFixedFit)) {
        end_card();
        return;
    }
    ImGui::TableSetupColumn("job", ImGuiTableColumnFlags_WidthFixed, 205.0f);
    ImGui::TableSetupColumn("how", ImGuiTableColumnFlags_WidthStretch);

    const auto row_gap = [] { ImGui::Dummy(ImVec2(0, 3)); };

    // Put the next control beside the last one if it fits before `right`, and
    // under it if not. Seven skill boxes are wider than the card, and the one
    // that ran off the edge was the + that adds an eighth.
    const auto beside = [](float right, float width, float gap = -1.0f) {
        const float spacing = gap >= 0.0f ? gap : ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::GetItemRectMax().x + spacing + width <= right) ImGui::SameLine(0, gap);
    };

    // A row of key boxes, one per key of a kind. Returns the right-hand edge
    // it wrapped at, for whatever follows on the same row.
    const auto key_row = [&](const std::vector<std::size_t>& which, const char* id,
                             bool show_interval) {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (std::size_t n = 0; n < which.size(); ++n) {
            MacroKey& k = keys[which[n]];
            if (n) beside(right, kKeyBox);
            ImGui::PushID(static_cast<int>(which[n]));
            if (which[n] == simple_listen_key_) {
                simple_listen_key_ = kNone;
                ImGui::PushID(id);
                simple_listening_ = ImGui::GetID("##box");
                ImGui::PopID();
            }
            const std::string tip = show_interval
                ? "Bấm mỗi " + seconds_text(k.interval_ms) + " giây." : std::string();
            const std::uint32_t before = k.keycode;
            if (simple_key_box(id, k.keycode, true, !k.enabled,
                               tip.empty() ? nullptr : tip.c_str())) {
                // Filling in an empty box means "use this one".
                if (before == 0 && k.keycode != 0) k.enabled = true;
                if (k.keycode == 0) k.enabled = false;
                simple_changed();
            }
            ImGui::PopID();
        }
        if (!which.empty()) beside(right, ImGui::GetFrameHeight());
        return right;
    };

    // ---- fighting ----
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::Checkbox("Tự đánh quái", &hunt_config_.enabled)) simple_changed();
    ImGui::TableNextColumn();
    label("Phím đánh");
    ImGui::SameLine();
    if (simple_key_box("attack", hunt_config_.attack_key, false, false, nullptr))
        simple_changed();
    hint("Tự đi tới con quái gần nhất và đánh nó.");
    row_gap();

    // ---- skills ----
    {
        const auto skills = of_kind(MacroKeyKind::Skill);
        bool on = any_on(skills);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("Tự dùng kỹ năng", &on)) {
            set_all(skills, on);
            simple_changed();
        }
        ImGui::TableNextColumn();
        (void)key_row(skills, "skill", true);
        ImGui::BeginDisabled(keys.size() >= MacroConfig::kMaxKeys);
        if (ImGui::Button("+##skill", ImVec2(ImGui::GetFrameHeight(), 0))) {
            simple_listen_key_ = keys.size();
            MacroKey k;
            k.label = "Skill " + std::to_string(skills.size() + 1);
            k.interval_ms = 3000;
            k.start_delay_ms = static_cast<std::uint32_t>(keys.size()) * 150;
            keys.push_back(std::move(k));
            simple_changed();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Thêm một phím kỹ năng: bấm + rồi nhấn phím (bấm mỗi 3 giây).");
        hint("Bấm các phím kỹ năng liên tục, mỗi phím theo giờ hồi chiêu của nó.");
        row_gap();
    }

    // ---- potions ----
    const auto potion_row = [&](MacroKeyKind kind, const char* job, const char* bar,
                                const char* what) {
        const auto which = of_kind(kind);
        if (which.empty()) return;          // there was no room left to add one
        MacroKey& k = keys[which.front()];
        ImGui::PushID(job);

        bool on = any_on(which);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Checkbox(job, &on)) {
            set_all(which, on);
            simple_changed();
        }
        ImGui::TableNextColumn();
        label("Phím");
        ImGui::SameLine();
        if (simple_key_box("potion", k.keycode, false, !k.enabled, nullptr)) simple_changed();
        ImGui::SameLine();
        label(bar);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        if (ImGui::SliderInt("##below", &k.threshold_pct, 5, 95, "%d%%"))
            apply_macro_config();
        if (ImGui::IsItemDeactivatedAfterEdit()) simple_changed();
        hint(what);
        row_gap();
        ImGui::PopID();
    };
    potion_row(MacroKeyKind::HpPotion, "Tự uống bình HP", "khi HP dưới",
               "Uống bình khi máu xuống thấp.");
    potion_row(MacroKeyKind::MpPotion, "Tự uống bình MP", "khi MP dưới",
               "Uống bình khi mana xuống thấp.");

    // ---- buffs ----
    {
        const auto buffs = of_kind(MacroKeyKind::Buff);
        bool on = any_on(buffs);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("Tự buff", &on)) {
            set_all(buffs, on);
            simple_changed();
        }
        ImGui::TableNextColumn();
        const float right = key_row(buffs, "buff", false);
        ImGui::BeginDisabled(keys.size() >= MacroConfig::kMaxKeys);
        if (ImGui::Button("+##buff", ImVec2(ImGui::GetFrameHeight(), 0))) {
            simple_listen_key_ = keys.size();
            MacroKey k;
            k.label = "Buff " + std::to_string(buffs.size() + 1);
            k.interval_ms = buffs.empty() ? 300000 : keys[buffs.front()].interval_ms;
            k.jitter_pct = 5;
            k.start_delay_ms = static_cast<std::uint32_t>(keys.size()) * 150;
            keys.push_back(std::move(k));
            simple_changed();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Thêm một phím buff: bấm + rồi nhấn phím.");

        // One interval for all of them: buffs here last minutes, and a player
        // who needs them on different clocks has the full screen for it.
        if (!buffs.empty()) {
            const ImGuiStyle& st = ImGui::GetStyle();
            const float every = ImGui::CalcTextSize("mỗi").x + 105.0f +
                                ImGui::CalcTextSize("phút").x + st.ItemSpacing.x * 2.0f;
            beside(right, every, 16.0f);
            label("mỗi");
            ImGui::SameLine();
            int minutes = static_cast<int>(keys[buffs.front()].interval_ms / 60000);
            ImGui::SetNextItemWidth(105);
            if (ImGui::InputInt("##minutes", &minutes, 1, 5)) {
                minutes = std::clamp(minutes, 1, 120);
                for (std::size_t i : buffs)
                    keys[i].interval_ms = static_cast<std::uint32_t>(minutes) * 60000u;
                simple_changed();
            }
            ImGui::SameLine();
            label("phút");
        }
        hint("Dùng lại các phím buff sau mỗi khoảng thời gian.");
    }

    ImGui::EndTable();
    end_card();
}

bool GuiApp::simple_key_box(const char* id, std::uint32_t& vk, bool can_clear, bool dimmed,
                            const char* tip) {
    ImGui::PushID(id);
    const ImGuiID me = ImGui::GetID("##box");
    bool changed = false;

    // Read the key before drawing, so the box shows the result this frame.
    if (simple_listening_ == me) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            simple_listening_ = 0;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (can_clear && vk != 0) { vk = 0; changed = true; }
            simple_listening_ = 0;
        } else if (const std::uint32_t got = pressed_vk()) {
            // The bot's own switches cannot double as game keys: pressing a
            // potion key would stop the bot.
            if (got == macro_config_.toggle_key || got == macro_config_.panic_key ||
                got == hunt_config_.toggle_key) {
                simple_note("Phím " + key_label(got) + " dùng để bật/tắt bot. Hãy chọn phím khác.");
            } else {
                changed = got != vk;
                vk = got;
                simple_listening_ = 0;
            }
        }
    }

    const bool listening = simple_listening_ == me;
    std::string text = listening ? "…" : vk ? key_label(vk) : "—";
    text += "##box";

    int colours = 0;
    if (listening) {
        ImGui::PushStyleColor(ImGuiCol_Button, kWait);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kWait);
        ImGui::PushStyleColor(ImGuiCol_Text, kInk);
        colours = 3;
    } else if (dimmed || vk == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        colours = 1;
    }
    if (ImGui::Button(text.c_str(), ImVec2(kKeyBox, 0)))
        simple_listening_ = listening ? 0 : me;
    ImGui::PopStyleColor(colours);

    // A click anywhere else gives up waiting.
    if (listening && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered())
        simple_listening_ = 0;

    std::string help = listening ? (can_clear ? "Nhấn phím muốn dùng. Esc để huỷ, Delete để bỏ phím này."
                                              : "Nhấn phím muốn dùng. Esc để huỷ.")
                                 : "Bấm vào ô rồi nhấn phím muốn dùng.";
    if (can_clear && !listening) help += "\nMuốn bỏ phím này: bấm vào ô rồi nhấn Delete.";
    if (tip) { help += "\n"; help += tip; }
    if (dimmed && vk != 0 && !listening) help += "\nĐang tắt.";
    ImGui::SetItemTooltip("%s", help.c_str());

    ImGui::PopID();
    return changed;
}

// ------------------------------------------------------------------ start

void GuiApp::simple_start_block() {
    const bool running = simple_running();
    const bool can_start = input_.window_valid();
    const std::string toggle = InputManager::key_name(macro_config_.toggle_key);

    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushFont(strong_font_, kButton);
    ImGui::PushStyleColor(ImGuiCol_Button, running ? kBad : kGood);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        running ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.45f, 0.90f, 0.52f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        running ? ImVec4(0.80f, 0.28f, 0.28f, 1.0f) : ImVec4(0.28f, 0.70f, 0.36f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    const std::string caption =
        (running ? "■   DỪNG LẠI   (" : "►   BẮT ĐẦU   (") + toggle + ")";
    ImGui::BeginDisabled(!running && !can_start);
    if (ImGui::Button(caption.c_str(), ImVec2(-1, 64))) simple_toggle();
    ImGui::EndDisabled();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
    if (!running && !can_start) ImGui::SetItemTooltip("Cần tìm thấy game trước.");

    {
        const std::string keys = toggle + ": bật / tắt     ·     " +
                                 InputManager::key_name(macro_config_.panic_key) +
                                 ": dừng ngay, thả hết phím";
        ImGui::PushFont(nullptr, kSmall);
        const float w = ImGui::CalcTextSize(keys.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(keys.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // ---- what it is doing ----
    ImGui::Dummy(ImVec2(0, 4));
    if (!begin_card("##state")) { end_card(); return; }

    const bool focused = input_.window_focused();
    if (!running) {
        if (!can_start)
            coloured(kWait, "Chưa chạy. Làm xong các mục đỏ ở trên trước.");
        else
            coloured(kDim, ("Chưa chạy. Vào game, rồi bấm BẮT ĐẦU hoặc phím " + toggle +
                            ".").c_str());
    } else if (macro_running_ && macro_->suspended()) {
        coloured(kWait, "Tạm dừng: nhân vật không ở trong game (mất kết nối, đang tải, hoặc "
                        "đã chết). Bot tự chạy lại khi nhân vật quay lại.");
    } else if (!focused) {
        coloured(kWait, "Đang chờ: hãy bấm vào cửa sổ game. Bot chỉ bấm phím khi game ở "
                        "phía trước.");
    } else if (hunting_ && hunter_) {
        switch (hunter_->status().state) {
            case Hunter::State::Attacking:   coloured(kGood, "Đang đánh quái."); break;
            case Hunter::State::Approaching: coloured(kGood, "Đang đi tới quái."); break;
            case Hunter::State::NoTargets:   coloured(kWait, "Không thấy quái nào quanh đây."); break;
            case Hunter::State::NoPlayer:
                coloured(kBad, "Không thấy nhân vật của bạn trên màn hình. Kiểm tra lại tên "
                               "nhân vật ở trên.");
                break;
            case Hunter::State::Unfocused:
                coloured(kWait, "Đang chờ: hãy bấm vào cửa sổ game.");
                break;
            case Hunter::State::Idle:        coloured(kDim, "Đang khởi động…"); break;
        }
    } else {
        coloured(kGood, "Đang chạy: tự bấm phím theo giờ và tự uống bình.");
    }

    // ---- what it has done ----
    if (simple_started_ms_ != 0) {
        const std::uint64_t until = running ? now_ms() : simple_stopped_ms_;
        const std::uint64_t elapsed = until > simple_started_ms_ ? until - simple_started_ms_ : 0;

        std::uint64_t hp = 0, mp = 0, buffs = 0;
        const auto ms = macro_->stats();
        for (std::size_t i = 0; i < macro_config_.keys.size() && i < ms.key_presses.size(); ++i) {
            const std::uint64_t base = i < simple_base_presses_.size() ? simple_base_presses_[i] : 0;
            const std::uint64_t n = ms.key_presses[i] > base ? ms.key_presses[i] - base : 0;
            switch (macro_key_kind(macro_config_.keys[i])) {
                case MacroKeyKind::HpPotion: hp += n; break;
                case MacroKeyKind::MpPotion: mp += n; break;
                case MacroKeyKind::Buff:     buffs += n; break;
                case MacroKeyKind::Skill:    break;
            }
        }
        const std::uint64_t finished = hunter_ ? hunter_->status().targets_finished : 0;
        const std::uint64_t kills =
            finished > simple_base_finished_ ? finished - simple_base_finished_ : 0;

        char line[256];
        std::snprintf(line, sizeof line,
                      "%s %s   ·   Hạ được %llu quái   ·   Bình HP: %llu   ·   Bình MP: %llu"
                      "   ·   Buff: %llu lần",
                      running ? "Đã chạy" : "Lần chạy vừa rồi:", clock_text(elapsed).c_str(),
                      static_cast<unsigned long long>(kills), static_cast<unsigned long long>(hp),
                      static_cast<unsigned long long>(mp), static_cast<unsigned long long>(buffs));
        hint(line);
    }

    // ---- the last thing that happened ----
    if (simple_listening_ != 0) {
        ImGui::PushFont(nullptr, kSmall);
        coloured(kWait, "Nhấn phím muốn dùng cho ô đang sáng màu. Esc để huỷ.");
        ImGui::PopFont();
    } else if (!simple_note_.empty() && now_ms() < simple_note_until_ms_) {
        hint(simple_note_.c_str());
    }

    end_card();
}

// -------------------------------------------------------- picking a window

void GuiApp::simple_window_list() {
    if (simple_pick_window_) {
        simple_pick_window_ = false;
        windows_ = InputManager::enumerate_windows();
        window_list_loaded_ = true;
        ImGui::OpenPopup("Chọn cửa sổ game");
    }

    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Chọn cửa sổ game", nullptr, ImGuiWindowFlags_NoSavedSettings))
        return;

    hint("Chọn cửa sổ của game trong danh sách. Bot chỉ gửi phím vào cửa sổ bạn chọn.");
    if (ImGui::Button("Làm mới danh sách")) windows_ = InputManager::enumerate_windows();
    ImGui::SameLine();
    if (ImGui::Button("Đóng")) ImGui::CloseCurrentPopup();

    if (ImGui::BeginChild("##windows", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (std::size_t i = 0; i < windows_.size(); ++i) {
            const auto& w = windows_[i];
            const std::string shown =
                (w.title.empty() ? "(" + w.window_class + ")" : w.title) + "   —   " + w.process;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(shown.c_str(), w.hwnd == input_.target_window())) {
                input_.set_target_window(w.hwnd);
                app_config_.window_title = w.title;
                app_config_.process_name = w.process;
                apply_vitals_config();
                simple_reader_tried_ = false;
                simple_note("Đã chọn: " + (w.title.empty() ? w.process : w.title));
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- running

void GuiApp::simple_start() {
    if (simple_running()) return;
    if (!input_.window_valid()) {
        simple_note("Chưa tìm thấy game. Mở game, vào nhân vật, rồi bấm Tìm lại.");
        return;
    }

    const bool any_key = any_key_on(macro_config_);
    if (!any_key && !hunt_config_.enabled) {
        simple_note("Chưa chọn việc nào. Đánh dấu ít nhất một việc ở phần \"Bot sẽ làm gì\".");
        return;
    }

    // The tallies start here, before anything can press a key, rather than
    // when the tick next notices the bot is running.
    simple_track_run(true);

    // The reader first. Every potion rule depends on it, and so does knowing
    // that the character has left the world and the keys should hold.
    if (!vitals_running_) start_vitals();
    apply_macro_config();
    if (any_key) start_macro();

    // The name-plate hunter, always. The packet bot needs your own position
    // and the server does not send one; the full screen's Attack switch makes
    // the same choice for the same reason.
    if (hunt_config_.enabled) {
        if (hunter_) hunter_->set_config(hunt_config_);
        start_hunt();
    }
    simple_note("Đã bắt đầu.");
}

void GuiApp::simple_stop() {
    stop_hunt();
    stop_macro();
    simple_track_run(false);
    simple_note("Đã dừng.");
}

void GuiApp::simple_track_run(bool running) {
    if (running == simple_was_running_) return;
    simple_was_running_ = running;
    if (running) {
        // Where the tallies start from, so the line under the button counts
        // this run rather than everything since GoBot was opened.
        simple_base_presses_ = macro_ ? macro_->stats().key_presses
                                      : std::vector<std::uint64_t>{};
        simple_base_finished_ = hunter_ ? hunter_->status().targets_finished : 0;
        simple_started_ms_ = now_ms();
    } else {
        simple_stopped_ms_ = now_ms();
    }
}

void GuiApp::simple_toggle() {
    if (simple_running()) simple_stop(); else simple_start();
}

void GuiApp::simple_changed() {
    apply_macro_config();
    if (hunter_) hunter_->set_config(hunt_config_);

    // Running: follow the switches. Ticking fighting on mid-run starts it,
    // ticking it off stops it, and a key switched on when the timers were not
    // running starts them.
    if (simple_running()) {
        if (hunt_config_.enabled && !hunting_) start_hunt();
        if (!hunt_config_.enabled && hunting_) stop_hunt();
        if (any_key_on(macro_config_) && !macro_running_) start_macro();
    }

    // No Save button on this screen: a setting a player has to remember to
    // save is a setting they lose.
    const bool ok = macro_config_.save(exe_relative("configs/macro.ini")) &&
                    hunt_config_.save(exe_relative("configs/hunt.ini"));
    if (!ok) simple_note("Không lưu được cài đặt vào thư mục configs.", 8000);
}

void GuiApp::simple_note(const std::string& msg, std::uint32_t ms) {
    simple_note_ = msg;
    simple_note_until_ms_ = now_ms() + ms;
}

} // namespace gobot
