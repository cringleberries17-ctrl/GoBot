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
#include "simple_ui.hpp"

#include "gobot/auto_login.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/session_manager.hpp"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
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

    // The tabs, fighting first: it is what most people open this for. The
    // others run beside it, and the start button under the tabs starts
    // whichever of them is switched on. A dot on a tab says it is running.
    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::BeginTabBar("##simpletabs")) {
        const auto tab = [](const char* name, bool running, const char* id) {
            std::string text = name;
            if (running) text += "  ●";
            text += "###";
            text += id;
            return ImGui::BeginTabItem(text.c_str());
        };
        if (tab("Đánh quái", macro_running_ || hunting_ || revive_running_, "fight")) {
            ImGui::Dummy(ImVec2(0, 4));
            simple_fight_tab();
            ImGui::EndTabItem();
        }
        if (tab("Tổ đội", party_running_, "party")) {
            ImGui::Dummy(ImVec2(0, 4));
            simple_party_tab();
            ImGui::EndTabItem();
        }
        if (tab("Tự chat", chat_running_ && !chat_oneshot_ && !chat_stop_, "chat")) {
            ImGui::Dummy(ImVec2(0, 4));
            simple_chat_tab();
            ImGui::EndTabItem();
        }
        if (tab("Tự đăng nhập", keeper_running_, "login")) {
            ImGui::Dummy(ImVec2(0, 4));
            simple_login_tab();
            ImGui::EndTabItem();
        }
        if (tab("Dọn rác", trash_running_, "trash")) {
            ImGui::Dummy(ImVec2(0, 4));
            simple_trash_tab();
            ImGui::EndTabItem();
        }
        if (tab("Nhiệm vụ", quest_running_ && !quest_oneshot_, "quest")) {
            ImGui::Dummy(ImVec2(0, 4));
            simple_quest_tab();
            ImGui::EndTabItem();
        }
        // Last: it watches the others rather than doing anything itself.
        if (tab("Thông báo", alerts_dot(), "alerts")) {
            ImGui::Dummy(ImVec2(0, 4));
            simple_alerts_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    simple_start_block();

    // The way to everything else, out of the way at the bottom -- and at the
    // other end of the same row the version, which a report should quote.
    ImGui::Dummy(ImVec2(0, 4));
    {
        const std::string version = package_version().empty()
                                        ? std::string("Phiên bản: bản tự build")
                                        : "Phiên bản " + package_version();
        ImGui::PushFont(nullptr, kSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(version.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();

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

void GuiApp::simple_fight_tab() {
    simple_checklist();
    simple_jobs();
}

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
    // it wrapped at, for whatever follows on the same row. `after` draws
    // something more beside each box, `after_w` wide.
    const auto key_row = [&](const std::vector<std::size_t>& which, const char* id,
                             bool show_interval,
                             const std::function<void(MacroKey&)>& after = nullptr,
                             float after_w = 0.0f) {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (std::size_t n = 0; n < which.size(); ++n) {
            MacroKey& k = keys[which[n]];
            if (n) beside(right, kKeyBox + after_w);
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
            if (after) after(k);
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
    ImGui::SameLine(0, 18);
    if (ImGui::Checkbox("Đứng yên một chỗ", &hunt_config_.stay_put)) simple_changed();
    ImGui::SetItemTooltip("Không đi đâu cả: chỉ đánh con quái nào tới trong tầm đánh.\n"
                          "Hợp với chỗ quái tự kéo tới.");
    hint(hunt_config_.stay_put ? "Đứng yên tại chỗ, đánh con quái nào tới trong tầm đánh."
                               : "Tự đi tới con quái gần nhất và đánh nó.");
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

    // ---- picking up ----
    //
    // Part of fighting -- the hunter presses it, after each kill -- but a row
    // of its own, because it is a key of its own and the first thing people
    // look for after "does it fight".
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool loot = hunt_config_.loot;
        if (ImGui::Checkbox("Tự nhặt đồ", &loot)) {
            hunt_config_.loot = loot;
            // Ticked with no key yet: ask for it straight away.
            if (loot && hunt_config_.loot_key == 0) {
                ImGui::PushID("loot");
                simple_listening_ = ImGui::GetID("##box");
                ImGui::PopID();
            }
            simple_changed();
        }
        ImGui::TableNextColumn();
        label("Phím nhặt");
        ImGui::SameLine();
        if (simple_key_box("loot", hunt_config_.loot_key, false, !hunt_config_.loot, nullptr)) {
            if (hunt_config_.loot_key != 0) hunt_config_.loot = true;
            simple_changed();
        }
        hint(hunt_config_.enabled
             ? "Bấm phím nhặt đồ sau mỗi con quái bị hạ, và thỉnh thoảng khi đang đánh."
             : "Nhặt đồ đi cùng Tự đánh quái, nên cần bật cả hai.");
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
        simple_potion_stock(kind);      // "Còn N bình", simple_potions.cpp
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
        // Each buff on its own clock, in seconds, as GoC has them: the
        // player's run 60 s and 1600 s, which no one interval suits. From
        // 60 s, where a key stops being a buff and would jump to the skills
        // row, to two hours.
        constexpr float kSecondsBox = 110.0f;
        const float seconds_w = 4.0f + kSecondsBox + 4.0f + ImGui::CalcTextSize("giây").x;
        const auto seconds_box = [&](MacroKey& k) {
            ImGui::SameLine(0, 4);
            int secs = static_cast<int>(k.interval_ms / 1000);
            ImGui::SetNextItemWidth(kSecondsBox);
            if (ImGui::InputInt("##seconds", &secs, 10, 60)) {
                secs = clamp_buff_seconds(secs);
                k.interval_ms = static_cast<std::uint32_t>(secs) * 1000u;
                simple_changed();
            }
            ImGui::SetItemTooltip("Bấm lại phím này sau mỗi %d giây (%d–%d).", secs,
                                  kBuffMinSeconds, kBuffMaxSeconds);
            ImGui::SameLine(0, 4);
            label("giây");
        };
        (void)key_row(buffs, "buff", false, seconds_box, seconds_w);
        ImGui::BeginDisabled(keys.size() >= MacroConfig::kMaxKeys);
        if (ImGui::Button("+##buff", ImVec2(ImGui::GetFrameHeight(), 0))) {
            simple_listen_key_ = keys.size();
            MacroKey k;
            k.label = "Buff " + std::to_string(buffs.size() + 1);
            k.interval_ms = 300000;
            k.jitter_pct = 5;
            k.start_delay_ms = static_cast<std::uint32_t>(keys.size()) * 150;
            keys.push_back(std::move(k));
            simple_changed();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Thêm một phím buff: bấm + rồi nhấn phím (bấm mỗi 300 giây).");
        hint(simple_ui::trial_hint(simple_ui::kTrialBuffs,
                                   "Mỗi phím buff có thời gian riêng (giây). Sau khi chết, hồi "
                                   "sinh hoặc vào lại game, GoBot buff lại tất cả.").c_str());
        row_gap();
    }

    // ---- dying ----
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::Checkbox("Tự hồi sinh", &simple_opts_.revive)) {
        save_simple_option("revive", simple_opts_.revive);
        if (simple_running()) {
            if (simple_opts_.revive) start_revive();
            else                     stop_revive();
        }
    }
    ImGui::TableNextColumn();
    hint(simple_ui::trial_hint(simple_ui::kTrialRevive,
                               "Khi nhân vật chết, bấm Sử dụng vật phẩm hồi sinh rồi Xác Nhận. Thử "
                               "tối đa 3 lần; không được thì dừng bot. Không hồi sinh tại làng, "
                               "không dùng Cash.").c_str());
    simple_popups_row();        // "Tự đóng thông báo", simple_popups.cpp

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
            // potion key would stop the bot (simple_hotkeys.cpp).
            if (const std::string clash = own_key_clash_vi(got, vk); !clash.empty()) {
                simple_note(clash);
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
    const bool have_game = input_.window_valid();
    // With the game closed BẮT ĐẦU still starts when "Tự vào lại game" is on:
    // the keeper opens the game and logs in first.
    const bool can_start = have_game || simple_opts_.keep_alive;
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
    if (!running && !can_start)
        ImGui::SetItemTooltip("Cần tìm thấy game trước, hoặc bật \"Tự vào lại game\" ở thẻ Tự "
                              "đăng nhập để GoBot tự mở game.");

    {
        const std::string keys = toggle + ": bật / tắt     ·     " +
                                 InputManager::key_name(macro_config_.panic_key) +
                                 ": dừng ngay, thả hết phím" + simple_hotkeys_line();
        ImGui::PushFont(nullptr, kSmall);
        const float w = ImGui::CalcTextSize(keys.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(keys.c_str());
        ImGui::PopStyleColor();

        bool refocus = simple_opts_.refocus;
        if (ImGui::Checkbox("Tự đưa game lên trước khi bạn không dùng máy 30 giây", &refocus)) {
            simple_opts_.refocus = refocus;
            save_simple_option("refocus", refocus);
        }
        ImGui::SetItemTooltip("Bot chỉ bấm phím được khi game ở phía trước. Nếu một cửa sổ khác "
                              "chiếm chỗ\n(thông báo, tin nhắn...) trong lúc bot chạy mà không ai "
                              "đụng chuột, bàn phím\n30 giây, GoBot đưa game lên lại.");
        ImGui::PopFont();
    }

    // ---- what it is doing ----
    ImGui::Dummy(ImVec2(0, 4));
    if (!begin_card("##state")) { end_card(); return; }

    const bool focused = input_.window_focused();
    std::string keeper_now, keeper_problem;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        keeper_now = keeper_vi_;
        keeper_problem = keeper_problem_vi_;
    }
    if (!running && !simple_stopped_vi_.empty()) {
        // The bot stopped itself -- the revive ran out of tries -- and says
        // why until the next BẮT ĐẦU.
        coloured(kBad, simple_stopped_vi_.c_str());
    } else if (!running) {
        if (!can_start)
            coloured(kWait, "Chưa chạy. Làm xong các mục đỏ ở trên trước.");
        else if (!have_game)
            coloured(kDim, ("Chưa chạy. Game đang tắt: bấm BẮT ĐẦU hoặc phím " + toggle +
                            " để GoBot mở game và vào nhân vật, rồi chạy các việc đã "
                            "chọn.").c_str());
        else
            coloured(kDim, ("Chưa chạy. Vào game, rồi bấm BẮT ĐẦU hoặc phím " + toggle +
                            ".").c_str());
    } else if (simple_cold_start_) {
        coloured(kWait, keeper_now.empty() ? "Đang mở game và vào nhân vật…" : keeper_now.c_str());
    } else if (macro_running_ && macro_->suspended()) {
        coloured(kWait, "Tạm dừng: nhân vật không ở trong game (mất kết nối, đang tải, hoặc "
                        "đã chết). Bot tự chạy lại khi nhân vật quay lại.");
    } else if (!focused && (macro_running_ || hunting_)) {
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
            case Hunter::State::Waiting:
                coloured(kGood, "Đứng yên, chờ quái tới trong tầm đánh.");
                break;
            case Hunter::State::Idle:        coloured(kDim, "Đang khởi động…"); break;
        }
    } else if (macro_running_) {
        coloured(kGood, "Đang chạy: tự bấm phím theo giờ và tự uống bình.");
    } else {
        coloured(kGood, "Đang chạy.");
    }

    // Fighting ticked and not running for want of a name: said in red where
    // the run is described, not only by the cross on the checklist above.
    if (running && hunt_config_.enabled && !hunting_ && !hunt_named())
        coloured(kBad, "Tự đánh quái chưa chạy: chưa gõ tên nhân vật ở phần \"Trước khi bắt "
                       "đầu\". Gõ tên rồi bấm ra ngoài ô, Tự đánh quái sẽ chạy.");

    // ---- the parts that run beside it ----
    if (party_running_)
        hint(("Tổ đội: đã nhận " + std::to_string(party_accepted_.load()) +
              " lời mời, đã mời " + std::to_string(party_invited_.load()) + " người.").c_str());
    // Not while a cold start is under way: the line above already says what
    // the keeper is doing, and the same words twice -- a held click's long
    // reason among them -- are just noise. Back once the game is in.
    if (keeper_running_ && !simple_cold_start_) {
        const int restarts = keeper_restarts_.load();
        const std::string now = keeper_now.empty() ? std::string("Đang theo dõi game.") : keeper_now;
        hint(("Tự đăng nhập: " + now +
              (restarts > 0 ? " Đã vào lại " + std::to_string(restarts) + " lần." : std::string()))
                 .c_str());
    } else if (!keeper_running_ && simple_opts_.keep_alive && !keeper_problem.empty()) {
        // Stopped for a person: why, where it will be seen.
        coloured(kBad, ("Tự đăng nhập đã dừng: " + keeper_problem).c_str());
    }
    if (revive_running_) {
        const int revived = revive_count_.load();
        hint(("Tự hồi sinh: đang theo dõi" +
              (revived > 0 ? ", đã hồi sinh " + std::to_string(revived) + " lần" : std::string()) +
              ".").c_str());
    }
    if (trash_running_ && !trash_oneshot_) {
        const int thrown = trash_thrown_.load();
        hint(("Dọn rác: đang bật" +
              (thrown > 0 ? ", đã bỏ " + std::to_string(thrown) + " món" : std::string()) +
              ".").c_str());
    }
    if (chat_running_ && !chat_oneshot_ && !chat_stop_)
        hint(("Tự chat: đang bật, đã gửi " + std::to_string(chat_sent_.load()) + " câu.").c_str());
    if (quest_running_ && !quest_oneshot_) hint("Làm nhiệm vụ nhanh: đang bật.");
    if (popups_running()) hint(simple_popups_line().c_str());
    // Said here too, by the button: it is why the fighting has stopped.
    if (chat_doubt_)
        coloured(kWait, "Ô chat trong game có thể đang mở: đánh quái, bình và buff chờ đến khi ô chat "
                        "đóng (xem thẻ Tự chat).");

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
    simple_stopped_vi_.clear();     // what stopped the last run is old news

    const bool any_key = any_key_on(macro_config_);
    Party::Settings party_rules;
    const bool party = party_rules.load(exe_relative("configs/party.ini")) &&
                       (party_rules.auto_accept || party_rules.auto_invite);
    CleanSettings clean;
    const bool trash = clean.load(exe_relative("configs/trash.ini")) && clean.enabled;
    ChatSettings chat_rules;
    const bool chat = chat_rules.load(exe_relative("configs/chat.ini")) && chat_rules.enabled;
    QuestSettings quest_rules;
    const bool quest = quest_rules.load(exe_relative("configs/quest.ini")) && quest_rules.enabled;
    // Fighting ticked with no name typed is no job: the hunter would not
    // start (start_hunt says why), so it is not counted as one here either.
    const bool hunt = hunt_config_.enabled && hunt_named();
    if (!any_key && !hunt && !party && !simple_opts_.keep_alive &&
        !simple_opts_.revive && !trash && !chat && !quest) {
        simple_note(hunt_config_.enabled
                        ? "Chưa gõ tên nhân vật nên chưa bật được Tự đánh quái. Gõ tên ở phần "
                          "\"Trước khi bắt đầu\"."
                        : "Chưa chọn việc nào. Đánh dấu ít nhất một việc ở phần \"Bot sẽ làm gì\".");
        return;
    }

    // Opened since GoBot last looked? Then there is nothing to open.
    if (!input_.window_valid() &&
        input_.find_game_window(app_config_.window_title, app_config_.window_class,
                                app_config_.process_name)) {
        apply_vitals_config();
        simple_reader_tried_ = false;
    }

    if (!input_.window_valid()) {
        // The game is closed. With "Tự vào lại game" on, the keeper opens it
        // and logs in -- GoC goes from a missing window to the world the same
        // way -- and the rest starts once the character is in, from tick().
        // Nothing else starts before: there is no game to press keys into.
        if (!simple_opts_.keep_alive) {
            simple_note("Chưa tìm thấy game. Mở game, vào nhân vật, rồi bấm Tìm lại — hoặc bật "
                        "\"Tự vào lại game\" ở thẻ Tự đăng nhập để GoBot tự mở game.");
            return;
        }
        if (!elevated_) {
            simple_note("GoBot không chạy bằng quyền Administrator nên không bấm được vào "
                        "VTCZone/game.", 10000);
            return;
        }
        // Running, only its window not found: there is no game to open, and
        // the keeper would watch it run and never log in, so nothing else
        // would ever start.
        IniFile app_ini;
        app_ini.load(exe_relative("configs/app.ini"));
        CrashWatch::Settings crash;
        crash.load(app_ini);
        if (SessionManager::process_running(crash.game_process)) {
            simple_note("Game đang chạy nhưng GoBot chưa tìm thấy cửa sổ của nó. Bấm Tìm lại "
                        "hoặc \"Chọn cửa sổ…\" ở thẻ Đánh quái.", 10000);
            return;
        }
        // Told to the keeper before it starts, so its very first look knows:
        // a game that comes up while it loads its pictures is followed, and
        // what was ticked starts on it.
        keeper_cold_start_ = true;
        start_keeper();
        simple_cold_start_ = true;
        // Said at once, rather than "watching" until the keeper has loaded
        // its pictures and looked for the first time.
        say_keeper_vi("Đang mở game và vào nhân vật…");
        simple_note("Đang mở game và vào nhân vật…", 15000);
        return;
    }
    simple_start_jobs();
}

void GuiApp::simple_start_jobs() {
    const bool any_key = any_key_on(macro_config_);
    Party::Settings party_rules;
    const bool party = party_rules.load(exe_relative("configs/party.ini")) &&
                       (party_rules.auto_accept || party_rules.auto_invite);
    CleanSettings clean;
    const bool trash = clean.load(exe_relative("configs/trash.ini")) && clean.enabled;
    // Read again here rather than handed over from simple_start: after a cold
    // start this runs minutes later, and the tab may have changed it since.
    ChatSettings chat_rules;
    const bool chat = chat_rules.load(exe_relative("configs/chat.ini")) && chat_rules.enabled;
    QuestSettings quest_rules;
    const bool quest = quest_rules.load(exe_relative("configs/quest.ini")) && quest_rules.enabled;

    // Nothing at all starts while a stop may have left the bin's dialog, or
    // the game's chat line, open: the macro and the hunter would press keys
    // into it, and the rest would start without them.
    if (!keys_may_start()) return;

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
    // the same choice for the same reason. Not without your character's
    // name, which is how it finds you: the rest starts, and the card says
    // in red why the fighting did not.
    const bool unnamed = hunt_config_.enabled && !hunt_named();
    if (hunt_config_.enabled && !unnamed) {
        if (hunter_) hunter_->set_config(hunt_config_);
        start_hunt();
    }

    // What runs beside the fighting, each when it is switched on.
    if (party) start_party();
    if (simple_opts_.keep_alive) start_keeper();
    if (simple_opts_.revive) start_revive();
    if (trash) start_trash();
    if (chat) start_chat();
    if (quest) start_quest();
    start_popups();             // when "Tự đóng thông báo" is ticked
    if (unnamed)
        simple_note("Đã bắt đầu, nhưng chưa bật Tự đánh quái: chưa gõ tên nhân vật.", 12000);
    else
        simple_note("Đã bắt đầu.");
}

void GuiApp::simple_stop() {
    simple_cold_start_ = false;     // nothing is started after a stop
    // The chat first: a line half typed is rubbed out and closed as it stops,
    // under its hold, which keeps everything else off until it is done. Not
    // waited for here: that takes a few seconds for a long line, and the
    // window would freeze meanwhile. The player has taken over: a chat line
    // still in doubt after that is handed over to tick(), not waited on.
    stop_chat(true, false);
    stop_hunt();
    stop_macro();
    stop_party();
    stop_revive();
    stop_quest();
    stop_popups();
    // Not said over the word that the bin's dialog may still be open.
    const bool bin_note = stop_trash();
    stop_keeper();
    simple_track_run(false);
    if (!bin_note) simple_note("Đã dừng.");
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

// ---------------------------------------------------------------- options

void GuiApp::load_simple_options() {
    IniFile ini;
    if (!ini.load(exe_relative("configs/app.ini"))) return;
    simple_opts_.keep_alive = ini.get_bool("Simple", "keep_alive", simple_opts_.keep_alive);
    simple_opts_.revive     = ini.get_bool("Simple", "revive", simple_opts_.revive);
    simple_opts_.refocus    = ini.get_bool("Simple", "refocus", simple_opts_.refocus);
}

void GuiApp::save_simple_option(const char* key, bool value) {
    if (!IniFile::set_in_place(exe_relative("configs/app.ini"), "Simple", key,
                               value ? "true" : "false"))
        simple_note("Không lưu được cài đặt vào configs/app.ini.", 8000);
}

void GuiApp::refocus_if_idle() {
    if (!simple_opts_.refocus) return;
    if (!macro_running_ && !hunting_) return;     // only the keys need the game in front
    if (!input_.window_valid() || input_.window_focused()) return;

    // One try every so often, not a tug of war with whatever took the focus.
    const std::uint64_t now = now_ms();
    if (now - last_refocus_ms_ < 15000) return;

    LASTINPUTINFO last{};
    last.cbSize = sizeof last;
    if (!GetLastInputInfo(&last)) return;
    const DWORD idle_ms = GetTickCount() - last.dwTime;
    if (idle_ms < 30000) return;

    last_refocus_ms_ = now;
    if (input_.focus_window())
        GB_INFO("Brought the game back to the front: it lost focus during a run and the "
                "machine has been idle %lus", static_cast<unsigned long>(idle_ms / 1000));
}

// ------------------------------------------------------------------ party

namespace {

/// Names as one line for the file, from a box with one name per line. Commas
/// are taken too: that is how the file writes them, and how people paste them.
std::string names_line(const char* text) {
    std::string out, name;
    const auto flush = [&] {
        const auto b = name.find_first_not_of(" \t");
        const auto e = name.find_last_not_of(" \t");
        if (b != std::string::npos) {
            if (!out.empty()) out += ',';
            out += name.substr(b, e - b + 1);
        }
        name.clear();
    };
    for (const char* p = text; *p; ++p) {
        if (*p == '\n' || *p == '\r' || *p == ',') flush();
        else name += *p;
    }
    flush();
    return out;
}

/// The other way: one name per line, for the box.
void names_box(char* out, std::size_t size, const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& n : names) {
        if (!text.empty()) text += '\n';
        text += n;
    }
    std::snprintf(out, size, "%s", text.c_str());
}

/// The last few lines a worker wrote, small and grey.
void last_lines(const std::vector<std::string>& lines, std::size_t n) {
    for (std::size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); ++i)
        hint(lines[i].c_str());
}

} // namespace

void GuiApp::simple_party_tab() {
    const std::string path = exe_relative("configs/party.ini");
    if (!simple_party_loaded_) {
        simple_party_loaded_ = true;
        if (!simple_party_.load(path))
            simple_note("Không đọc được configs/party.ini.", 8000);
        names_box(simple_accept_list_, sizeof simple_accept_list_, simple_party_.accept_only);
        names_box(simple_invite_list_, sizeof simple_invite_list_, simple_party_.whitelist);
        names_box(simple_black_list_, sizeof simple_black_list_, simple_party_.blacklist);
        std::snprintf(simple_keyword_, sizeof simple_keyword_, "%s",
                      simple_party_.magic_word.c_str());
    }

    // Written key by key. Below these switches the file carries the measured
    // geometry of the party dialogs and the reasoning for every number, which
    // a rewrite of the whole file would throw away.
    const auto put = [&](const char* key, const std::string& value) {
        if (!IniFile::set_in_place(path, "Party", key, value))
            simple_note("Không lưu được configs/party.ini.", 8000);
    };
    const auto put_bool = [&](const char* key, bool value) { put(key, value ? "true" : "false"); };

    // Switched on mid-run: start it. The worker reads the file again every
    // couple of seconds, so everything else follows by itself.
    const auto follow = [&] {
        if (simple_running() && !party_running_ &&
            (simple_party_.auto_accept || simple_party_.auto_invite))
            start_party();
    };

    if (!begin_card("##party")) { end_card(); return; }
    heading(strong_font_, "Tổ đội");
    hint("Chạy khi bấm BẮT ĐẦU, cùng với đánh quái hoặc một mình.");
    ImGui::Dummy(ImVec2(0, 4));

    // ---- answering ----
    if (ImGui::Checkbox("Tự nhận lời mời vào tổ đội", &simple_party_.auto_accept)) {
        put_bool("party_auto_accept", simple_party_.auto_accept);
        follow();
    }
    ImGui::Indent(mark_indent());
    ImGui::BeginDisabled(!simple_party_.auto_accept);
    if (ImGui::RadioButton("Nhận từ mọi người (trừ danh sách đen)", simple_party_.accept_all)) {
        simple_party_.accept_all = true;
        put_bool("party_accept_all", true);
    }
    if (ImGui::RadioButton("Chỉ nhận từ những người này:", !simple_party_.accept_all)) {
        simple_party_.accept_all = false;
        put_bool("party_accept_all", false);
    }
    if (!simple_party_.accept_all) {
        ImGui::InputTextMultiline("##acceptonly", simple_accept_list_, sizeof simple_accept_list_,
                                  ImVec2(-1, 84));
        if (ImGui::IsItemDeactivatedAfterEdit())
            put("party_accept_only", names_line(simple_accept_list_));
        hint("Mỗi dòng một tên.");
    }
    ImGui::EndDisabled();
    ImGui::Unindent(mark_indent());
    ImGui::Dummy(ImVec2(0, 6));

    // ---- inviting ----
    if (ImGui::Checkbox("Tự mời người chat từ khoá", &simple_party_.auto_invite)) {
        put_bool("party_auto_party", simple_party_.auto_invite);
        follow();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("##keyword", simple_keyword_, sizeof simple_keyword_);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        simple_party_.magic_word = simple_keyword_;
        put("party_magic_word", simple_party_.magic_word);
        if (!IniFile::set_in_place(path, "Chat", "keyword", simple_party_.magic_word))
            simple_note("Không lưu được configs/party.ini.", 8000);
    }
    ImGui::Indent(mark_indent());
    ImGui::BeginDisabled(!simple_party_.auto_invite);
    hint(simple_ui::trial_hint(simple_ui::kTrialInvite,
                               "Ai chat đúng từ này, đứng riêng chứ không dính vào chữ khác, sẽ "
                               "được mời vào tổ đội.").c_str());
    simple_party_leader_option();       // "Chỉ mời khi đang làm chủ tổ đội" (simple_party.cpp)
    if (ImGui::Checkbox("Chỉ mời người có tên trong danh sách:", &simple_party_.by_list))
        put_bool("party_by_list", simple_party_.by_list);
    if (simple_party_.by_list) {
        ImGui::InputTextMultiline("##invitelist", simple_invite_list_, sizeof simple_invite_list_,
                                  ImVec2(-1, 84));
        if (ImGui::IsItemDeactivatedAfterEdit())
            put("party_whitelist", names_line(simple_invite_list_));
        hint("Mỗi dòng một tên.");
    }
    ImGui::EndDisabled();
    ImGui::Unindent(mark_indent());
    ImGui::Dummy(ImVec2(0, 6));

    // ---- never ----
    if (ImGui::Checkbox("Dùng danh sách đen", &simple_party_.use_blacklist))
        put_bool("use_blacklist", simple_party_.use_blacklist);
    ImGui::Indent(mark_indent());
    hint("Không bao giờ nhận lời mời của những người này, và không mời họ.");
    if (simple_party_.use_blacklist) {
        ImGui::InputTextMultiline("##blacklist", simple_black_list_, sizeof simple_black_list_,
                                  ImVec2(-1, 84));
        if (ImGui::IsItemDeactivatedAfterEdit())
            put("party_blacklist", names_line(simple_black_list_));
        hint("Mỗi dòng một tên.");
    }
    ImGui::Unindent(mark_indent());
    end_card();

    // ---- what it is doing ----
    if (!begin_card("##partystate")) { end_card(); return; }
    if (party_running_)
        coloured(kGood, ("Đang chạy: đã nhận " + std::to_string(party_accepted_.load()) +
                         " lời mời, đã mời " + std::to_string(party_invited_.load()) +
                         " người.").c_str());
    else if (simple_party_.auto_accept || simple_party_.auto_invite)
        coloured(kDim, "Sẽ chạy khi bấm BẮT ĐẦU.");
    else
        coloured(kDim, "Đang tắt.");
    simple_party_hud_line();            // "Tổ đội: …" and its "Thử xem" (simple_party.cpp)
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        lines = party_log_;
    }
    last_lines(lines, 4);
    end_card();
}

// ------------------------------------------------------------------ trash

void GuiApp::simple_rematch_bag() {
    simple_bag_match_.clear();
    if (!simple_bag_shot_) return;
    const BagLayout& l = simple_clean_.layout;
    for (const ScreenRect& s : simple_bag_view_.slots(l))
        simple_bag_match_.push_back(
            bag::is_empty(simple_bag_frame_, s, l)
                ? -1
                : simple_trash_.match(simple_bag_frame_, s, simple_clean_.min_score));
}

void GuiApp::simple_reload_trash() {
    // The same list a pass goes by, rivals and all, so the red outlines on the
    // photographed bag are what a pass would throw away.
    simple_trash_.build(exe_relative("configs/trash"), simple_clean_, simple_catalog_);
    simple_rematch_bag();
}

void GuiApp::simple_save_ids() {
    const auto join = [](const std::vector<std::string>& ids) {
        std::string out;
        for (const std::string& id : ids) {
            if (!out.empty()) out += ',';
            out += id;
        }
        return out;
    };
    const std::string cfg = exe_relative("configs/trash.ini");
    if (!IniFile::set_in_place(cfg, "Items", "delete", join(simple_clean_.delete_ids)) ||
        !IniFile::set_in_place(cfg, "Items", "drop", join(simple_clean_.drop_ids)) ||
        !IniFile::set_in_place(cfg, "Items", "use", join(simple_clean_.use_ids)))
        simple_note("Không lưu được configs/trash.ini.", 8000);
}

void GuiApp::simple_trash_tab() {
    const std::string cfg = exe_relative("configs/trash.ini");
    const std::string dir = exe_relative("configs/trash");
    if (!simple_catalog_loaded_) {
        simple_catalog_loaded_ = true;
        simple_catalog_.load(ItemCatalog::default_dir());
    }
    if (!simple_clean_loaded_) {
        simple_clean_loaded_ = true;
        if (!simple_clean_.load(cfg)) simple_note("Không đọc được configs/trash.ini.", 8000);
        simple_reload_trash();
    }
    const auto put = [&](const char* key, const std::string& value) {
        if (!IniFile::set_in_place(cfg, "Trash", key, value))
            simple_note("Không lưu được configs/trash.ini.", 8000);
    };
    const BagLayout& layout = simple_clean_.layout;
    const ImU32 kDeleteMark = ImGui::GetColorU32(kBad);
    const ImU32 kDropMark = ImGui::GetColorU32(kWait);
    const ImU32 kUseMark = ImGui::GetColorU32(kGood);       // Dùng: registered (simple_trash_use.cpp)

    // ---- when ----
    if (!begin_card("##trash")) { end_card(); return; }
    heading(strong_font_, "Dọn rác");
    if (ImGui::Checkbox("Tự dọn rác", &simple_clean_.enabled)) {
        put("enabled", simple_clean_.enabled ? "true" : "false");
        if (simple_clean_.enabled && simple_running()) start_trash();
        if (!simple_clean_.enabled && trash_running_ && !trash_oneshot_) stop_trash();
    }
    ImGui::SameLine(0, 16);
    label("mỗi");
    ImGui::SameLine();
    {
        static const int kMinutes[] = { 5, 10, 15, 30, 60 };
        const std::string shown = std::to_string(simple_clean_.interval_minutes) + " phút";
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##every", shown.c_str())) {
            for (const int m : kMinutes) {
                const std::string item = std::to_string(m) + " phút";
                if (ImGui::Selectable(item.c_str(), m == simple_clean_.interval_minutes)) {
                    simple_clean_.interval_minutes = m;
                    put("interval_minutes", std::to_string(m));
                }
            }
            ImGui::EndCombo();
        }
    }
    hint("Chọn món cần bỏ một lần ở dưới. Khi bot chạy, GoBot tự mở túi đồ, bỏ những món "
         "đó rồi đóng túi lại: lần đầu ngay sau khi bấm BẮT ĐẦU, sau đó theo giờ ở trên. "
         "Đánh quái, bình và buff tạm dừng trong lúc dọn.");

    ImGui::Dummy(ImVec2(0, 4));
    label("Phím mở túi đồ");
    ImGui::SameLine();
    if (simple_key_box("bagkey", simple_clean_.bag_key, false, false, nullptr))
        put("bag_key", std::to_string(simple_clean_.bag_key));
    ImGui::SameLine(0, 18);
    label("Tìm trong:");
    static const char* const kTabKeys[kBagTabs] = { "tab_equip1", "tab_equip2", "tab_use",
                                                    "tab_other", "tab_pet" };
    for (int t = 0; t < kBagTabs; ++t) {
        ImGui::SameLine();
        ImGui::PushID(t);
        if (ImGui::Checkbox(bag_tab_name(t), &simple_clean_.tabs[t]))
            put(kTabKeys[t], simple_clean_.tabs[t] ? "true" : "false");
        ImGui::PopID();
    }
    end_card();

    // ---- what, chosen beforehand ----
    if (!begin_card("##trashcatalog")) { end_card(); return; }
    {
        int chosen = 0;
        for (const auto& item : simple_trash_.items) if (!item.id.empty()) ++chosen;
        heading(strong_font_, ("Chọn đồ cần bỏ (" + std::to_string(chosen) + " món)").c_str());
    }
    if (simple_catalog_.groups.empty()) {
        hint("Không tìm thấy hình các món đồ của game.");
    } else {
        hint("Bấm vào món muốn bỏ. Viền đỏ: bỏ vào thùng rác, mất hẳn. Viền vàng: vứt xuống "
             "đất. Viền xanh: dùng (đăng ký mảnh ghép). Không cần món đó đang có trong túi. Món "
             "được nhận theo hình vẽ, không theo màu: mảnh ghép trong túi đổi màu liên tục, và "
             "món cùng hình khác màu (viền đỏ mảnh) cũng bị xoá theo.");
        for (const auto& group : simple_catalog_.groups) {
            ImGui::PushFont(nullptr, kSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(group.name);
            ImGui::PopStyleColor();
            ImGui::PopFont();

            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            for (std::size_t i = 0; i < group.entries.size(); ++i) {
                const auto& e = group.entries[i];
                if (i > 0) {
                    const float next = ImGui::GetItemRectMax().x + 4 + 34;
                    if (next <= right) ImGui::SameLine(0, 4);
                }
                ImGui::PushID(e.id.c_str());
                draw_frame_region(e.tile, 0, 0, e.tile.width, e.tile.height, 1.0f, "##tile");
                const bool del = std::find(simple_clean_.delete_ids.begin(),
                                           simple_clean_.delete_ids.end(), e.id) !=
                                 simple_clean_.delete_ids.end();
                const bool drop = std::find(simple_clean_.drop_ids.begin(),
                                            simple_clean_.drop_ids.end(), e.id) !=
                                  simple_clean_.drop_ids.end();
                const bool use = std::find(simple_clean_.use_ids.begin(),
                                           simple_clean_.use_ids.end(), e.id) !=
                                 simple_clean_.use_ids.end();
                // On the list without being ticked: drawn like a ticked one.
                std::string via;
                for (const auto& item : simple_trash_.items)
                    if (item.id == e.id && !item.via.empty() &&
                        item.action == TrashList::Action::Delete)
                        via = item.via;
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                if (use) {
                    // Green: registered; with the red inside it, the rest deleted after.
                    ImGui::GetWindowDrawList()->AddRect(a, b, kUseMark, 0, 0, 3.0f);
                    if (del)
                        ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x + 3, a.y + 3), ImVec2(b.x - 3, b.y - 3),
                                                            kDeleteMark, 0, 0, 2.0f);
                }
                else if (del)  ImGui::GetWindowDrawList()->AddRect(a, b, kDeleteMark, 0, 0, 3.0f);
                else if (drop) ImGui::GetWindowDrawList()->AddRect(a, b, kDropMark, 0, 0, 3.0f);
                else if (!via.empty())
                    ImGui::GetWindowDrawList()->AddRect(a, b, kDeleteMark, 0, 0, 1.0f);
                else if (ImGui::IsItemHovered())
                    ImGui::GetWindowDrawList()->AddRect(a, b, ImGui::GetColorU32(kDim), 0, 0, 1.0f);
                if (!del && !drop && !use && !via.empty())
                    ImGui::SetItemTooltip("Cũng sẽ bị xoá: cùng hình với %s, khác màu. Bấm để xem.",
                                          via.c_str());
                else
                    ImGui::SetItemTooltip("%s", use && del ? "Sẽ đăng ký vào Yêu Quái Bách Khoa, rồi bỏ phần còn "
                                                             "lại vào thùng rác. Bấm để đổi."
                                              : use  ? "Sẽ đăng ký vào Yêu Quái Bách Khoa. Bấm để đổi."
                                              : del  ? "Sẽ bỏ vào thùng rác. Bấm để đổi."
                                              : drop ? "Sẽ vứt xuống đất. Bấm để đổi."
                                                     : "Bấm để chọn bỏ món này.");
                if (ImGui::IsItemClicked()) {
                    simple_pick_id_ = e.id;
                    // Worked out once, when the menu opens: the menu shows
                    // them before anything is ticked.
                    simple_pick_kin_ = simple_catalog_.same_drawing(e.id);
                    ImGui::OpenPopup("##catpick");
                }
                if (ImGui::BeginPopup("##catpick")) {
                    // The same drawing in other colours goes with it, by the
                    // player's rule: shown here, before anything is ticked.
                    std::string kin;
                    for (const std::string& id : simple_pick_kin_)
                        kin += (kin.empty() ? "" : ", ") + id;
                    // A popup sizes itself to what is in it, so the lines
                    // are given a width to wrap at.
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 330.0f);
                    if (!del && !drop && !via.empty()) {
                        ImGui::PushFont(nullptr, kSmall);
                        coloured(kWait, ("Món này cũng bị xoá, vì cùng hình với " + via +
                                         ". Muốn giữ nó thì bỏ chọn món đó.").c_str());
                        ImGui::PopFont();
                        ImGui::Separator();
                    }
                    if (!simple_pick_kin_.empty()) {
                        ImGui::PushFont(nullptr, kSmall);
                        coloured(kWait, "Chọn món này thì cũng xoá luôn các món cùng hình, "
                                        "khác màu:");
                        ImGui::PopFont();
                        for (std::size_t k = 0; k < simple_pick_kin_.size(); ++k) {
                            const ItemCatalog::Entry* same = simple_catalog_.find(simple_pick_kin_[k]);
                            if (same == nullptr) continue;
                            if (k > 0) ImGui::SameLine(0, 4);
                            ImGui::PushID(static_cast<int>(k));
                            draw_frame_region(same->tile, 0, 0, same->tile.width,
                                              same->tile.height, 1.0f, "##kin");
                            ImGui::SetItemTooltip("%s", same->id.c_str());
                            ImGui::PopID();
                        }
                        ImGui::Separator();
                    }
                    const auto set = [&](int how) {     // 0 keep, 1 delete, 2 drop
                        auto& d = simple_clean_.delete_ids;
                        auto& p = simple_clean_.drop_ids;
                        auto& u = simple_clean_.use_ids;
                        d.erase(std::remove(d.begin(), d.end(), simple_pick_id_), d.end());
                        p.erase(std::remove(p.begin(), p.end(), simple_pick_id_), p.end());
                        u.erase(std::remove(u.begin(), u.end(), simple_pick_id_), u.end());
                        if (how == 1) d.push_back(simple_pick_id_);
                        if (how == 2) p.push_back(simple_pick_id_);
                        simple_save_ids();
                        simple_reload_trash();
                        if (how == 1 && !kin.empty())
                            simple_note("Cũng sẽ xoá: " + kin + " (cùng hình, khác màu).", 12000);
                        ImGui::CloseCurrentPopup();
                    };
                    if (ImGui::Selectable("Bỏ vào thùng rác (xoá hẳn)", del && !use)) set(1);
                    // Not offered: GoBot has no picture of the drop question
                    // with its words, and answers no dialog it cannot tell
                    // from the others.
                    ImGui::Selectable("Vứt xuống đất (chưa dùng được)", drop,
                                      ImGuiSelectableFlags_Disabled);
                    simple_use_options(e.id, group.folder.c_str());     // fragments (simple_trash_use.cpp)
                    ImGui::Separator();
                    if (ImGui::Selectable("Không bỏ món này", !del && !drop && !use)) set(0);
                    ImGui::PopTextWrapPos();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0, 2));
        }
    }
    ImGui::PushFont(nullptr, kSmall);
    coloured(kWait, "Xoá là mất hẳn, không lấy lại được. Bấm Thử tìm ở dưới trước khi bật.");
    ImGui::PopFont();
    end_card();

    // ---- what, photographed from the bag ----
    if (!begin_card("##trashpick")) { end_card(); return; }
    {
        int cut = 0;
        for (const auto& item : simple_trash_.items) if (!item.file.empty()) ++cut;
        heading(strong_font_, ("Món khác, chụp từ túi đồ (" + std::to_string(cut) + ")").c_str());
    }
    hint("Cho món không có ở trên, như đồ trang bị. Mở túi đồ trong game ở ngăn có món đó, "
         "để túi đồ không bị GoBot che, bấm Chụp túi đồ, rồi bấm vào món.");

    // What was cut from the bag before, each with its way off the list.
    {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        int remove = -1;
        bool first = true;
        for (std::size_t i = 0; i < simple_trash_.items.size(); ++i) {
            const auto& item = simple_trash_.items[i];
            if (item.file.empty()) continue;
            if (!first) {
                const float next = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + 60;
                if (next <= right) ImGui::SameLine();
            }
            first = false;
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginGroup();
            draw_frame_region(item.shown, 0, 0, item.shown.width, item.shown.height, 1.5f,
                              "##thumb");
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const bool used = item.action == TrashList::Action::Use;
            ImGui::GetWindowDrawList()->AddRect(
                a, b, used ? kUseMark : item.action == TrashList::Action::Delete ? kDeleteMark : kDropMark,
                0, 0, 2.0f);
            ImGui::PushFont(nullptr, kSmall);
            ImGui::TextUnformatted(used ? "Dùng" : item.action == TrashList::Action::Delete ? "Xoá" : "Vứt");
            ImGui::SameLine();
            if (ImGui::SmallButton("×")) remove = static_cast<int>(i);
            ImGui::PopFont();
            ImGui::SetItemTooltip("Bỏ món này khỏi danh sách");
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (remove >= 0) {
            // Read back whole, since the catalog icons a picture stood for are
            // rivals again once it is gone.
            simple_trash_.remove(static_cast<std::size_t>(remove));
            simple_reload_trash();
        }
    }

    if (input_.window_valid() && overlap_fraction() > 0.02f) {
        mark(Mark::Bad);
        ImGui::SameLine();
        label("Cửa sổ GoBot đang che game");
        ImGui::SameLine();
        if (ImGui::Button("Dời GoBot sang bên##trash")) move_clear_of_game();
    }
    ImGui::BeginDisabled(!input_.window_valid());
    if (ImGui::Button("Chụp túi đồ")) {
        if (!simple_store_loaded_) {
            simple_store_loaded_ = true;
            simple_store_.load_best(exe_relative("configs/templates.ini"));
        }
        ScreenCapture capture;
        Frame frame;
        if (!capture.grab(input_.target_window(), frame) || !frame.valid()) {
            simple_note("Không chụp được màn hình game.", 7000);
        } else {
            const BagView view = bag::find(frame, simple_store_, simple_clean_.layout);
            if (!view.found) {
                simple_note("Không thấy túi đồ trên màn hình. Mở túi đồ trong game (phím " +
                            InputManager::key_name(simple_clean_.bag_key) +
                            "), để nó không bị che, rồi bấm lại.", 9000);
            } else {
                simple_bag_frame_ = std::move(frame);
                simple_bag_view_ = view;
                simple_bag_shot_ = true;
                simple_rematch_bag();
                simple_note(view.bin_empty
                    ? "Đã chụp túi đồ. Bấm vào món muốn bỏ."
                    : "Đã chụp túi đồ, nhưng không thấy thùng rác. Thử lại khi túi đồ hiện rõ.",
                    7000);
            }
        }
    }
    ImGui::EndDisabled();

    if (simple_bag_shot_) {
        const auto slots = simple_bag_view_.slots(layout);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const ScreenRect& s = slots[i];
            if (i % static_cast<std::size_t>(layout.cols) != 0) ImGui::SameLine(0, 4);
            ImGui::PushID(static_cast<int>(i));
            draw_frame_region(simple_bag_frame_, s.x, s.y, s.w, s.h, 1.5f, "##slot");
            const bool empty = bag::is_empty(simple_bag_frame_, s, layout);
            const int listed = i < simple_bag_match_.size() ? simple_bag_match_[i] : -1;
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (listed >= 0)
                ImGui::GetWindowDrawList()->AddRect(
                    a, b,
                    simple_trash_.items[static_cast<std::size_t>(listed)].action == TrashList::Action::Use
                        ? kUseMark : kDeleteMark,
                    0, 0, 3.0f);
            else if (!empty && ImGui::IsItemHovered())
                ImGui::GetWindowDrawList()->AddRect(a, b, kDropMark, 0, 0, 2.0f);
            if (!empty && listed < 0 && ImGui::IsItemClicked()) {
                simple_pick_slot_ = static_cast<int>(i);
                ImGui::OpenPopup("##pickitem");
            }
            if (!empty)
                ImGui::SetItemTooltip("%s", listed >= 0 ? "Đã có trong danh sách"
                                                        : "Bấm để thêm vào danh sách");
            if (ImGui::BeginPopup("##pickitem")) {
                const auto add = [&](TrashList::Action action) {
                    const ScreenRect& at = slots[static_cast<std::size_t>(simple_pick_slot_)];
                    const std::string file =
                        simple_trash_.add(dir, bag::cut(simple_bag_frame_, at), action, layout);
                    simple_reload_trash();
                    // Matched by its drawing, so the catalog's icons drawn
                    // like it -- the gold pieces for a red one -- go too.
                    std::string kin;
                    for (const auto& item : simple_trash_.items)
                        if (!file.empty() && item.file == file)
                            for (const std::string& id : item.kin)
                                kin += (kin.empty() ? "" : ", ") + id;
                    simple_note(file.empty() ? std::string("Không lưu được hình món đồ.")
                                : kin.empty() ? std::string("Đã thêm vào danh sách đồ bỏ đi.")
                                              : "Đã thêm vào danh sách đồ bỏ đi. Cũng sẽ xoá: " +
                                                    kin + " (cùng hình, khác màu).",
                                kin.empty() ? 6000 : 12000);
                    ImGui::CloseCurrentPopup();
                };
                if (ImGui::Selectable("Bỏ vào thùng rác (xoá hẳn)")) add(TrashList::Action::Delete);
                ImGui::Selectable("Vứt xuống đất (chưa dùng được)", false,
                                  ImGuiSelectableFlags_Disabled);
                // On Tiêu Dùng only (simple_trash_use.cpp).
                if (simple_pick_slot_ >= 0 && static_cast<std::size_t>(simple_pick_slot_) < slots.size())
                    simple_use_cut_option(slots[static_cast<std::size_t>(simple_pick_slot_)]);
                ImGui::Separator();
                if (ImGui::Selectable("Thôi")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        hint("Ô viền đỏ là món sẽ bị bỏ, dù màu khác với hình đã chọn; ô viền xanh là món sẽ dùng. "
             "Món trông giống món đã chọn mà cũng giống cả một món khác hình chưa chọn thì không bị "
             "bỏ: bấm Thử tìm để xem.");
    }
    end_card();

    // ---- now ----
    if (!begin_card("##trashrun")) { end_card(); return; }
    const bool busy = trash_running_ && (trash_oneshot_ || trash_request_ != 0);
    // Not while the game's chat line may be open: a pass presses the bag key,
    // which would be a letter in it. The worker waits for that as well.
    const bool chat_open = chat_doubt_;
    ImGui::BeginDisabled(busy || chat_open || !input_.window_valid() || simple_trash_.items.empty());
    if (ImGui::Button("Thử tìm (không xoá)")) request_trash(true);
    ImGui::SameLine();
    if (ImGui::Button("Dọn ngay")) request_trash(false);
    ImGui::EndDisabled();
    ImGui::SameLine(0, 18);
    simple_hotkey_box(true);
    if (chat_open)
        coloured(kWait, "Ô chat trong game có thể đang mở: chưa dọn rác được cho đến khi ô chat đóng "
                        "(xem thẻ Tự chat).");
    hint("Thử tìm: mở túi đồ, xem từng ngăn và báo món nào sẽ bị bỏ, nhưng không bỏ gì. "
         "Khi một bước không được, GoBot giữ lại ảnh màn hình lúc đó trong thư mục "
         "trash_debug để sửa.");
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        lines = trash_log_;
    }
    last_lines(lines, 6);
    end_card();
}

// ------------------------------------------------------------------- chat

void GuiApp::simple_chat_tab() {
    simple_ui::trial_line(simple_ui::kTrialChat);
    const std::string cfg = exe_relative("configs/chat.ini");
    if (!simple_chat_loaded_) {
        simple_chat_loaded_ = true;
        // No file is Tự chat off, which is what the defaults say.
        if (!simple_chat_.load(cfg) && path_exists(cfg))
            simple_note("Không đọc được configs/chat.ini.", 8000);
        std::snprintf(simple_chat_lines_, sizeof simple_chat_lines_, "%s",
                      ChatSettings::join_lines(simple_chat_.lines).c_str());
        simple_chat_interval_ = simple_chat_.interval_s;
    }
    const auto put = [&](const char* key, const std::string& value) {
        if (!ChatSettings::save_key(cfg, key, value))
            simple_note("Không lưu được configs/chat.ini.", 8000);
    };

    // What is worth warning about, looked at every few seconds rather than
    // every frame: going through every process running is not free.
    if (simple_chat_checked_ms_ == 0 || now_ms() - simple_chat_checked_ms_ > 5000) {
        simple_chat_checked_ms_ = now_ms();
        std::string typers;
        for (const std::string& t : vietnamese_typers_running())
            typers += (typers.empty() ? "" : ", ") + t;
        simple_chat_typers_ = typers;
        simple_chat_layout_ = window_keyboard_layout(input_.target_window());
    }

    // ---- what to say, and how often ----
    if (!begin_card("##chat")) { end_card(); return; }
    heading(strong_font_, "Tự chat");
    if (ImGui::Checkbox("Bật tự chat", &simple_chat_.enabled)) {
        put("enabled", simple_chat_.enabled ? "true" : "false");
        // Not while BẮT ĐẦU is still opening the game: there is no chat line
        // yet, and what is ticked then starts once the character is in.
        if (simple_chat_.enabled && simple_running() && !simple_cold_start_) start_chat();
        // Not the player taking over the keys -- the fighting goes on -- so a
        // chat line left in doubt is still watched, and the keys held off,
        // until it shows shut.
        if (!simple_chat_.enabled && chat_running_ && !chat_oneshot_) stop_chat(false, false);
    }
    {
        const int first = simple_chat_.first_delay_s;
        const std::string when = first > 0 ? "khoảng " + std::to_string(first) +
                                                 " giây sau khi bấm BẮT ĐẦU"
                                           : "ngay khi bấm BẮT ĐẦU";
        hint(("Khi bot chạy, GoBot gõ các câu dưới đây vào ô chat của game, mỗi lần một câu: câu "
              "đầu " + when + ", sau đó theo giờ ở dưới. Đánh quái, bình và buff tạm dừng trong "
              "mấy giây lúc gõ.").c_str());
    }

    ImGui::Dummy(ImVec2(0, 4));
    label("Các câu sẽ gửi (mỗi dòng một câu, tối đa 10 câu)");
    ImGui::InputTextMultiline("##chatlines", simple_chat_lines_, sizeof simple_chat_lines_,
                              ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 6.5f));
    // Each line keeps its "Chỉ khi chưa có tổ đội" by its words (simple_chat_alone.cpp).
    if (ImGui::IsItemDeactivatedAfterEdit()) simple_chat_save_lines(cfg);

    // What each line will come out as, worked out from the box as it is now.
    {
        const std::string text = simple_chat_lines_;
        int written = 0;
        for (std::size_t at = 0; at <= text.size();) {
            const std::size_t end = text.find('\n', at);
            const std::string one = text.substr(at, end == std::string::npos ? std::string::npos
                                                                             : end - at);
            if (one.find_first_not_of(" \t\r") != std::string::npos) ++written;
            if (end == std::string::npos) break;
            at = end + 1;
        }
        const auto lines = ChatSettings::split_lines(text);
        ImGui::PushFont(nullptr, kSmall);
        if (lines.empty()) coloured(kWait, "Danh sách câu đang trống.");
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const ChatText t = prepare_chat(lines[i], simple_chat_.method,
                                            simple_chat_.strip_diacritics, simple_chat_.max_length);
            const std::string head = std::to_string(i + 1) + ". Sẽ gõ: ";
            if (t.empty()) {
                coloured(kBad, (head + "(không còn chữ nào gõ được, câu này sẽ bị bỏ qua)").c_str());
                continue;
            }
            std::string what;
            if (t.stripped) what += "bỏ dấu";
            if (t.dropped) what += std::string(what.empty() ? "" : ", ") + "bỏ ký tự không gõ được";
            if (t.cut) what += std::string(what.empty() ? "" : ", ") + "cắt còn " +
                               std::to_string(simple_chat_.max_length) + " ký tự";
            if (what.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextWrapped("%s%s", head.c_str(), t.typed.c_str());
                ImGui::PopStyleColor();
            } else {
                coloured(kWait, (head + t.typed + "   (" + what + ")").c_str());
            }
            simple_chat_alone_tick(lines, i);
        }
        if (written > ChatSettings::kMaxLines)
            coloured(kWait, ("Chỉ dùng " + std::to_string(ChatSettings::kMaxLines) +
                             " câu đầu tiên.").c_str());
        ImGui::PopFont();
    }

    ImGui::Dummy(ImVec2(0, 4));
    label("Gửi mỗi");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("##chatevery", &simple_chat_interval_, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const int asked = simple_chat_interval_;
        simple_chat_interval_ = ChatSettings::clamp_interval(asked);
        simple_chat_.interval_s = simple_chat_interval_;
        put("interval_s", std::to_string(simple_chat_interval_));
        if (asked < ChatSettings::kMinInterval)
            simple_note("Ít nhất 30 giây giữa hai câu, để khỏi bị coi là spam.", 8000);
        else if (asked > ChatSettings::kMaxInterval)
            simple_note("Nhiều nhất 3600 giây (1 giờ) giữa hai câu.", 8000);
    }
    ImGui::SameLine();
    label("giây (ít nhất 30)");
    ImGui::SameLine(0, 24);
    if (ImGui::RadioButton("Lần lượt", !simple_chat_.random_order)) {
        simple_chat_.random_order = false;
        put("order", "in_turn");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Ngẫu nhiên", simple_chat_.random_order)) {
        simple_chat_.random_order = true;
        put("order", "random");
    }

    ImGui::Dummy(ImVec2(0, 4));
    hint("Câu được gửi vào kênh đang chọn ở nút bên trái ô chat trong game (thường là Thường). "
         "Muốn gửi vào Đội hay Môn Phái thì đổi nút đó trong game trước khi bấm BẮT ĐẦU.");
    if (simple_chat_.method == ChatMethod::Vk)
        hint("Chỉ gõ được chữ không dấu, số và dấu cách; chữ có dấu sẽ được bỏ dấu.");
    else
        coloured(kWait, (std::string("Cách gõ: ") + chat_method_name(simple_chat_.method) +
                         " (đang thử nghiệm, đặt trong configs/chat.ini).").c_str());
    if (!simple_chat_.method_note.empty()) coloured(kWait, simple_chat_.method_note.c_str());

    // ---- what gets in the way ----
    if (!simple_chat_typers_.empty()) {
        ImGui::PushFont(nullptr, kSmall);
        coloured(kWait, ("Unikey/EVKey đang chạy (" + simple_chat_typers_ +
                         "): chuyển sang gõ tiếng Anh (E) khi dùng Tự chat, nếu không chữ có "
                         "thể bị đổi (vd dd thành đ).").c_str());
        ImGui::PopFont();
    }
    if (simple_chat_layout_ != 0 && simple_chat_layout_ != 0x0409) {
        ImGui::PushFont(nullptr, kSmall);
        coloured(kWait, "Bàn phím trong game không phải English (US): chữ và số có thể bị gõ sai.");
        ImGui::PopFont();
    }
    if (input_.window_valid() && overlap_fraction() > 0.02f) {
        mark(Mark::Bad);
        ImGui::SameLine();
        label("Cửa sổ GoBot đang che game");
        ImGui::SameLine();
        if (ImGui::Button("Dời GoBot sang bên##chat")) move_clear_of_game();
    }
    end_card();

    // ---- now ----
    if (!begin_card("##chatrun")) { end_card(); return; }
    const bool busy = chat_running_ && (chat_oneshot_ || chat_request_ != 0 || chat_stop_);
    ImGui::BeginDisabled(busy || !input_.window_valid());
    if (ImGui::Button("Kiểm tra ô chat")) request_chat(false);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Chỉ xem ô chat đang mở hay đóng, không bấm phím nào.\nĐừng để cửa sổ "
                          "nào che khung chat ở góc trái dưới màn hình game.");
    ImGui::SameLine();
    // As long as the shortest interval, as no two lines go closer together
    // than that; the worker also counts from a timed line.
    const bool cooling = simple_chat_tested_ms_ != 0 &&
                         now_ms() - simple_chat_tested_ms_ < ChatSettings::kMinInterval * 1000ull;
    const bool trash_busy = trash_pass_ || (trash_running_ && (trash_oneshot_ || trash_request_ != 0));
    const bool have_line = !ChatSettings::split_lines(simple_chat_lines_).empty();
    ImGui::BeginDisabled(busy || cooling || trash_busy || !input_.window_valid() || !have_line ||
                         chat_doubt_);
    if (ImGui::Button("Gửi thử 1 câu")) {
        simple_chat_tested_ms_ = now_ms();
        request_chat(true);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Gửi câu đầu tiên một lần, đúng các bước như khi chạy: đưa game lên "
                          "trước, mở ô chat, gõ, gửi.\nHai câu, kể cả câu gửi thử, luôn cách nhau ít "
                          "nhất 30 giây.\nCâu đầu được gửi thử cả khi có đánh dấu \"Chỉ khi chưa có tổ đội\".");
    hint("Khi một bước không được, GoBot giữ lại ảnh màn hình lúc đó trong thư mục chat_debug.");

    // The chat line maybe left open, first: it is why nothing else moves.
    if (chat_doubt_) {
        const bool watched = chat_doubt_watched_;
        coloured(kBad, watched
                           ? "Ô chat trong game có thể đang mở. GoBot giữ đánh quái, bình và buff lại "
                             "cho đến khi thấy ô chat đóng, để không gõ phím vào đó."
                           : "Tự chat đã dừng, nhưng ô chat trong game có thể vẫn đang mở: đánh quái, "
                             "bình và buff chưa chạy lại.");
        hint(watched ? "Đóng ô chat trong game: xoá chữ trong đó rồi bấm Enter. GoBot cũng tự xoá và "
                       "đóng khi thấy ô chat mở mà không ai dùng chuột hay bàn phím."
                     : "Đóng ô chat trong game (xoá chữ trong đó rồi bấm Enter), rồi bấm BẮT ĐẦU.");
        // The player taking it over, here on the tab: the big button reads
        // BẮT ĐẦU when only Tự chat's own worker is left watching the line --
        // after a Gửi thử, or with Tự chat unticked. As DỪNG does it: what
        // runs stops, GoBot presses nothing more into the line, and the keys
        // stay held off until a look shows it shut.
        if (watched) {
            if (ImGui::Button("Để tôi tự đóng ô chat")) {
                if (simple_running()) simple_stop();
                else                  stop_chat(true, false);
            }
            ImGui::SetItemTooltip("GoBot thôi tự xoá và đóng ô chat, và dừng các việc đang chạy như "
                                  "nút DỪNG.\nĐóng ô chat trong game (xoá chữ trong đó rồi bấm Enter), "
                                  "rồi bấm BẮT ĐẦU.");
        }
    }
    if (chat_running_ && chat_stop_) {
        coloured(kWait, chat_doubt_watched_ ? "Đang dừng: chờ thấy ô chat đóng…"
                                            : "Đang dừng: xoá câu đang gõ dở và đóng ô chat…");
    } else if (chat_running_ && !chat_oneshot_) {
        const std::uint64_t next = chat_next_ms_.load();
        std::string state = "Đang chạy: đã gửi " + std::to_string(chat_sent_.load()) + " câu";
        if (next != 0) {
            const std::uint64_t now = now_ms();
            const std::uint64_t left = next > now ? (next - now + 999) / 1000 : 0;
            state += ", câu tiếp theo sau " + std::to_string(left) + " giây";
        }
        coloured(kGood, (state + ".").c_str());
    } else if (simple_chat_.enabled) {
        coloured(kDim, "Sẽ chạy khi bấm BẮT ĐẦU.");
    } else {
        coloured(kDim, "Đang tắt.");
    }
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        lines = chat_log_;
    }
    last_lines(lines, 4);
    end_card();
}

