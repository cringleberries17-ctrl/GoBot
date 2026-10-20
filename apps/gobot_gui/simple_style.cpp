// Đánh quái's attack style ("Kiểu đánh"), keeping distance ("Giữ khoảng
// cách") and the jump key ("Phím nhảy") with its "Thử": their pieces of the
// "Tự đánh quái" row, the "Tầm đánh" window, the jump key's line in "Trước
// khi bắt đầu", and the one press "Thử" makes.
//
// What the numbers do in a fight is the hunter's (gobot/hunter.hpp), the
// styles are gobot/attack_style.hpp, and "Thử" is gobot/jump_check.hpp. Here
// is only what the window draws and keeps. "Thử" acts on the game -- one key
// press, on the player's click -- so it is held to the gates everything that
// presses keys is: not while the bot runs, not under a hold, and through
// keys_may_start. In a file of its own so that simple_view.cpp gets only the
// calls.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/attack_style.hpp"
#include "gobot/common.hpp"
#include "gobot/jump_check.hpp"
#include "gobot/logger.hpp"
#include "gobot/navigator.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <vector>

#include <windows.h>    // the VK_ codes keys are stored as

namespace gobot {

/// What the window keeps for these: the "Tầm đánh" window asked for, and
/// "Thử" -- the check under way, and what the last one said.
struct GuiApp::StyleUi {
    bool open_ranges = false;

    std::future<JumpCheck::Result> jump;
    std::atomic<bool> jump_stop{ false };
    bool              jump_started_detector = false;
    std::uint32_t     jump_key = 0;        ///< the key the check pressed
    std::string       jump_said;           ///< what the last check said
    /// The key a check pressed this session and saw no jump on: "đã thử,
    /// không thấy nhảy" rather than "chưa thử", which it was not. Not kept
    /// in hunt.ini, which has only tried-and-seen (jump_checked).
    std::uint32_t     jump_not_seen = 0;

    /// The checklist's "Đổi phím nhảy": the jump box waits for a key.
    bool listen_jump = false;

