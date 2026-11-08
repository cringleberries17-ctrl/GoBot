// Đánh quái's attack style ("Kiểu đánh"), keeping distance ("Giữ khoảng
// cách") and the jump key ("Phím nhảy") with its "Thử": their pieces of the
// "Tự đánh quái" row, the "Tầm đánh" window -- which has the hunting spot's
// numbers too ("Chỗ đánh quái", simple_spot.cpp) -- the jump key's line in
// "Trước khi bắt đầu", and the one press "Thử" makes. And the moves on the
// row: "Nhảy xuống bậc dưới", "Nhảy lên bục trên" with its "Thử ↑", "Thử ↑↑"
// and "Thử Ctrl + ↑", and "Tự tìm quái trong vùng" (2026.11.05b) -- their
// ticks, their lines under the row, and what the card says of them.
//
// What the numbers do in a fight is the hunter's (gobot/hunter.hpp), the
// styles are gobot/attack_style.hpp, and "Thử" is gobot/jump_check.hpp; why a
// tick cannot work and which card lines hold are gobot/hunt_card.hpp's. Here
// is only what the window draws and keeps. "Thử" acts on the game -- one key
// press, or one jump, on the player's click -- so it is held to the gates
// everything that presses keys is: not while the bot runs, not under a hold,
// and through keys_may_start. In a file of its own so that simple_view.cpp
// gets only the calls.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/attack_style.hpp"
#include "gobot/common.hpp"
#include "gobot/hunt_card.hpp"
#include "gobot/jump_check.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_book.hpp"
#include "gobot/navigator.hpp"
#include "gobot/packet_eyes.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
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

    /// "Thử ↑", "Thử ↑↑", "Thử Ctrl + ↑" (2026.11.05b): the jump under way
    /// -- its result says which kind -- and what the last one said.
    std::future<JumpCheck::KindResult> kind;
    std::atomic<bool> kind_stop{ false };
    std::string       kind_said;
};

void GuiApp::StyleUiFree::operator()(StyleUi* ui) const {
    // A "Thử" still under way presses nothing more, and is waited for: it
    // reads the detector and the keyboard, which go after this.
    if (ui) {
        ui->jump_stop = true;
        ui->kind_stop = true;
    }
    delete ui;
}

bool GuiApp::jump_test_running() const { return style_ui_ && style_ui_->jump.valid(); }

bool GuiApp::jump_kind_running() const { return style_ui_ && style_ui_->kind.valid(); }

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
                               "chỗ trống rồi bấm Thử để xem nhân vật có nhảy không. Không chọn được ↓: "
                               "trên cửa, ↓ đưa nhân vật sang bản đồ khác.");
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
    else if (InputManager::never_pressed(hunt_config_.jump_key))
        why = "GoBot không bấm ↓ một mình (trên cửa, ↓ đưa nhân vật sang bản đồ khác). Đặt phím nhảy khác.";
    else if (coord_.critical_active())
        why = "Bot đang tạm dừng (" + coord_.critical_reason() + ") — thử lại sau.";
    ImGui::BeginDisabled(!why.empty());
    if (ImGui::Button(busy ? "Đang thử…###jumptry" : "Thử###jumptry")) simple_jump_try();
    ImGui::EndDisabled();
    const std::string try_tip =
        trial_hint(kTrialJump, "Bấm phím nhảy MỘT lần trong game rồi xem nhân vật có nhảy lên không.\nGame "
                               "được đưa lên phía trước. Đứng chỗ trống.");
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
                              "mục tiêu mỗi 4 giây; chỉ quay người khi chưa quay về phía quái.");
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
                          "Giữ khoảng cách và Chỗ đánh quái.");
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
                         ("Lùi lại khi quái gần hơn khoảng cách tối thiểu (Kiểu đánh). Lùi tối đa " +
                          std::to_string(hunt_config_.keep_distance_steps) +
                          " bước về một phía rồi đứng đánh tại chỗ, tới khi nhân vật đi bộ về phía "
                          "ngược lại mới lùi tiếp. Không lùi khi Đứng yên một chỗ.")
                             .c_str());
    ImGui::SetItemTooltip("%s", tip.c_str());
}

// --------------------------------------------- "Nhảy xuống bậc dưới"

std::string GuiApp::simple_drop_refusal_vi() {
    // The player's answer 7: never standing still. And only with the packets
    // steering, where the drop's every rule comes from. The hunter refuses
    // the same at run time (Hunter::drop_rule), whatever this shows.
    if (hunt_config_.stay_put) return "Không nhảy xuống khi Đứng yên một chỗ: bỏ dấu Đứng yên trước.";
    if (hunt_config_.packet_positions != PacketPositions::On) return "Cần dấu Vị trí từ gói tin.";
    // A key of the hunter's on Ctrl would be pressed with the drop's ↓; one
    // of GoBot's own on Ctrl or ↓ would read the drop's press as the player's.
    const auto ctrl = [](std::uint32_t vk, bool down) {
        const std::uint32_t low = vk & 0xFFu;
        return vk != 0 && (low == 0x11u || low == 0xA2u || low == 0xA3u || (down && low == 0x28u));
    };
    const struct {
        std::uint32_t vk;
        bool          own;
        const char*   what;
    } keys[] = {
        { hunt_config_.jump_key, false, "nhảy" },
        { hunt_config_.left_key, false, "đi sang trái" },
        { hunt_config_.right_key, false, "đi sang phải" },
        { hunt_config_.attack_key, false, "đánh" },
        { macro_config_.toggle_key, true, "bật/tắt bot" },
        { macro_config_.panic_key, true, "dừng ngay" },
        { hunt_config_.toggle_key, true, "bật/tắt đánh quái" },
        { hotkeys_.trash_now, true, "dọn túi ngay" },
        { hotkeys_.login_now, true, "vào game ngay" },
    };
    for (const auto& k : keys)
        if (ctrl(k.vk, k.own))
            return std::string("Phím ") + k.what + " đang là " + ((k.vk & 0xFFu) == 0x28u ? "↓" : "Ctrl") +
                   ": đổi phím khác trước.";
    return {};
}

