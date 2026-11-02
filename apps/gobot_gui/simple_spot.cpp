// "Chỗ đánh quái", the hunting spot, on the Đánh quái tab: its tick and
// "Đặt chỗ này" on the "Tự đánh quái" row, its line under the row, Npcap's
// line on the checklist while it is ticked, the card's line under "Vị trí:",
// why fighting with it ticked does not start, and its part of the 10-s
// "Positions" line in gobot_gui.log.
//
// The spot is the hunter's (HunterConfig::spot, Hunter::Spot): taken where
// the character stands, settled on the ground, at the first look the game's
// packets place it -- in their world pixels -- and kept for the run only:
// nothing of it is saved yet. What it does in a fight is the hunter's; the
// area's numbers are in the "Tầm đánh" window (simple_style.cpp). Here is
// only what the window draws and asks of the hunter. In a file of its own so
// that simple_view.cpp gets only the calls.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/common.hpp"
#include "gobot/hunter.hpp"
#include "gobot/logger.hpp"
#include "gobot/packet_capture.hpp"

#include "imgui.h"

#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace gobot {
namespace {

using namespace simple_ui;

/// How often the checklist looks at Npcap's service again while the spot is
/// ticked: Npcap installed, or its service started, is seen this soon after.
constexpr std::uint64_t kNpcapLookMs = 5'000;

/// World pixels as whole ones, for the screen and the log.
std::string px(float v) { return std::to_string(std::lround(v)); }

/// Whether the hunter's status has the character placed against the spot
/// (Status::spot_dx, spot_dy): a spot set, not suspended, and the packets
/// knowing where the character is -- which every reason but these has.
bool placed(const Hunter::Status& hs) {
    using Why = Hunter::SpotWhy;
    return hs.spot_set && !hs.spot_suspended &&
           (hs.spot_why == Why::None || hs.spot_why == Why::TooHigh || hs.spot_why == Why::Retrying ||
            hs.spot_why == Why::GaveUp);
}

/// SpotWhy in the log's English.
const char* spot_why_name(Hunter::SpotWhy why) {
    switch (why) {
        case Hunter::SpotWhy::None:         return "in use";
        case Hunter::SpotWhy::WaitingFirst: return "no report of yours yet, hunting as without a spot";
        case Hunter::SpotWhy::Settling:     return "waiting for the character to stand still on the ground";
        case Hunter::SpotWhy::NoFix:        return "no capture to place the character, standing";
        case Hunter::SpotWhy::Paused:       return "the packets lost the character, standing";
        case Hunter::SpotWhy::Suspended:    return "a map entry or a log in since it was set, standing";
        case Hunter::SpotWhy::TooHigh:      return "more than one jump above, not walked back to";
        case Hunter::SpotWhy::Retrying:     return "a walk back got nowhere, tried again later";
        case Hunter::SpotWhy::GaveUp:       return "walks back got nowhere, no more tries";
    }
    return "?";
}

} // namespace

// ------------------------------------------------------------- the row