    /// Every other key GoBot presses or watches, for the jump key's line:
    /// three of them come from files, so they are not read on every frame.
    std::vector<NamedKey> keys;
    std::uint64_t         keys_ms = 0;
};

void GuiApp::StyleUiFree::operator()(StyleUi* ui) const {
    // A "Thử" still under way presses nothing more, and is waited for: it
    // reads the detector and the keyboard, which go after this.
    if (ui) ui->jump_stop = true;
    delete ui;
}

namespace {

using namespace simple_ui;

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

/// GoC's four sets by their own names, after the classes and weapons.
bool by_set_name(const AttackStyle& s) {
    const std::string id = s.id;
    return id.rfind("near_", 0) == 0 || id.rfind("far_", 0) == 0;
}

const char* const kRangesTitle = "Tầm đánh";

} // namespace

GuiApp::StyleUi& GuiApp::style_ui() {
    if (!style_ui_) style_ui_.reset(new StyleUi);
    return *style_ui_;
}

// ------------------------------------------------------------- the row

void GuiApp::simple_jump_box() {
    StyleUi& ui = style_ui();
    ImGui::SameLine(0, 18);
    label("Phím nhảy");
    ImGui::SameLine();
    // The checklist's "Đổi phím nhảy": this box waits for the key.
    if (ui.listen_jump) {
        ui.listen_jump = false;
        ImGui::PushID("jump");
        simple_listening_ = ImGui::GetID("##box");
        ImGui::PopID();
    }
    std::uint32_t key = hunt_config_.jump_key;
    // Marked as on trial in every state, not only once a check has said
    // something: the hint under the row comes only then.
    const std::string box_tip =
        trial_hint(kTrialJump, "Phím nhảy trong game (thường là ↑). Bot nhảy khi quái ở chỗ cao hơn. Đứng "
                               "chỗ trống, xa cổng dịch chuyển và dây leo rồi bấm Thử để xem nhân vật có "
                               "nhảy không.");
    if (simple_key_box("jump", key, true, false, box_tip.c_str())) {
        hunt_config_.set_jump_key(key);     // a new key has not been tried
        simple_changed();
    }

    // "Thử" presses a key in the game, so: only when nothing of the bot
    // runs, nothing holds the game, there is a game and a key -- and it says
    // which of these it waits for.
    ImGui::SameLine();
    const bool busy = ui.jump.valid();
    std::string why;
    if (busy)                              why = "Đang thử phím nhảy…";
    else if (!input_.window_valid())       why = "Chưa tìm thấy game.";
    else if (simple_running())             why = "Bot đang chạy: dừng bot rồi hãy thử.";
    else if (keeper_entering())            why = "Đang vào game — thử lại sau.";
    else if (hunt_config_.jump_key == 0)   why = "Chưa đặt phím nhảy.";
    else if (coord_.critical_active())
        why = "Bot đang tạm dừng (" + coord_.critical_reason() + ") — thử lại sau.";
    ImGui::BeginDisabled(!why.empty());
    if (ImGui::Button(busy ? "Đang thử…###jumptry" : "Thử###jumptry")) simple_jump_try();
    ImGui::EndDisabled();
    const std::string try_tip =
        trial_hint(kTrialJump, "Bấm phím nhảy MỘT lần trong game rồi xem nhân vật có nhảy lên không.\nGame "
                               "được đưa lên phía trước. Đứng chỗ trống, xa cổng dịch chuyển và dây leo.");
    ImGui::SetItemTooltip("%s", why.empty() ? try_tip.c_str() : why.c_str());
}

void GuiApp::simple_style_line() {
    StyleUi& ui = style_ui();
    label("Kiểu đánh");
    ImGui::SameLine();

    // What the picker shows: the style, or "Tự chỉnh" once its numbers were
    // edited by hand -- told apart by the numbers themselves.
    const AttackStyle* now = find_attack_style(hunt_config_.style);
    std::string shown;
    if (now)
        shown = attack_style_matches(hunt_config_, *now)
                    ? std::string(now->label_vi)
                    : std::string(kCustomStyleVi) + " (" + now->label_vi + ")";
    else
        shown = legacy_ranges(hunt_config_) ? std::string(kLegacyStyleVi)
                                            : std::string(kLegacyStyleVi) + " (đã chỉnh tầm)";

    // Narrow enough to leave room for "Tầm đánh…" on the line; the list that
    // drops down is as wide as its longest entry. Marked as on trial in every
    // state: the hint under the row comes only with a style chosen.
    ImGui::SetNextItemWidth(220);
    const bool open = ImGui::BeginCombo("##style", shown.c_str(), ImGuiComboFlags_HeightLarge);
    if (!open)
        ImGui::SetItemTooltip("%s", trial_hint(kTrialStyles, "Tầm đánh và cách đánh theo lớp nhân vật, như "
                                                             "\"Config Mẫu\" của GoC. Như trước: bot đánh "
                                                             "như từ trước tới giờ.")
                                        .c_str());
    if (open) {
        if (ImGui::Selectable(kLegacyStyleVi, now == nullptr)) {
            apply_legacy_style(hunt_config_);
            simple_changed();
        }
        ImGui::SetItemTooltip("Như GoBot từ trước tới giờ: tầm đánh 150 px, cao thấp 60 px, chọn lại "
                              "mục tiêu mỗi 4 giây, quay người mỗi lần đánh.");
        const auto& styles = attack_styles();
        for (std::size_t i = 0; i < styles.size(); ++i) {
            const AttackStyle& s = styles[i];
            if (i == 0 || (by_set_name(s) && !by_set_name(styles[i - 1]))) ImGui::Separator();
            ImGui::PushID(s.id);
            if (ImGui::Selectable(s.label_vi, now == &s)) {
                apply_attack_style(hunt_config_, s);
                simple_changed();
            }
            const RangePreset& r = *s.ranges;
            ImGui::SetItemTooltip("Đánh từ cách %d đến %d px; cao nhất %d px, thấp nhất %d px; không "
                                  "cần nhảy: cao %d, thấp %d px.",
                                  r.min_range, r.max_range, r.highest_hit_y, r.lowest_hit_y,
                                  r.higher_threshold, r.lower_threshold);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(kAttackStyleGuideVi);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if (ImGui::Button("Tầm đánh…")) ui.open_ranges = true;
    ImGui::SetItemTooltip("Các số của kiểu đánh -- tầm đánh, khoảng cách tối thiểu, cao, thấp -- "
                          "và Giữ khoảng cách.");
}

void GuiApp::simple_keep_distance_tick() {
    ImGui::SameLine(0, 18);
    // GoC never steps back in its lazy mode, and neither does this.
    ImGui::BeginDisabled(hunt_config_.stay_put);
    if (ImGui::Checkbox("Giữ khoảng cách", &hunt_config_.keep_distance)) simple_changed();
    ImGui::EndDisabled();
    const std::string tip =
        hunt_config_.stay_put
            ? std::string("Không lùi khi Đứng yên một chỗ: bỏ dấu Đứng yên trước.")
            : trial_hint(kTrialKeepDistance,
                         ("Lùi lại khi quái gần hơn khoảng cách tối thiểu (Kiểu đánh), tối đa " +
                          std::to_string(hunt_config_.keep_distance_steps) +
                          " bước liền nhau. Không lùi khi Đứng yên một chỗ.")
                             .c_str());
    ImGui::SetItemTooltip("%s", tip.c_str());
}

void GuiApp::simple_style_hints() {
    StyleUi& ui = style_ui();
    if (hunt_config_.goc_rules())
        hint(trial_hint(kTrialStyles,
                        ("Kiểu đánh theo GoC: đánh một con tới khi nó chết, chỉ quay người khi cần, bỏ "
                         "con nào đánh " +
                         std::to_string(hunt_config_.no_damage_ms / 1000) +
                         " giây không thấy mất máu.").c_str())
                 .c_str());
    if (hunt_config_.keep_distance && !hunt_config_.stay_put) {
        const std::string text =
            hunt_config_.min_distance <= 0
                ? std::string("Chưa chọn Kiểu đánh: bot lùi khi quái gần hơn ") +
                      std::to_string(Hunter::kDefaultKeepAwayPx) + " px."
                : "Lùi lại khi quái gần hơn " + std::to_string(hunt_config_.min_distance) +
                      " px, tối đa " + std::to_string(hunt_config_.keep_distance_steps) +
                      " bước liền nhau.";
        hint(trial_hint(kTrialKeepDistance, text.c_str()).c_str());
    }
    if (!ui.jump_said.empty()) hint(trial_hint(kTrialJump, ui.jump_said.c_str()).c_str());
}

// ---------------------------------------------------------- "Tầm đánh"

void GuiApp::simple_style_popups() {
    StyleUi& ui = style_ui();
    if (ui.open_ranges) {
        ui.open_ranges = false;
        ImGui::OpenPopup(kRangesTitle);
    }
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kRangesTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    bool changed = false;
    const AttackStyle* now = find_attack_style(hunt_config_.style);
    label(("Kiểu đánh: " + std::string(now ? now->label_vi : kLegacyStyleVi)).c_str());
    hint("Đo bằng điểm ảnh trên màn hình game, như số của GoC. Sửa một số thì kiểu đánh thành "
         "\"Tự chỉnh\"; chọn lại kiểu đánh để lấy lại số của nó.");
    ImGui::Dummy(ImVec2(0, 4));

    // One number: a box with + and -, and GoC's own words for it.
    const auto number = [&changed](const char* text, const char* tip, int& v, int lo, int hi,
                                   int step) {
        int x = v;
        ImGui::SetNextItemWidth(150);
        if (ImGui::InputInt(text, &x, step, step * 10)) {
            v = std::clamp(x, lo, hi);
            changed = true;
        }
        ImGui::SetItemTooltip("%s", tip);
    };
    int min_d = hunt_config_.min_distance;
    int reach = hunt_config_.reach;
    int up = hunt_config_.up();
    int down = hunt_config_.down();
    number("Khoảng cách tối thiểu (px)", "Khoảng cách tối thiểu đến mục tiêu. 0: không có.", min_d, 0,
           400, 1);
    number("Tầm đánh tối đa (px)", "Tầm đánh tối đa.", reach, 40, 500, 1);
    number("Cao nhất (px)", "Vị trí cao nhất có thể tấn công.", up, 0, 400, 1);
    number("Thấp nhất (px)", "Vị trí thấp nhất có thể tấn công.", down, 0, 400, 1);
    if (changed) {
        hunt_config_.reach = reach;
        hunt_config_.min_distance = std::min(min_d, reach);
        if (up != hunt_config_.up() || down != hunt_config_.down()) hunt_config_.set_band(up, down);
    }

    // The no-jump band, for what comes next.
    label(("Cao không cần nhảy: " + std::to_string(hunt_config_.up_nj()) + " px").c_str());
    ImGui::SetItemTooltip("Tấn công quái ở vị trí cao hơn mà không nhảy lên.");
    ImGui::SameLine(0, 24);
    label(("Thấp không cần nhảy: " + std::to_string(hunt_config_.down_nj()) + " px").c_str());
    ImGui::SetItemTooltip("Tấn công quái ở vị trí thấp hơn mà không nhảy xuống.");
    hint("Hai số này dùng cho Đánh khi nhảy (sắp có).");

    // Keeping distance, and turning.
    ImGui::Dummy(ImVec2(0, 6));
    heading(strong_font_, "Giữ khoảng cách");
    int steps = hunt_config_.keep_distance_steps;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Lùi tối đa");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("##steps", &steps, 1, 1)) {
        hunt_config_.keep_distance_steps = std::clamp(steps, 1, 10);
        changed = true;
    }
    ImGui::SetItemTooltip("Bot không thấy mép bục, nên lùi có giới hạn: hết số bước này mà quái vẫn "
                          "gần thì đánh tại chỗ (1–10).");
    ImGui::SameLine();
    label("bước liền nhau");
    int facing = hunt_config_.facing_tolerance;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Hạn chế quay người:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("##facing", &facing, 1, 5)) {
        hunt_config_.facing_tolerance = std::clamp(facing, 0, 100);
        changed = true;
    }
    ImGui::SetItemTooltip("Quái lệch sang bên quá số này (0–100) thì mới quay người về phía nó.");
    ImGui::SameLine();
    label("px");

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("Đóng")) ImGui::CloseCurrentPopup();
    // Saved at once, and a running hunter takes it on its next pass.
    if (changed) simple_changed();
    ImGui::EndPopup();
}