void GuiApp::simple_drop_tick() {
    // Beside "Vị trí từ gói tin", or under it when the row is full.
    const char* const kDrop = "Nhảy xuống bậc dưới";
    const float wide =
        ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(kDrop).x;
    ImGui::SameLine(0, 18);
    if (ImGui::GetContentRegionAvail().x < wide) ImGui::NewLine();
    // Greyed out, with its reason, while it cannot be ticked.
    const std::string why = simple_drop_refusal_vi();
    ImGui::BeginDisabled(!why.empty());
    if (ImGui::Checkbox("Nhảy xuống bậc dưới", &hunt_config_.drop_down)) simple_changed();
    ImGui::EndDisabled();
    const std::string tip =
        !why.empty()
            ? why
            : trial_hint(kTrialDrop,
                         "Khi con quái đang nhắm ở bậc thấp hơn và trên bậc này không còn con quái nào, bot đi tới\n"
                         "trên chỗ con quái, đứng lại rồi bấm Ctrl + ↓ để nhảy xuống — chỉ khi bản đồ của game cho\n"
                         "thấy nhân vật sẽ rơi xuống đúng bậc của con quái (hoặc một bậc ở giữa), không rơi quá.\n"
                         "Chỉ nhảy trên bản đồ GoBot đã thấy nhân vật vào (qua cửa hoặc đăng nhập khi GoBot đang mở),\n"
                         "hoặc đã nhận ra từ chỗ nhân vật và quái đứng. Không nhảy gần cửa. Có Chỗ đánh quái: chỉ\n"
                         "nhảy trong vùng đánh. GoBot không bao giờ bấm ↓ một mình: ↓ chỉ đi ra khi đang giữ Ctrl.");
    ImGui::SetItemTooltip("%s", tip.c_str());
}

void GuiApp::simple_drop_hints() {
    // Why it stopped the last hunt, in red until the next start of it.
    if (!drop_stopped_vi_.empty()) coloured(kBad, drop_stopped_vi_.c_str());
    if (!hunt_config_.drop_down || !simple_drop_refusal_vi().empty()) return;
    const HunterConfig& c = hunt_config_;
    // As drop_rule has them: more than the larger of the band below and
    // drop_min_px down, drop_side_px across at most, and no gate within
    // drop_gate_px.
    std::string text = "Nhảy xuống (Ctrl + ↓) khi quái thấp hơn quá " +
                       std::to_string(std::max(c.down(), c.drop_min_px)) + " px, cách không quá " +
                       std::to_string(c.drop_side_px) + " px theo chiều ngang; không nhảy khi có cửa trong vòng " +
                       std::to_string(c.drop_gate_px) + " px.";
    // The area's height said with it: at the shipped 100, a monster dropped
    // to is already outside it. The map tab's zone, by its own box.
    const Hunter::Status hs = hunting_ && hunter_ ? hunter_->status() : Hunter::Status{};
    if (c.spot && hs.spot_set && hs.spot_from_map)
        text += " Có Chỗ đánh quái: chỉ nhảy khi chỗ đứng, chỗ rơi xuống và con quái đều ở trong vùng đánh (vùng "
                "dọc " + std::to_string(std::lround(hs.spot_top)) + "–" + std::to_string(std::lround(hs.spot_bottom)) +
                ").";
    else if (c.spot)
        text += " Có Chỗ đánh quái: chỉ nhảy khi chỗ đứng, chỗ rơi xuống và con quái đều ở trong vùng đánh (cao "
                "thấp " + std::to_string(c.spot_half_h) + " px; vùng vẽ ở tab Bản đồ thì theo vùng đó).";
    hint(trial_hint(kTrialDrop, text.c_str()).c_str());
    if (!hunting_ || !hunter_) return;

    // While it hunts: off until the next start, or why the monster below is
    // not dropped to just now -- for what the player can act on.
    bool off = false;
    const std::string wait = drop_wait_vi(hs, map_known_now(), &off);
    if (wait.empty()) return;
    if (off) coloured(kWait, wait.c_str());
    else     hint(wait.c_str());
}