void GuiApp::simple_spot_tick() {
    // Beside "Vị trí từ gói tin", or under it when the row is full, as that
    // one sits beside "Giữ khoảng cách".
    const char* const kSpot = "Chỗ đánh quái";
    const float wide = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                       ImGui::CalcTextSize(kSpot).x;
    ImGui::SameLine(0, 18);
    if (ImGui::GetContentRegionAvail().x < wide) ImGui::NewLine();
    if (ImGui::Checkbox(kSpot, &hunt_config_.spot)) simple_changed();
    // Marked as on trial ticked or not, as the ticks beside it are.
    const std::string tip = trial_hint(
        kTrialSpot,
        "Chỉ đi tới quái trong một vùng quanh chỗ đánh quái: chỗ nhân vật đứng yên trên mặt đất\n"
        "lần đầu gói tin của game báo được vị trí sau khi bắt đầu (sau bước đi hay kỹ năng đầu tiên).\n"
        "Quái trong tầm đánh thì vẫn đánh, dù ở ngoài vùng. Bị đẩy ra xa mà hết quái thì bot tự\n"
        "đi bộ về chỗ. Không nhảy theo quái, không bao giờ bấm ↓: chỗ thấp hơn chỉ về được khi\n"
        "bục cho nhân vật đi rơi xuống từ mép bục.\n"
        "Cần Npcap: vị trí lấy từ gói tin của game, có dấu \"Vị trí từ gói tin\" hay không.\n"
        "Bản này chỉ nhớ chỗ trong lần chạy này: mỗi lần bấm BẮT ĐẦU lấy chỗ mới.\n"
        "Đổi bản đồ thì bot đứng yên tới khi nhân vật qua cửa quay về gần chỗ đã rời đi.\n"
        "Vào lại game (Tự đăng nhập hay Tìm lại) thì bot đứng yên tới khi nhân vật đứng yên cách chỗ\n"
        "đánh quái, hoặc chỗ nó đứng trước khi vào lại, không quá 150 px, cùng bản đồ (sau bước đi\n"
        "hay kỹ năng đầu tiên, để gói tin báo chỗ đứng). Không thì bấm Đặt chỗ này.\n"
        "Muốn đưa nhân vật sang chỗ khác khi bot đang chạy: bỏ dấu Chỗ đánh quái, đi tới chỗ mới,\n"
        "đánh dấu lại rồi bấm Đặt chỗ này.\n"
        "Vùng đánh và khi nào đi về: nút Tầm đánh….");
    ImGui::SetItemTooltip("%s", tip.c_str());
    if (!hunt_config_.spot) return;

    // "Đặt chỗ này": the hunter takes the spot again where the character
    // stands, at its next look that has it settled on the ground. Only while
    // it hunts: otherwise there is nothing to ask, and the next start takes a
    // spot afresh anyway.
    ImGui::SameLine();
    const bool can = hunting_ && hunter_ != nullptr;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Đặt chỗ này")) {
        hunter_->retake_spot();
        simple_note("Đặt chỗ đánh quái ở chỗ nhân vật đang đứng, lúc nhân vật đứng yên trên mặt đất.");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", can ? "Đặt chỗ đánh quái mới ở chỗ nhân vật đang đứng: khi đã sang bản đồ khác, "
                                      "vào lại game, hay muốn đánh ở chỗ khác."
                                    : "Chỉ dùng được khi đang đánh quái.");
}

void GuiApp::simple_spot_hint() {
    if (!hunt_config_.spot) return;
    const std::string text = "Chỗ đánh quái: chỉ đi tới quái cách chỗ không quá " +
                             std::to_string(hunt_config_.spot_half_w) + " px ngang và " +
                             std::to_string(hunt_config_.spot_half_h) + " px dọc; hết quái thì đi về khi cách " +
                             "chỗ hơn " + std::to_string(hunt_config_.spot_tolerance) +
                             " px. Chỗ lấy khi bắt đầu đánh, chỉ nhớ trong lần chạy này.";
    hint(trial_hint(kTrialSpot, text.c_str()).c_str());
}

// ------------------------------------------------------- the checklist