// ------------------------------------------------------------------ login

void GuiApp::simple_login_look() {
    // Taken when it is in; never waited for.
    if (login_look_.valid()) {
        if (login_look_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        login_seen_ = login_look_.get();
    }
    // Counted from when the last one began, so an answer that waited while
    // the tab was not up is followed by a fresh look at once.
    if (login_seen_.done && now_ms() - login_looked_ms_ < 4000) return;
    login_looked_ms_ = now_ms();

    // One look at a time, so the pictures loaded here are only ever the
    // look's. The first one loads them, which takes a few seconds.
    login_look_ = std::async(std::launch::async, [this] {
        LoginLook out;
        if (!login_store_loaded_) {
            login_store_loaded_ = true;
            AutoLogin::load_pictures(login_store_, exe_relative("configs/templates.ini"));
        }
        AutoLogin::Settings login;
        login.load(exe_relative("configs/login.ini"));     // what ships, with no file
        out.pages = login.pages.size();
        out.recognised = login.recognised(login_store_);

        // Where a running VTCZone is, remembered: the tab shows it, and the
        // crossing starts it from there, when login.ini's path names nothing.
        AutoLogin::running_launcher(true);
        if (!SessionManager::process_running("VTCZone.exe")) {
            out.vtczone = VtcZone::NotRunning;
        } else if (void* hwnd = AutoLogin::window_of(login, AutoLogin::On::Publisher)) {
            ScreenCapture capture;
            Frame frame;
            if (capture.grab(hwnd, frame)) {
                AutoLogin::Publisher seen = AutoLogin::look_at_publisher(frame, login_store_, login);
                // A frame read off the screen is whatever is over VTCZone, and
                // a blue button on some other window is not VTCZone signed out.
                // This only looks, so it cannot bring VTCZone to the front as
                // the crossing does; the form counts only if VTCZone is clear.
                if (seen == AutoLogin::Publisher::SignIn && capture.from_screen() &&
                    !AutoLogin::uncovered(hwnd))
                    seen = AutoLogin::Publisher::Unknown;
                switch (seen) {
                    case AutoLogin::Publisher::Ready:   out.vtczone = VtcZone::SignedIn; break;
                    case AutoLogin::Publisher::SignIn:  out.vtczone = VtcZone::SignInForm; break;
                    case AutoLogin::Publisher::Unknown: break;
                }
            }
        }
        out.done = true;
        return out;
    });
}

void GuiApp::simple_login_tab() {
    simple_ui::trial_line(simple_ui::kTrialLogin);
    const std::string path = exe_relative("configs/login.ini");

    // Read every time, as the full screen does: the command line tool and a
    // person with a text editor write this file too.
    //
    // With no file at all, the one that ships is written out, so there is a
    // file to pick a channel in and to put right by hand. Once, until there is
    // a file again, so a folder that cannot be written to is not tried every
    // frame -- and only when there is truly none: a file that is there and
    // will not open for a moment is the player's, and is never replaced.
    AutoLogin::Settings login;
    bool have = login.load(path);
    if (have) {
        login_file_tried_ = false;
    } else if (!login_file_tried_ && AutoLogin::Settings::file_missing(path)) {
        login_file_tried_ = true;
        have = login.save(path);
        if (have) GB_INFO("Wrote %s with the pages that ship", path.c_str());
    }

    if (!begin_card("##login")) { end_card(); return; }
    heading(strong_font_, "Tự vào lại game");

    if (ImGui::Checkbox("Tự vào lại game khi bị văng hoặc mất kết nối", &simple_opts_.keep_alive)) {
        save_simple_option("keep_alive", simple_opts_.keep_alive);
        if (simple_running()) {
            if (simple_opts_.keep_alive) start_keeper();
            else                         stop_keeper();
        }
    }
    hint("GoBot theo dõi game. Nếu game tắt, bị treo hoặc hiện bảng lỗi, GoBot đóng game, bấm "
         "Chơi game trong VTCZone rồi vào đúng kênh và nhân vật dưới đây. VTCZone phải đang mở "
         "(hoặc mở được) và đã đăng nhập sẵn — GoBot không cần, không hỏi và không lưu tên đăng "
         "nhập hay mật khẩu.");
    hint("Game đang tắt? Bật ô này rồi bấm BẮT ĐẦU: GoBot mở game và vào nhân vật trước, rồi "
         "mới chạy đánh quái, bình và buff.");
    simple_stall_option();
    simple_hotkey_box(false);

    // Lines up the boxes after their names.
    const float field = ImGui::GetCursorPosX() + 150.0f;

    // ---- where to ----
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SeparatorText("Vào kênh và nhân vật nào");
    {
        const AutoLogin::Page* channel_page = nullptr;
        const AutoLogin::Page* char_page = nullptr;
        for (const auto& page : login.pages) {
            if (page.step == AutoLogin::Step::Channel)   channel_page = &page;
            if (page.step == AutoLogin::Step::Character) char_page = &page;
        }
        const int channels =
            channel_page != nullptr && channel_page->row_count > 0 ? channel_page->row_count : 8;
        const int characters =
            char_page != nullptr && char_page->row_count > 0 ? char_page->row_count : 3;

        // Counted from one here and from zero in the file: "Kênh 1" is what
        // the client calls it.
        //
        // One key changed in place. configs/login.ini is written in by hand,
        // and its comments are the only record of how each page was measured.
        // With no file, the one that ships is written out first and the key
        // changed in that; with one that will not open, nothing is written.
        const auto pick = [&](const char* id, const char* what, int count, int current,
                              const char* key) {
            label(what);
            ImGui::SameLine(field);
            ImGui::SetNextItemWidth(180);
            const std::string shown = std::string(what) + " " + std::to_string(current + 1);
            if (ImGui::BeginCombo(id, shown.c_str())) {
                for (int i = 0; i < count; ++i) {
                    const std::string item = std::string(what) + " " + std::to_string(i + 1);
                    if (!ImGui::Selectable(item.c_str(), i == current) || i == current) continue;
                    const bool wrote = (have || (AutoLogin::Settings::file_missing(path) &&
                                                 AutoLogin::Settings{}.save(path))) &&
                                       IniFile::set_in_place(path, "Login", key, i);
                    if (!wrote) simple_note("Không lưu được configs/login.ini.", 8000);
                }
                ImGui::EndCombo();
            }
        };
        pick("##channel", "Kênh", channels, std::clamp(login.channel_index, 0, channels - 1),
             "channel_index");
        pick("##character", "Nhân vật", characters,
             std::clamp(login.character_index, 0, characters - 1), "character_index");
        if (char_page != nullptr && char_page->pick == AutoLogin::Pick::Slot)
            hint("Nhân vật đếm từ bên trái màn hình chọn nhân vật.");
    }

    // ---- whether it can ----
    simple_login_look();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SeparatorText("Kiểm tra");
    const auto indented_hint = [](const char* text) {
        ImGui::Indent(mark_indent());
        hint(text);
        ImGui::Unindent(mark_indent());
    };

    login.adopt_running_launcher(false);     // seen running elsewhere: that one
    const bool launcher_ok = login.launcher_found();
    mark(launcher_ok ? Mark::Good : Mark::Bad);
    ImGui::SameLine();
    label(launcher_ok ? "Đã thấy VTCZone" : "Không thấy VTCZone");
    if (!login.launcher_path.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("(%s)", login.launcher_path.c_str());
        ImGui::PopStyleColor();
    }

    if (!login_seen_.done) {
        mark(Mark::Wait);
        ImGui::SameLine();
        label("Đang xem VTCZone và các hình nhận diện…");
    } else {
        const bool vtczone_up = login_seen_.vtczone != VtcZone::NotRunning;
        mark(vtczone_up ? Mark::Good : launcher_ok ? Mark::Wait : Mark::Bad);
        ImGui::SameLine();
        label(vtczone_up   ? "VTCZone đang chạy"
              : launcher_ok ? "VTCZone chưa chạy — GoBot sẽ tự mở"
                            : "VTCZone chưa chạy, và GoBot không tìm thấy nó để mở");
        if (!vtczone_up && !launcher_ok)
            indented_hint("Hãy mở VTCZone bằng tay, hoặc sửa dòng launcher= trong "
                          "configs/login.ini cho đúng chỗ cài VTCZone.");

        if (vtczone_up) {
            switch (login_seen_.vtczone) {
                case VtcZone::SignedIn:
                    mark(Mark::Good);
                    ImGui::SameLine();
                    label("VTCZone đã đăng nhập");
                    break;
                case VtcZone::SignInForm:
                    mark(Mark::Bad);
                    ImGui::SameLine();
                    label("VTCZone đang ở màn hình đăng nhập — hãy đăng nhập bằng tay");
                    indented_hint("Chọn ghi nhớ đăng nhập nếu có, để VTCZone tự giữ đăng nhập. "
                                  "GoBot không bao giờ gõ gì vào đó.");
                    break;
                default:
                    mark(Mark::Wait);
                    ImGui::SameLine();
                    label("Chưa xem được VTCZone đã đăng nhập chưa");
                    indented_hint("Để VTCZone hiện rõ trên màn hình, ở trang có nút Chơi game "
                                  "(không thu nhỏ, không bị che), rồi xem lại.");
                    break;
            }
        }

        const bool all_seen = login_seen_.pages > 0 && login_seen_.recognised == login_seen_.pages;
        mark(all_seen ? Mark::Good : Mark::Bad);
        ImGui::SameLine();
        label(("Đã nhận ra " + std::to_string(login_seen_.recognised) + "/" +
               std::to_string(login_seen_.pages) + " màn hình vào game").c_str());
        if (!all_seen) indented_hint("Thiếu hình nhận diện màn hình — cài lại GoBot.");
    }

    if (!elevated_) {
        mark(Mark::Bad);
        ImGui::SameLine();
        label("GoBot không chạy bằng quyền Administrator nên không bấm được vào VTCZone/game.");
    }
    end_card();

    // ---- what it is doing ----
    if (!begin_card("##loginstate")) { end_card(); return; }
    std::string now, problem;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        now = keeper_vi_;
        problem = keeper_problem_vi_;
    }
    if (keeper_running_) {
        const int restarts = keeper_restarts_.load();
        coloured(kGood, ((now.empty() ? std::string("Đang theo dõi game.") : now) +
                         (restarts > 0 ? " Đã vào lại " + std::to_string(restarts) + " lần."
                                       : std::string())).c_str());
    } else {
        coloured(kDim, simple_opts_.keep_alive ? "Sẽ bật khi bấm BẮT ĐẦU." : "Đang tắt.");
    }
    // What stopped it last, in words; the English is in gobot.log.
    if (!problem.empty()) coloured(kBad, problem.c_str());
    end_card();
}

} // namespace gobot