std::string GuiApp::drop_wait_vi(const Hunter::Status& hs, bool map_known_here, bool* off) {
    if (off != nullptr) *off = false;
    using Off = Hunter::DropOff;
    const char* off_vi = nullptr;
    switch (hs.drop_off) {
        case Off::Fails:    off_vi = "Nhảy xuống đã tắt tới lần BẮT ĐẦU sau: nhiều lần bấm Ctrl + ↓ không rơi xuống."; break;
        case Off::Rose:     off_vi = "Nhảy xuống đã tắt tới lần BẮT ĐẦU sau: bấm Ctrl + ↓ làm nhân vật nhảy lên."; break;
        case Off::OffMap:   off_vi = "Nhảy xuống đã tắt tới lần BẮT ĐẦU sau: nhân vật rơi xuống chỗ khác với bản đồ của game."; break;
        case Off::HeldLong: off_vi = "Nhảy xuống đã tắt tới lần BẮT ĐẦU sau: phím ↓ bị giữ quá lâu."; break;
        case Off::MapChanged:
        case Off::KeysStuck:
        case Off::None:
            break;
    }
    if (off_vi != nullptr) {
        if (off != nullptr) *off = true;
        return off_vi;
    }
    if (hs.drop_wait_ms == 0 || now_ms() - hs.drop_wait_ms > 1500) return {};
    using Why = Hunter::DropWhy;
    const bool naming = hs.drop_map.why == MapTrust::Why::Naming || hs.drop_map.why == MapTrust::Why::Ambiguous ||
                        hs.drop_map.why == MapTrust::Why::Confirming;
    const std::string count = std::to_string(hs.drop_map.points) + "/" + std::to_string(hs.drop_map.needed);
    std::string wait;
    switch (hs.drop_wait) {
        case Why::MapUnknown:
            // The Bản đồ tab knows the map; the drop's own check, apart, does
            // not yet.
            if (map_known_here) {
                wait = "Nhảy xuống: GoBot còn đang tự kiểm tra bản đồ cho việc nhảy";
                if (naming) wait += " (" + count + ")";
                wait += ".";
                break;
            }
            wait = "Nhảy xuống: chưa biết bản đồ này — qua cửa hoặc đăng nhập lại một lần khi GoBot đang mở";
            if (naming) wait += ", hoặc chờ GoBot nhận ra bản đồ từ chỗ nhân vật và quái đứng (" + count + ")";
            wait += ".";
            break;
        case Why::Gate:
            wait = "Nhảy xuống: cửa ở gần (" + std::to_string(hs.drop_wait_px) + " px).";
            break;
        case Why::PastMonster: wait = "Nhảy xuống: rơi từ đây sẽ xuống quá bậc của quái."; break;
        case Why::Bottom:      wait = "Nhảy xuống: đang đứng ở nền dưới cùng."; break;
        case Why::NoDropHere:  wait = "Nhảy xuống: chỗ này bấm Ctrl + ↓ không rơi."; break;
        case Why::OutsideArea: wait = "Nhảy xuống: sẽ ra khỏi vùng đánh."; break;
        case Why::Veto:        wait = "Nhảy xuống: đang chờ (máu thấp, bot tạm dừng, hoặc đang vào game)."; break;
        default:               break;
    }
    return wait;
}

// --------------------------------------------- "Nhảy lên bục trên"

namespace {

/// GoBot's own keys, each with what it does, as simple_drop_refusal_vi names
/// them: one on Ctrl or ↑ would take a jump's press for the player's.
struct OwnKeys {
    std::uint32_t vk[5];
    const char*   what[5];
};

/// Why the planner found no sure jump, in a few words for the card
/// (JumpNo); `px` the floor's rise above the character's.
std::string jump_no_vi(JumpNo no, int px) {
    switch (no) {
        case JumpNo::Short:
            return "bục cao " + std::to_string(px) + " px, Ctrl+↑ chỉ chắc tới " +
                   std::to_string(jump_peak(JumpKind::CtrlUp).usable()) + " px";
        case JumpNo::OffFloor:  return "chỗ đứng không nằm trên bục nào của file bản đồ";
        case JumpNo::Blocked:   return "có bục chặn ở phía trên";
        case JumpNo::Ambiguous: return "bục cao vừa tới mức nhảy lúc tới lúc không";
        case JumpNo::Ceiling:
            return "có bục ngoài vùng trong khoảng " + std::to_string(kJumpUpCeilingPx) + " px phía trên";
        case JumpNo::Edge:      return "mép bục ở ngay chỗ rơi xuống";
        case JumpNo::Outside:   return "chỗ nhảy, chỗ rơi xuống hay con quái ở ngoài vùng";
        case JumpNo::Weaker:    return "lỡ chỉ nhảy được kiểu thường thì sẽ rơi ra ngoài vùng";
        case JumpNo::Learned:   return "ở đây đã nhảy hỏng trong lần mở GoBot này";
        case JumpNo::Solid:     return "bục đó không nhảy xuyên lên được (đã rơi lại)";
        case JumpNo::Gate:      return "gần cửa";
        case JumpNo::Narrow:    return "chỗ đứng nhảy được quá hẹp";
        case JumpNo::Far:       return "lên được bục nhưng đi tới con quái sẽ ra khỏi vùng";
        case JumpNo::NotTargetFloor: return "nhảy lên được, nhưng không phải bục của con quái";
        case JumpNo::None:      break;
    }
    return "không có chỗ nhảy nào chắc chắn";
}

} // namespace