void GuiApp::simple_spot_checklist() {
    // Only where it matters: fighting and the spot both ticked. A capture
    // running is Npcap working, whoever started it; otherwise its service is
    // looked at every kNpcapLookMs, not on every frame.
    if (!hunt_config_.enabled || !hunt_config_.spot) return;
    const std::uint64_t now = now_ms();
    if (capture_.running()) {
        spot_npcap_ok_ = true;
    } else if (spot_npcap_ms_ == 0 || now - spot_npcap_ms_ >= kNpcapLookMs) {
        spot_npcap_ms_ = now;
        spot_npcap_ok_ = PacketCapture::npcap_available();
    }
    const bool off = hunt_config_.packet_positions == PacketPositions::Off;
    mark(off || !spot_npcap_ok_ ? Mark::Bad : Mark::Good);
    ImGui::SameLine();
    label(off               ? "Chỗ đánh quái: vị trí từ gói tin đang tắt"
          : spot_npcap_ok_ ? "Có Npcap (Chỗ đánh quái cần nó)"
                           : "Chưa có Npcap: Chỗ đánh quái cần nó");
    if (!off && spot_npcap_ok_) return;
    ImGui::Indent(mark_indent());
    hint(off ? "packet_positions=off trong hunt.ini (hoặc Positions from packets: Off ở màn hình Nâng cao). Đánh "
               "dấu \"Vị trí từ gói tin\", hoặc bỏ dấu Chỗ đánh quái; không thì Tự đánh quái không chạy."
             : "Npcap chưa cài, hoặc dịch vụ Npcap chưa chạy (máy từng chạy GoC thì thường đã có). Cài Npcap, "
               "hoặc bỏ dấu Chỗ đánh quái; không thì Tự đánh quái không chạy.");
    ImGui::Unindent(mark_indent());
}

// ------------------------------------------------------------ the card

void GuiApp::simple_spot_state(const Hunter::Status& hs) {
    if (!hunt_config_.spot) return;
    using Why = Hunter::SpotWhy;
    const std::string off = px(std::abs(hs.spot_dx));
    switch (hs.spot_why) {
        case Why::None: {
            // In use, or ticked a moment ago and not looked at yet.
            if (!hs.spot_set) {
                hint("Chỗ đánh quái: chưa đặt.");
                break;
            }
            std::string line = "Chỗ đánh quái: " + px(hs.spot_x) + ", " + px(hs.spot_y) + " (vùng ±" +
                               std::to_string(hs.spot_half_w) + " / ±" + std::to_string(hs.spot_half_h) +
                               " px); nhân vật cách chỗ " + off + " px";
            // Height only past what the walk back takes for level ground.
            if (hs.spot_dy < -static_cast<float>(Hunter::kSpotFloorPx))
                line += ", chỗ cao hơn " + px(-hs.spot_dy) + " px";
            else if (hs.spot_dy > static_cast<float>(Hunter::kSpotFloorPx))
                line += ", chỗ thấp hơn " + px(hs.spot_dy) + " px";
            if (hs.returns > 0) line += "; đã đi về " + std::to_string(hs.returns) + " lần";
            coloured(kGood, (line + ".").c_str());
            break;
        }
        case Why::WaitingFirst:
            hint("Chỗ đánh quái: chờ nhân vật đi một bước hay dùng một kỹ năng để gói tin báo vị trí; tới "
                 "lúc đó bot đánh như thường.");
            break;
        case Why::Settling:
            hint("Chỗ đánh quái: chờ nhân vật đứng yên trên mặt đất để đặt chỗ; tới lúc đó bot đứng yên, chỉ "
                 "đánh quái tới trong tầm.");
            break;
        case Why::NoFix:
            coloured(kWait, "Chỗ đánh quái: chưa biết nhân vật ở đâu (chưa có gói tin của game) — bot đứng yên, "
                            "chỉ đánh quái tới trong tầm.");
            break;
        case Why::Paused:
            coloured(kWait, "Chỗ đánh quái: gói tin tạm không biết nhân vật ở đâu — bot đứng yên, chỉ đánh "
                            "quái tới trong tầm, tới khi biết lại.");
            break;
        case Why::Suspended:
            // A log in (the keeper, Tìm lại) or a door: each comes back its
            // own way (Hunter::Spot::login).
            if (hs.spot_login)
                coloured(kWait, "Chỗ đánh quái: vừa vào lại game — bot đứng yên, chỉ đánh quái tới trong tầm, "
                                "tới khi nhân vật đứng yên cách chỗ đánh quái (hoặc chỗ nó đứng trước khi vào "
                                "lại) không quá 150 px. Đi một bước hay dùng một kỹ năng để gói tin báo chỗ "
                                "đứng. Muốn đánh ở đây thì bấm Đặt chỗ này.");
            else
                coloured(kWait, "Chỗ đánh quái: nhân vật đã sang bản đồ khác — bot đứng yên, chỉ đánh quái tới "
                                "trong tầm, tới khi nhân vật qua cửa quay về gần chỗ đã rời đi; muốn đánh ở đây "
                                "thì bấm Đặt chỗ này.");
            break;
        case Why::TooHigh:
            coloured(kWait, ("Chỗ đánh quái cao hơn " + px(-hs.spot_dy) +
                             " px, quá một lần nhảy: bot không đi về được, đánh ở đây. Tự đưa nhân vật về, "
                             "hoặc bấm Đặt chỗ này.").c_str());
            break;
        case Why::Retrying:
            coloured(kWait, ("Chỗ đánh quái: lần đi về vừa rồi không tới được (vướng tường hay bục?), lát nữa "
                             "thử lại. Nhân vật cách chỗ " + off + " px.").c_str());
            break;
        case Why::GaveUp:
            coloured(kBad, ("Chỗ đánh quái: đi về " + std::to_string(Hunter::kSpotMaxTries) +
                            " lần liền không tới được nên thôi. Tự đưa nhân vật về gần chỗ, hoặc bấm Đặt "
                            "chỗ này.").c_str());
            break;
    }
}