// ------------------------------------------------------- the checklist

void GuiApp::simple_jump_checklist() {
    // The walker's jump key, hunt.ini's [Move] one: no box here sets it, and
    // a file from before it shipped as ↑ has Space, the game's attack. Said
    // whenever it would attack rather than jump, fighting ticked or not; the
    // player's file is left as it is.
    {
        const std::string walk = JumpCheck::walk_key_warning_vi(nav_config_.jump_key, hunt_config_.attack_key);
        if (!walk.empty()) {
            mark(Mark::Wait);
            ImGui::SameLine();
            label(("Phím nhảy khi đi tới điểm đã lưu là " + key_text(nav_config_.jump_key) + ": sẽ đánh chứ không "
                   "nhảy").c_str());
            ImGui::Indent(mark_indent());
            hint(walk.c_str());
            ImGui::Unindent(mark_indent());
        }
    }
    // Only where it matters: fighting ticked, and the hop wanted.
    if (!hunt_config_.enabled || !hunt_config_.jump_to_reach) return;
    StyleUi& ui = style_ui();
    const std::uint64_t now = now_ms();
    if (ui.keys_ms == 0 || now - ui.keys_ms > 3000) {
        ui.keys_ms = now;
        ui.keys.clear();
        for (NamedKey& k : game_keys_in_use())
            if (k.what_vi != "nhảy") ui.keys.push_back(std::move(k));
    }

    const std::uint32_t jump = hunt_config_.jump_key;
    const JumpCheck::KeyAdvice a = JumpCheck::advice(jump, hunt_config_.attack_key, ui.keys);
    const std::string k = key_text(jump);
    Mark m = Mark::Wait;
    std::string line, help;
    bool fix = true;
    switch (a.kind) {
        case JumpCheck::Advice::NotSet:
            line = "Chưa đặt phím nhảy";
            help = "Bot sẽ không nhảy lên chỗ cao. Phím nhảy trong game thường là ↑.";
            break;
        case JumpCheck::Advice::Conflict: {
            m = Mark::Bad;
            // "phím kỹ năng" reads "trùng phím kỹ năng", not "phím phím".
            std::string what = a.what_vi;
            if (what.rfind("phím ", 0) == 0) what = what.substr(std::string("phím ").size());
            line = "Phím nhảy trùng phím " + what + " (" + k + ")";
            help = "Một phím chỉ làm một việc: bấm để nhảy thì game làm việc kia.";
            break;
        }
        case JumpCheck::Advice::SpaceIsAttack:
            line = "Phím nhảy đang là Space";
            help = "Trong game Space là phím Tấn công: bot sẽ đánh chứ không nhảy. Phím nhảy trong game "
                   "thường là ↑.";
            break;
        case JumpCheck::Advice::Clean: {
            // Three things it can be: tried and seen to jump; tried, pressed
            // and not seen to; not tried. The second was shown as the third.
            const bool not_seen = !hunt_config_.jump_checked && ui.jump_not_seen == jump;
            m = hunt_config_.jump_checked ? Mark::Good : Mark::Wait;
            line = "Phím nhảy: " + k +
                   (hunt_config_.jump_checked ? " (đã thử)"
                    : not_seen                ? " (đã thử, không thấy nhảy)"
                                              : " (chưa thử)");
            if (not_seen)
                help = "Đã bấm " + k + " một lần nhưng không thấy nhân vật nhảy. Hãy nhìn trong game và "
                       "kiểm tra phím nhảy trong cài đặt phím của game.";
            else if (!hunt_config_.jump_checked)
                help = "Đứng chỗ trống, xa cổng dịch chuyển và dây leo, rồi bấm Thử ở dòng Tự đánh quái.";
            fix = not_seen;
            break;
        }
    }
    mark(m);
    ImGui::SameLine();
    label(line.c_str());
    if (fix) {
        ImGui::SameLine();
        if (ImGui::Button(jump == 0 ? "Đặt phím nhảy" : "Đổi phím nhảy")) ui.listen_jump = true;
        ImGui::SetItemTooltip("Ô Phím nhảy ở dòng Tự đánh quái sẽ chờ bạn nhấn phím.");
    }
    if (!help.empty()) {
        ImGui::Indent(mark_indent());
        hint(help.c_str());
        ImGui::Unindent(mark_indent());
    }
}