std::string GuiApp::simple_jump_up_refusal_vi() {
    // What the settings tell (jump_up_tick); the rest -- no area for this
    // map, the map not trusted -- only a hunt shows, and the card says it.
    const OwnKeys own{ { macro_config_.toggle_key, macro_config_.panic_key, hunt_config_.toggle_key, hotkeys_.trash_now,
                         hotkeys_.login_now },
                       { "bật/tắt bot", "dừng ngay", "bật/tắt đánh quái", "dọn túi ngay", "vào game ngay" } };
    std::size_t at = 0;
    switch (jump_up_tick(hunt_config_, own.vk, &at)) {
        case JumpUpTick::Ok:        return {};
        case JumpUpTick::StayPut:   return "Không nhảy lên khi Đứng yên một chỗ: bỏ dấu Đứng yên trước.";
        case JumpUpTick::NoPackets: return "Cần dấu Vị trí từ gói tin.";
        case JumpUpTick::NoSpot:    return "Cần dấu Chỗ đánh quái, với vùng đánh quái vẽ ở tab Bản đồ.";
        case JumpUpTick::JumpKey:
            return hunt_config_.jump_key == 0 ? std::string("Phím nhảy phải là ↑ (chưa đặt phím nhảy).")
                                              : "Phím nhảy phải là ↑ (đang là " + key_text(hunt_config_.jump_key) + ").";
        case JumpUpTick::OwnKey:
            return std::string("Phím ") + own.what[at < 5 ? at : 0] + " đang là " +
                   ((own.vk[at < 5 ? at : 0] & 0xFFu) == VK_UP ? "↑" : "Ctrl") + ": đổi phím khác trước.";
    }
    return {};
}

void GuiApp::simple_jump_up_tick() {
    // After "Nhảy xuống bậc dưới", beside it or under it when the row is full.
    const char* const kJumpUp = "Nhảy lên bục trên (↑ / ↑↑ / Ctrl+↑)";
    const float wide =
        ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(kJumpUp).x;
    ImGui::SameLine(0, 18);
    if (ImGui::GetContentRegionAvail().x < wide) ImGui::NewLine();
    // On by default (the owner's answer 5): greyed out, with its reason, while
    // it cannot work as set -- but a ticked one may still be unticked: a
    // GoBot key on Ctrl or ↑ greys it while the old hop stays off in an area.
    const std::string why = simple_jump_up_refusal_vi();
    ImGui::BeginDisabled(!why.empty() && !hunt_config_.jump_up);
    if (ImGui::Checkbox(kJumpUp, &hunt_config_.jump_up)) simple_changed();
    ImGui::EndDisabled();
    const std::string tip =
        !why.empty()
            ? why
            : trial_hint(kTrialJumpUp,
                         ("Nhảy thẳng lên bục phía trên để đánh quái trên đó và để về chỗ về. Chỉ nhảy khi bản đồ\n"
                          "cho thấy chắc chắn rơi đúng bục, trong vùng đánh quái. ↑, ↑↑ hay Ctrl + ↑: GoBot tự chọn\n"
                          "theo file bản đồ của game, có chừa hao (↑ tới khoảng " +
                          std::to_string(jump_peak(JumpKind::Up).usable()) + " px, ↑↑ tới " +
                          std::to_string(jump_peak(JumpKind::UpUp).usable()) + " px, Ctrl + ↑ tới " +
                          std::to_string(jump_peak(JumpKind::CtrlUp).usable()) +
                          " px).\n"
                          "Chỗ đứng nhảy, chỗ rơi xuống và con quái đều trong vùng vẽ ở tab Bản đồ, và không có bục\n"
                          "nào ngoài vùng trong khoảng " +
                          std::to_string(kJumpUpCeilingPx) +
                          " px phía trên. Đang bay mà quái vào tầm thì vẫn đánh.\n"
                          "Tối đa " +
                          std::to_string(Hunter::kJumpUpsPerMinute) + " lần mỗi phút, " +
                          std::to_string(Hunter::kJumpUpsPerTarget) + " lần cho một con quái; " +
                          std::to_string(Hunter::kJumpUpFailsInRow) + " lần liền hoặc " +
                          std::to_string(Hunter::kJumpUpFailsPerRun) +
                          " lần trong một lượt\n"
                          "không lên được bục thì tự tắt tới lần BẮT ĐẦU sau. Ctrl + ↑ đứng trên cửa không đổi bản\n"
                          "đồ; nhưng bản đồ mà đổi trong " +
                          std::to_string(Hunter::kPressWatchMs / 1000) +
                          " giây sau một lần nhảy thì GoBot dừng đánh quái và bỏ dấu\n"
                          "ô này. Bỏ dấu: GoBot không bấm gì mới, chỉ ghi vào log chỗ lẽ ra đã nhảy. GoBot không\n"
                          "bao giờ bấm ↓ một mình. Cần phím nhảy là ↑. Thử ↑ / ↑↑ / Ctrl + ↑ để đo, không bắt buộc.")
                             .c_str());
    ImGui::SetItemTooltip("%s", tip.c_str());
}