// ------------------------------------------------------------ the start

const char* GuiApp::spot_refusal(std::string& vi) {
    vi.clear();
    if (!hunt_config_.spot) return nullptr;
    if (hunt_config_.packet_positions == PacketPositions::Off) {
        vi = "Chỗ đánh quái cần vị trí từ gói tin, mà nó đang tắt (packet_positions=off trong hunt.ini, hoặc "
             "Positions from packets: Off ở màn hình Nâng cao). Đánh dấu \"Vị trí từ gói tin\", hoặc bỏ dấu "
             "Chỗ đánh quái.";
        return "positions from packets are off (packet_positions=off)";
    }
    // A capture running is Npcap working, whoever started it.
    if (capture_.running()) return nullptr;
    spot_npcap_ms_ = now_ms();
    spot_npcap_ok_ = PacketCapture::npcap_available();
    if (!spot_npcap_ok_) {
        vi = "Chỗ đánh quái cần Npcap, mà Npcap chưa cài hoặc dịch vụ Npcap chưa chạy. Cài Npcap rồi bấm BẮT "
             "ĐẦU lại, hoặc bỏ dấu Chỗ đánh quái.";
        return "Npcap is not installed, or its service is not running";
    }
    return nullptr;
}

// ------------------------------------------------------------- the log

std::string GuiApp::spot_positions_text() {
    if (!hunter_) return "no hunter";
    const Hunter::Status hs = hunter_->status();
    std::ostringstream out;
    if (!hs.spot_set) {
        out << "no spot";
    } else if (hs.spot_suspended) {
        out << "spot suspended by a " << (hs.spot_login ? "log in" : "map entry") << " (set at " << px(hs.spot_x)
            << "," << px(hs.spot_y) << ")";
    } else {
        out << "spot at " << px(hs.spot_x) << "," << px(hs.spot_y) << " (+-" << hs.spot_half_w << " / +-"
            << hs.spot_half_h << ")";
        if (placed(hs))
            out << ", you " << px(std::abs(hs.spot_dx)) << " px off (it is " << px(std::abs(hs.spot_dy)) << " px "
                << (hs.spot_dy < 0.0f ? "up" : "down") << ")";
    }
    out << ", returns " << hs.returns << ", given up " << hs.return_give_ups;
    if (hs.spot_why != Hunter::SpotWhy::None) out << " -- " << spot_why_name(hs.spot_why);
    return out.str();
}

} // namespace gobot