// --------------------------------------------------------------- "Thử"

bool GuiApp::keeper_entering() {
    // The one-shot "vào game ngay" is no run (simple_running), and takes no
    // hold while it clicks through the login pages; a cold start's keeper,
    // and a crossing under way, likewise. The popup closer asks the same.
    if (keeper_running_ || keeper_cold_start_) return true;
    std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
    return keeper_flow_ != nullptr;
}

void GuiApp::simple_jump_try() {
    StyleUi& ui = style_ui();
    if (ui.jump.valid() || hunt_config_.jump_key == 0) return;
    // Never into the login pages while the keeper is getting into the game:
    // a jump key there moves the selection or presses a button, and a
    // crossing spoilt is one of its three tries.
    if (keeper_entering()) {
        simple_note("Đang vào game — thử lại sau.");
        return;
    }
    // Never into a chat line or the bin's dialog left open: it says why.
    if (!keys_may_start()) return;
    const std::uint32_t key = hunt_config_.jump_key;

    // The character's plate is what is watched, and the detector reads it.
    // Started here it only looks, and it is stopped again afterwards.
    ui.jump_started_detector = false;
    if (!detector_running_) {
        start_detector();
        if (!detector_running_) {
            simple_note("Không đọc được màn hình game nên chưa thử phím nhảy.");
            return;
        }
        ui.jump_started_detector = true;
    }
    ui.jump_stop = false;
    ui.jump_key = key;
    ui.jump_said = "Đang thử phím nhảy " + key_text(key) + "…";
    GB_INFO("Jump test: pressing %s once to see whether the character jumps",
            InputManager::key_name(key).c_str());
    std::atomic<bool>* stop = &ui.jump_stop;
    ui.jump = std::async(std::launch::async, [this, key, stop] {
        JumpCheck check(coord_);
        JumpCheck::Eyes eyes;
        eyes.player = [this](Nameplate& out, bool* fresh) { return detector_.player(out, fresh); };
        check.set_eyes(eyes);
        JumpCheck::Hands hands;
        // A key goes only to the game in front, and the plate is read off the
        // screen, where whatever covers the game is read instead.
        hands.focus = [this] { return input_.window_focused() || input_.focus_window(); };
        hands.focused = [this] { return input_.window_focused(); };
        hands.key = [this](std::uint32_t vk, int hold) {
            const bool ok = input_.key_press(vk, hold);
            // Down, and the game lost focus before it came up: the key-up
            // was refused and the key is still down in Windows. release_all
            // sends the key-up whatever has focus.
            if (!ok && input_.is_held(vk)) input_.release_all();
            return ok;
        };
        check.set_hands(hands);
        return check.run(key, stop);
    });
}