void GuiApp::simple_jump_up_hints() {
    // Not able to work as set: why, under the row too, when ticked and with
    // the spot ticked -- it ships ticked, and with no spot nobody needs it
    // said. Nor then the Thử row.
    const std::string why = simple_jump_up_refusal_vi();
    if (!why.empty()) {
        if (hunt_config_.jump_up && hunt_config_.spot)
            hint(trial_hint(kTrialJumpUp, ("Nhảy lên bục trên chưa chạy: " + why).c_str()).c_str());
        return;
    }
    if (hunt_config_.jump_up) {
        hint(trial_hint(kTrialJumpUp,
                        ("Nhảy lên bục trên: chỉ trong vùng vẽ ở tab Bản đồ, khi bản đồ cho thấy chắc chắn rơi đúng "
                         "bục (↑ tới khoảng " +
                         std::to_string(jump_peak(JumpKind::Up).usable()) + " px, ↑↑ tới " +
                         std::to_string(jump_peak(JumpKind::UpUp).usable()) + " px, Ctrl + ↑ tới " +
                         std::to_string(jump_peak(JumpKind::CtrlUp).usable()) + " px); tối đa " +
                         std::to_string(Hunter::kJumpUpsPerMinute) + " lần mỗi phút.")
                            .c_str())
                 .c_str());
        // Ctrl + ↑ alone left out: the rest still go.
        if (jump_up_no_ctrl(hunt_config_))
            coloured(kWait, "Phím đánh hoặc phím đi đang là Ctrl nên GoBot không dùng Ctrl+↑ (↑ và ↑↑ vẫn dùng).");
    }

    // "Thử ↑", "Thử ↑↑", "Thử Ctrl + ↑": one jump of each kind, its flight
    // measured -- where the jumps up can work as set, ticked or not (a jump
    // that changed the map unticks it, and a measure may be wanted then).
    // They help measure; nothing waits for them.
    StyleUi& ui = style_ui();
    const bool busy = ui.kind.valid() || ui.jump.valid();
    std::string wait;
    if (busy)                           wait = "Đang thử…";
    else if (!input_.window_valid())    wait = "Chưa tìm thấy game.";
    else if (simple_running())          wait = "Bot đang chạy: dừng bot rồi hãy thử.";
    else if (keeper_entering())         wait = "Đang vào game — thử lại sau.";
    else if (hunt_config_.packet_positions == PacketPositions::Off)
        wait = "Cần Vị trí từ gói tin: chỉ gói tin cho biết nhân vật nhảy kiểu gì.";
    else if (coord_.critical_active())
        wait = "Bot đang tạm dừng (" + coord_.critical_reason() + ") — thử lại sau.";
    ImGui::PushFont(nullptr, kSmall);
    label("Đo cú nhảy:");
    ImGui::BeginDisabled(!wait.empty());
    for (const JumpKind k : { JumpKind::Up, JumpKind::UpUp, JumpKind::CtrlUp }) {
        ImGui::SameLine();
        const std::string text = std::string("Thử ") + jump_kind_key(k) + "###jumpkind" +
                                 std::to_string(static_cast<int>(k));
        if (ImGui::SmallButton(text.c_str())) simple_jump_kind_try(k);
        ImGui::SetItemTooltip("%s", !wait.empty()
                                        ? wait.c_str()
                                        : "Nhảy MỘT lần kiểu này trong game và đo theo gói tin: có nhảy đúng kiểu "
                                          "không, bay bao lâu, rơi xuống ở đâu. Đứng ở mặt đất phẳng, không có bục "
                                          "phía trên. Không bắt buộc: chỉ để đo.");
    }
    ImGui::EndDisabled();
    ImGui::PopFont();
    if (!ui.kind_said.empty()) hint(ui.kind_said.c_str());
}

void GuiApp::simple_jump_kind_try(JumpKind kind) {
    StyleUi& ui = style_ui();
    if (ui.kind.valid() || ui.jump.valid()) return;
    // Never into the login pages, a chat line or the bin's dialog: as "Thử".
    if (keeper_entering()) {
        simple_note("Đang vào game — thử lại sau.");
        return;
    }
    if (!keys_may_start()) return;
    // The map the Bản đồ tab has read, if any: the floor above is looked for
    // on the check's own thread, never here.
    const std::shared_ptr<const GameMapBook::Page> page = map_page_now();
    ui.kind_stop = false;
    ui.kind_said = std::string("Đang thử ") + jump_kind_key(kind) + "…";
    GB_INFO("Jump test: one %s, to see how the character jumps (by the packets' take-offs by kind)",
            jump_kind_name(kind));
    std::atomic<bool>* stop = &ui.kind_stop;
    ui.kind = std::async(std::launch::async, [this, kind, stop, page] {
        JumpCheck check(coord_);
        JumpCheck::Eyes eyes;
        // Your client's take-offs by kind and your own last reports, under
        // packet_takeoffs' guards.
        eyes.air = [this] { return packet_air_counts(); };
        eyes.own_since = [this](std::uint64_t ms) { return packet_own_since(ms); };
        // A floor the map shows above where you stand, within the ceiling:
        // the tab's page, or -- with none -- the map entry's own file, read
        // here, on the check's thread.
        eyes.floor_above = [this, page]() -> std::optional<int> {
            const std::uint64_t at = now_ms();
            PacketLink link;
            link.capturing = capture_.running();
            link.npcap = link.capturing || position_npcap_ok_.load();
            link.last_tcp_ms = capture_.stats().last_tcp_ms;
            const WorldFix f =
                world_fix(packet_world_.view(at, PacketWorld::kOwnStepReckonMs), packet_world_.stats(), link, at);
            if (f.state != WorldFix::State::Known) return std::nullopt;
            std::shared_ptr<const GameMapBook::Page> p = page;
            if (!p && f.map_known) p = game_maps_.get(GameMapBook::stem_of(f.map, f.area));
            if (!p) return std::nullopt;
            return floor_above(*p, MapPoint{ static_cast<int>(std::lround(f.rx)), static_cast<int>(std::lround(f.ry)) });
        };
        check.set_eyes(eyes);
        JumpCheck::Hands hands;
        hands.focus = [this] { return input_.window_focused() || input_.focus_window(); };
        hands.focused = [this] { return input_.window_focused(); };
        // Always ↑, by the input's own presses: never a key of the settings.
        hands.jump = [this](JumpKind k, const std::function<bool()>& go_on) {
            return JumpCheck::press_with(input_, k, go_on);
        };
        check.set_hands(hands);
        return check.run_kind(kind, stop);
    });
}

std::vector<std::pair<bool, std::string>> GuiApp::jump_up_card_vi(const Hunter::Status& hs) {
    // Each line, and whether it is said in red: off until the next start.
    std::vector<std::pair<bool, std::string>> lines;
    const JumpUpCard card = jump_up_card(hunt_config_, hs, now_ms());
    const std::string off = "Nhảy lên đã tắt tới lần BẮT ĐẦU sau: ";
    using Off = Hunter::JumpUpOff;
    switch (card.off) {
        case Off::Fails:
            lines.push_back({ true, off + std::to_string(Hunter::kJumpUpFailsInRow) + " lần liền (hoặc " +
                                        std::to_string(Hunter::kJumpUpFailsPerRun) +
                                        " lần trong lượt này) không lên được bục." });
            break;
        case Off::Physics:
            lines.push_back({ true, off + "nhân vật nhảy khác với GoBot tính. Hãy gửi file log." });
            break;
        case Off::OffMap:
            lines.push_back({ true, off + "nhân vật rơi xuống chỗ khác với bản đồ của game." });
            break;
        case Off::HeldLong:
            lines.push_back({ true, off + "phím ↑ bị giữ quá lâu." });
            break;
        case Off::None:
        case Off::MapChanged:
        case Off::KeysStuck:
            break;
    }
    for (const JumpKind k : { JumpKind::Up, JumpKind::UpUp, JumpKind::CtrlUp }) {
        if ((card.kinds_off & (1u << static_cast<unsigned>(k))) == 0) continue;
        const char* what = k == JumpKind::CtrlUp ? "game không nhảy cao"
                           : k == JumpKind::UpUp ? "nhân vật không nhảy thêm trên không"
                                                 : "nhân vật không nhảy";
        lines.push_back({ true, std::string(jump_kind_key(k)) + " đã tắt tới lần BẮT ĐẦU sau: " +
                                    std::to_string(Hunter::kJumpUpKindMisses) + " lần bấm mà " + what + "." });
    }
    const std::string key = jump_kind_key(hs.jump_kind);
    const std::string x = std::to_string(hs.jump_x);
    const char* const toward = hs.jump_for == Hunter::JumpUpFor::Home   ? " để về chỗ về"
                               : hs.jump_for == Hunter::JumpUpFor::Seek ? " tới con quái đang tìm"
                                                                        : "";
    switch (card.now) {
        case JumpUpCard::Now::Approach:
            lines.push_back({ false, "Nhảy lên: đang đi tới x " + x + " để nhảy " + key + toward + "." });
            break;
        case JumpUpCard::Now::Settle:
            lines.push_back({ false, "Nhảy lên: đang chờ nhân vật đứng yên ở x " + x + " để nhảy " + key + "." });
            break;
        case JumpUpCard::Now::Air:
            lines.push_back({ false, "Nhảy lên: đang ở trên không (" + key + ", lên bục " +
                                         std::to_string(hs.jump_land_top) + ")." });
            break;
        case JumpUpCard::Now::NoPlan: {
            const std::string who = hs.jump_wait_name.empty() ? std::string("con quái") : hs.jump_wait_name;
            lines.push_back({ false, "Nhảy lên: không có chỗ nhảy chắc chắn tới bục của " + who + " (" +
                                         jump_no_vi(hs.jump_wait_no, hs.jump_wait_px) + ")." });
            break;
        }
        case JumpUpCard::Now::None:
            break;
    }
    if (card.would)
        lines.push_back({ false, "Nhảy lên (đang tắt): lẽ ra đã nhảy " + std::to_string(hs.jumps_would) +
                                     " lần — xem log." });
    if (card.counts)
        lines.push_back({ false, "Nhảy lên: " + std::to_string(hs.jumps) + " lần, " +
                                     std::to_string(hs.jumps_worked) + " lần lên tới bục, " +
                                     std::to_string(hs.jumps_fell_back) + " lần rơi lại." });
    return lines;
}

void GuiApp::simple_jump_up_state(const Hunter::Status& hs) {
    // The red ones in red; the rest grey, the dry run's among them.
    for (const auto& [red, text] : jump_up_card_vi(hs)) {
        if (red) coloured(kBad, text.c_str());
        else     hint(text.c_str());
    }
}

// --------------------------------------------- "Tự tìm quái trong vùng"

std::string GuiApp::simple_seek_refusal_vi() {
    switch (seek_tick(hunt_config_)) {
        case SeekTick::Ok:        return {};
        case SeekTick::StayPut:   return "Không tự tìm quái khi Đứng yên một chỗ: bỏ dấu Đứng yên trước.";
        case SeekTick::NoPackets: return "Cần dấu Vị trí từ gói tin.";
        case SeekTick::NoSpot:    return "Cần dấu Chỗ đánh quái, với vùng đánh quái vẽ ở tab Bản đồ.";
    }
    return {};
}