void GuiApp::simple_style_tick() {
    if (!style_ui_) return;
    StyleUi& ui = *style_ui_;
    if (!ui.jump.valid() || ui.jump.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    const JumpCheck::Result r = ui.jump.get();
    ui.jump_said = JumpCheck::said_vi(r, key_text(ui.jump_key));
    GB_INFO("Jump test: %s %s -- %s; the plate rose %d px over %d reads",
            r.pressed ? "pressed" : "did not press", InputManager::key_name(ui.jump_key).c_str(),
            JumpCheck::outcome_name(r.outcome), r.rose_px, r.reads);

    // The detector as it was: stopped again, if the test started it and
    // nothing has taken it up since.
    if (ui.jump_started_detector && detector_running_ && !hunting_ &&
        !(navigator_ && navigator_->running()))
        stop_detector();
    ui.jump_started_detector = false;

    // "Đã thử" by what was seen, on the key that was pressed -- and, for the
    // checklist, that it was pressed and not seen to jump.
    if (hunt_config_.jump_key == ui.jump_key &&
        (r.outcome == JumpCheck::Outcome::Jumped || r.outcome == JumpCheck::Outcome::NoJump)) {
        const bool jumped = r.outcome == JumpCheck::Outcome::Jumped;
        ui.jump_not_seen = jumped ? 0 : ui.jump_key;
        if (hunt_config_.jump_checked != jumped) {
            hunt_config_.jump_checked = jumped;
            simple_changed();
        }
    }
    simple_note(ui.jump_said, 12000);
}

void GuiApp::simple_style_stop() {
    if (style_ui_) style_ui_->jump_stop = true;
}

} // namespace gobot