void GuiApp::simple_seek_tick() {
    // After "Chỗ đánh quái", beside it or under it when the row is full.
    const char* const kSeek = "Tự tìm quái trong vùng";
    const float wide = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(kSeek).x;
    ImGui::SameLine(0, 18);
    if (ImGui::GetContentRegionAvail().x < wide) ImGui::NewLine();
    // On by default (the seek's Q7 (a)): greyed out, with its reason, while
    // it cannot work as set.
    const std::string why = simple_seek_refusal_vi();
    ImGui::BeginDisabled(!why.empty());
    if (ImGui::Checkbox(kSeek, &hunt_config_.seek_in_zone)) simple_changed();
    ImGui::EndDisabled();
    const std::string tip =
        !why.empty()
            ? why
            : trial_hint(kTrialSeek,
                         "Đi tới quái ở bất kỳ chỗ nào trong vùng vẽ ở tab Bản đồ mà GoBot đi tới được (đi bộ, rơi\n"
                         "xuống, nhảy lên), kể cả quái chưa thấy trên màn hình; không bao giờ ra khỏi vùng. Đánh con\n"
                         "trong tầm trước, rồi con đi bộ trên bục đang đứng tới được, rồi con ở bục dưới, cuối cùng\n"
                         "con phải nhảy lên mới tới. Trong vùng không còn con nào tới được thì đứng chờ tại chỗ,\n"
                         "không đi về chỗ về; bị đẩy ra ngoài vùng thì đi về như trước. Bỏ dấu: như bản 2026.11.05.");
    ImGui::SetItemTooltip("%s", tip.c_str());
}

std::string GuiApp::seek_going_vi(const Hunter::Status& hs) {
    const std::string mob = std::to_string(hs.seek_mob_x) + "," + std::to_string(hs.seek_mob_y);
    std::string s = "Đang đi tìm quái trong vùng: ";
    if (hs.seek_planned && hs.seek_leg == 'J') {
        s += std::string("nhảy ") + jump_kind_key(hs.seek_jump_kind) + " ở x " + std::to_string(hs.seek_leg_x) +
             " lên bục " + std::to_string(hs.seek_jump_land_top) + " rồi đi tới con ở " + mob;
    } else if (hs.seek_planned && hs.seek_leg == 'F') {
        s += "đi ra mép bục ở x " + std::to_string(hs.seek_leg_x) + " rồi rơi xuống, tới con ở " + mob;
    } else {
        s += "con ở " + mob;
        const int dx = hs.seek_mob_x - hs.seek_you_x;
        if (dx != 0) s += std::string(" (bên ") + (dx < 0 ? "trái " : "phải ") + std::to_string(std::abs(dx)) + " px)";
    }
    // Projected: a walker out of view, followed on along its floor.
    if (hs.seek_projected) s += " — chỗ đoán: con quái đi khuất, GoBot đoán nó đi tiếp dọc bục";
    return s + ".";
}

void GuiApp::simple_seek_none(const Hunter::Status& hs) {
    coloured(kWait, hs.seek_none_why == Hunter::SeekNone::Empty
                        ? "Trong vùng không có con quái nào — đứng chờ ở đây."
                        : "Trong vùng không còn con quái nào tới được — đứng chờ ở đây.");
    // Some above that only a jump up gets to: what would get there.
    switch (seek_above(hunt_config_, hs)) {
        case SeekAbove::TickOff:
            hint((std::to_string(hs.seek_above_no_way) +
                  " con ở bục trên: chưa nhảy lên được — bật “Nhảy lên bục trên”.")
                     .c_str());
            break;
        case SeekAbove::Latched:
            hint((std::to_string(hs.seek_above_no_way) +
                  " con ở bục trên: chưa nhảy lên được — Nhảy lên đã tắt tới lần BẮT ĐẦU sau.")
                     .c_str());
            break;
        case SeekAbove::None:
            break;
    }
    if (hs.seek_passed > 0)
        hint((std::to_string(hs.seek_passed) + " con đang tạm bỏ qua (không tới được, hoặc tới nơi không thấy).")
                 .c_str());
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
                      " bước về một phía rồi đứng đánh.";
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
    ImGui::SetItemTooltip("Bot không thấy mép bục, nên lùi có giới hạn: lùi đủ số bước này về một phía "
                          "thì đứng đánh tại chỗ, tới khi nhân vật đi bộ về phía ngược lại mới lùi tiếp "
                          "(1–10).");
    ImGui::SameLine();
    label("bước về một phía");
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

    // The hunting spot's area, and how far off it the character walks back:
    // in the packets' world pixels, clamped as hunt.ini's are. The area is
    // the spot's own from the moment it is taken; the walk back's distance is
    // read at every pass.
    ImGui::Dummy(ImVec2(0, 6));
    heading(strong_font_, "Chỗ đánh quái");
    int half_w = hunt_config_.spot_half_w;
    int half_h = hunt_config_.spot_half_h;
    int tolerance = hunt_config_.spot_tolerance;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Vùng đánh: ngang ±");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputInt("##spot_w", &half_w, 10, 50)) {
        hunt_config_.spot_half_w = std::clamp(half_w, 50, 2000);
        changed = true;
    }
    ImGui::SetItemTooltip("Bot chỉ đi tới quái cách chỗ đánh quái không quá số này về mỗi bên (50–2000). Quái "
                          "trong tầm đánh thì vẫn đánh, dù ở ngoài vùng. (Bản đồ có vùng vẽ trên tab Bản đồ: dùng "
                          "vùng đó.)");
    ImGui::SameLine();
    label("dọc ±");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputInt("##spot_h", &half_h, 10, 50)) {
        hunt_config_.spot_half_h = std::clamp(half_h, 20, 500);
        changed = true;
    }
    ImGui::SetItemTooltip("Và cao hơn hay thấp hơn chỗ đánh quái không quá số này (20–500). (Bản đồ có vùng vẽ trên "
                          "tab Bản đồ: dùng vùng đó.)");
    ImGui::SameLine();
    label("px");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Đi về khi cách chỗ hơn");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputInt("##spot_tolerance", &tolerance, 1, 10)) {
        hunt_config_.spot_tolerance = std::clamp(tolerance, 10, 1000);
        changed = true;
    }
    ImGui::SetItemTooltip("Hết quái mà nhân vật cách chỗ đánh quái (theo chiều ngang) hơn số này thì bot đi bộ "
                          "về chỗ (10–1000). Bằng hay lớn hơn số ngang của vùng đánh: chỉ đi về khi bị đẩy ra "
                          "ngoài vùng. (Bản đồ có vùng vẽ trên tab Bản đồ: tính từ chỗ về của vùng đó.)");
    ImGui::SameLine();
    label("px");
    hint(trial_hint(kTrialSpot, "Cỡ vùng mới (vùng vẽ trên tab Bản đồ giữ cỡ của nó): đo bằng điểm ảnh của bản đồ "
                                "trong gói tin của game. Dùng từ chỗ đặt sau (lần bắt đầu sau, hoặc khi bấm Đặt chỗ "
                                "này), và là cỡ của vùng tạo mới trên tab Bản đồ; số Đi về dùng ngay.")
             .c_str());

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
        // Not the jump key itself, nor the jumps up's own ↑ (game_keys_in_use).
        for (NamedKey& k : game_keys_in_use())
            if (k.what_vi != "nhảy" && k.what_vi != kJumpUpKeyVi) ui.keys.push_back(std::move(k));
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
        case JumpCheck::Advice::DownChangesMap:
            m = Mark::Bad;
            line = "Phím nhảy đang là ↓";
            help = "Đứng trên cửa mà bấm ↓ là sang bản đồ khác, nên GoBot không bao giờ bấm ↓ một mình: bot sẽ "
                   "không nhảy. Phím nhảy trong game thường là ↑.";
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
                help = "Đứng chỗ trống, rồi bấm Thử ở dòng Tự đánh quái.";
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
    // Started here it only looks, and it is stopped again afterwards unless
    // something else looks through it by then (simple_style_tick). Kept
    // meanwhile, by a hunt's end as well (jump_test_running).
    if (!detector_running_) {
        start_detector();
        if (!detector_running_) {
            simple_note("Không đọc được màn hình game nên chưa thử phím nhảy.");
            return;
        }
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
        // Your client's own take-offs, whenever the packets can tell: they
        // decide, and the plate is watched beside them.
        eyes.takeoffs = [this] { return packet_takeoffs(); };
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
    // A "Thử ↑", "Thử ↑↑" or "Thử Ctrl + ↑" that has finished: what it saw,
    // measured, in the log and under the row.
    if (ui.kind.valid() && ui.kind.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const JumpCheck::KindResult r = ui.kind.get();
        ui.kind_said = JumpCheck::kind_said_vi(r);
        GB_INFO("Jump test: %s %s -- %s (packets: jumps %d, walk-offs %d, second %d, high %d, landed %d; take-off "
                "%d ms after the key, flight %d ms, rose %d px, g %s; Up held %u ms)",
                r.pressed ? "pressed" : "did not press", jump_kind_name(r.kind),
                JumpCheck::kind_outcome_name(r.outcome), r.jumps, r.walkoffs, r.second, r.high, r.landed,
                r.takeoff_ms, r.flight_ms, r.rose_px,
                r.gravity ? std::to_string(static_cast<int>(std::lround(*r.gravity))).c_str() : "not measured",
                static_cast<unsigned>(r.held_ms));
        simple_note(ui.kind_said, 12000);
    }
    if (!ui.jump.valid() || ui.jump.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    const JumpCheck::Result r = ui.jump.get();
    ui.jump_said = JumpCheck::said_vi(r, key_text(ui.jump_key));
    if (r.by_packets) {
        // Decided by your client's take-offs: the plate only beside them.
        const bool followed =
            r.outcome == JumpCheck::Outcome::Jumped && r.reads > 0 && r.rose_px < JumpCheck::kStillPx;
        GB_INFO("Jump test: %s %s -- %s (packets: %d take-off%s; the plate rose %d px over %d reads%s)",
                r.pressed ? "pressed" : "did not press", InputManager::key_name(ui.jump_key).c_str(),
                JumpCheck::outcome_name(r.outcome), r.takeoffs, r.takeoffs == 1 ? "" : "s", r.rose_px, r.reads,
                followed ? " -- the camera followed" : "");
    } else {
        GB_INFO("Jump test: %s %s -- %s; the plate rose %d px over %d reads",
                r.pressed ? "pressed" : "did not press", InputManager::key_name(ui.jump_key).c_str(),
                JumpCheck::outcome_name(r.outcome), r.rose_px, r.reads);
    }

    // The detector stopped again unless something else looks through it:
    // the test started it, or kept it past a hunt's end (release_detector --
    // a hunt, a walk, "Quái đang thấy" and the full screen's Radar keep it).
    release_detector();

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
    if (!style_ui_) return;
    style_ui_->jump_stop = true;
    style_ui_->kind_stop = true;
}

} // namespace gobot
