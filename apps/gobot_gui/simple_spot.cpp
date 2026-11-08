// "Chỗ đánh quái", the hunting spot, on the Đánh quái tab: its tick and
// "Đặt chỗ này" on the "Tự đánh quái" row, its line under the row, Npcap's
// line on the checklist while it is ticked, the card's line under "Vị trí:",
// why fighting with it ticked does not start, and its part of the 10-s
// "Positions" line in gobot_gui.log.
//
// The spot is the hunter's (HunterConfig::spot, Hunter::Spot): taken where
// the character stands, settled on the ground, at the first look the game's
// packets place it -- in their world pixels -- and kept for the run only; or,
// on a map with a hunting zone drawn on the Bản đồ tab (simple_map.cpp), that
// zone, saved for the map in configs\spots.ini. What it does in a fight is the
// hunter's; the area's numbers are in the "Tầm đánh" window
// (simple_style.cpp). Here is only what the window draws and asks of the
// hunter. In a file of its own so that simple_view.cpp gets only the calls.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/common.hpp"
#include "gobot/hunt_card.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/logger.hpp"
#include "gobot/packet_capture.hpp"

#include "imgui.h"

#include <cmath>
#include <cstdint>
#include <optional>
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
            hs.spot_why == Why::GaveUp || hs.spot_why == Why::NoReturn);
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
        case Hunter::SpotWhy::NoReturn:     return "no way back into the area the map shows, fighting where it stands";
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
    // Marked as on trial ticked or not, as the ticks beside it are. When a
    // saved zone takes over is the owner's Q1 (zone_take), said from there.
    const std::string take =
        kZoneTakeAnywhere ? std::string("bot dùng vùng đó, nhân vật ở đâu trên bản đồ cũng tự đi bộ về chỗ về của\n"
                                        "vùng.")
                          : "bot dùng vùng đó khi nhân vật\nđứng trong vùng hoặc cách vùng không quá " +
                                std::to_string(kZoneTakePx) +
                                " px; ở xa hơn thì đánh ở chỗ đang đứng\ntới khi nhân vật đi vào vùng.";
    const std::string tip = trial_hint(
        kTrialSpot,
        ("Chỉ đi tới quái trong một vùng quanh chỗ đánh quái: chỗ nhân vật đứng yên trên mặt đất\n"
         "lần đầu gói tin của game báo được vị trí sau khi bắt đầu (sau bước đi hay kỹ năng đầu tiên).\n"
         "Quái trong tầm đánh thì vẫn đánh, dù ở ngoài vùng. Bị đẩy ra xa mà hết quái thì bot tự\n"
         "đi bộ về chỗ. Không nhảy theo quái; đi về chỗ thì không bao giờ bấm ↓: chỗ thấp hơn\n"
         "chỉ về được khi bục cho nhân vật đi rơi xuống từ mép bục. Có dấu Nhảy xuống bậc dưới:\n"
         "chỉ nhảy xuống tới quái, khi chỗ đứng, chỗ rơi xuống và con quái đều ở trong vùng.\n"
         "Có vùng vẽ ở tab Bản đồ và dấu Nhảy lên bục trên: về chỗ về bằng cách đi bộ, rơi xuống\n"
         "hoặc nhảy lên; có dấu Tự tìm quái trong vùng: đi tới quái ở bất kỳ chỗ nào trong vùng.\n"
         "Cần Npcap: vị trí lấy từ gói tin của game, có dấu \"Vị trí từ gói tin\" hay không.\n"
         "Bản đồ có vùng đánh vẽ ở tab Bản đồ (lưu cho từng bản đồ): " + take + " Bản đồ chưa có\n"
         "vùng: chỗ chỉ nhớ trong lần chạy này, mỗi lần bấm BẮT ĐẦU lấy chỗ mới.\n"
         "Đổi bản đồ thì bot đứng yên tới khi nhân vật qua cửa quay về gần chỗ đã rời đi.\n"
         "Vào lại game (Tự đăng nhập hay Tìm lại) thì bot đứng yên tới khi nhân vật đứng yên cách chỗ\n"
         "đánh quái, hoặc chỗ nó đứng trước khi vào lại, không quá 150 px, cùng bản đồ (sau bước đi\n"
         "hay kỹ năng đầu tiên, để gói tin báo chỗ đứng). Không thì bấm Đặt chỗ này.\n"
         "Muốn đánh ở chỗ khác khi bot đang chạy: đưa nhân vật tới chỗ mới rồi bấm Đặt chỗ này. Bản\n"
         "đồ có vùng đã lưu thì vùng dời tới đó và lưu luôn (có Hoàn tác ở tab Bản đồ). Bỏ dấu Chỗ đánh\n"
         "quái, đi tới chỗ mới, đánh dấu lại rồi bấm Đặt chỗ này cũng được.\n"
         "Vùng đánh và khi nào đi về: nút Tầm đánh…, hoặc kéo vùng trên tab Bản đồ.")
            .c_str());
    ImGui::SetItemTooltip("%s", tip.c_str());
    if (!hunt_config_.spot) return;

    // "Đặt chỗ này": on a map with a saved zone, the zone moved to where the
    // character stands and saved (the owner's Q2, map_zone_here); otherwise
    // the hunter takes the spot again where the character stands, at its
    // next look that has it settled on the ground. Only while it hunts:
    // otherwise there is nothing to ask, and the next start takes a spot
    // afresh anyway.
    ImGui::SameLine();
    const bool can = hunting_ && hunter_ != nullptr;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Đặt chỗ này")) {
        if (!map_zone_here()) {
            hunter_->retake_spot();
            simple_note("Đặt chỗ đánh quái ở chỗ nhân vật đang đứng, lúc nhân vật đứng yên trên mặt đất.");
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", can ? (std::string("Đặt chỗ đánh quái mới ở chỗ nhân vật đang đứng: khi đã sang bản "
                                                   "đồ khác, vào lại game, hay muốn đánh ở chỗ khác.") +
                                       (kSpotHereMovesZone ? " (Bản đồ có vùng đã lưu: dời vùng tới đây và lưu.)"
                                                           : " (Bản đồ có vùng đã lưu: chỉ trong lần chạy này, vùng "
                                                             "đã lưu giữ nguyên.)"))
                                          .c_str()
                                    : "Chỉ dùng được khi đang đánh quái.");
}

void GuiApp::simple_spot_hint() {
    if (!hunt_config_.spot) return;
    // A zone saved for the map the character is on: that zone's numbers.
    std::string name;
    if (const std::optional<HuntZone> z = map_zone_now(&name)) {
        const std::string text = "Chỗ đánh quái: vùng đã lưu của bản đồ " + name + ": ngang " +
                                 std::to_string(z->left) + "–" + std::to_string(z->right) + ", dọc " +
                                 std::to_string(z->top) + "–" + std::to_string(z->bottom) + ", chỗ về " +
                                 std::to_string(z->home_x) + ", " + std::to_string(z->home_y) +
                                 "; hết quái thì đi về khi cách chỗ về hơn " +
                                 std::to_string(hunt_config_.spot_tolerance) + " px.";
        hint(trial_hint(kTrialSpot, text.c_str()).c_str());
        // "Tự tìm quái trong vùng" (2026.11.05b): what it does in this zone.
        if (hunt_config_.seek_in_zone && seek_tick(hunt_config_) == SeekTick::Ok)
            hint(trial_hint(kTrialSeek, "Tự tìm quái: bật — bot đi tới quái ở bất kỳ chỗ nào trong vùng mà nó tới được, "
                                        "kể cả con chưa thấy trên màn hình; không còn con nào tới được thì đứng chờ "
                                        "tại chỗ, không đi về chỗ về.")
                     .c_str());
        else
            hint("Tự tìm quái: tắt — bot đi tới quái thấy quanh nhân vật; hết quái thì đi về chỗ về.");
        return;
    }
    const std::string text = "Chỗ đánh quái: chỉ đi tới quái cách chỗ không quá " +
                             std::to_string(hunt_config_.spot_half_w) + " px ngang và " +
                             std::to_string(hunt_config_.spot_half_h) + " px dọc; hết quái thì đi về khi cách " +
                             "chỗ hơn " + std::to_string(hunt_config_.spot_tolerance) +
                             " px. Chỗ lấy khi bắt đầu đánh, chỉ nhớ trong lần chạy này (vùng vẽ ở tab Bản đồ thì "
                             "lưu cho từng bản đồ).";
    hint(trial_hint(kTrialSpot, text.c_str()).c_str());
    // The jumps up and the seek go by a zone of the map tab's alone.
    if (hunt_config_.jump_up || hunt_config_.seek_in_zone)
        hint("Nhảy lên bục trên và Tự tìm quái trong vùng chỉ chạy trong vùng vẽ ở tab Bản đồ (lưu cho từng bản đồ); "
             "với chỗ của lần chạy này, bot đánh như bản 2026.11.05.");
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

std::string GuiApp::spot_offset_vi(const Hunter::Status& hs) {
    // The zone's home, or the spot itself.
    const std::string place = hs.spot_from_map ? "chỗ về" : "chỗ";
    std::string line = "nhân vật cách " + place + " " + px(std::abs(hs.spot_dx)) + " px";
    // Height only past what the walk back takes for level ground.
    if (hs.spot_dy < -static_cast<float>(Hunter::kSpotFloorPx))
        line += ", " + place + " cao hơn " + px(-hs.spot_dy) + " px";
    else if (hs.spot_dy > static_cast<float>(Hunter::kSpotFloorPx))
        line += ", " + place + " thấp hơn " + px(hs.spot_dy) + " px";
    if (hs.returns > 0) line += "; đã đi về " + std::to_string(hs.returns) + " lần";
    return line;
}

std::string GuiApp::spot_outside_vi(const Hunter::Status& hs) {
    // The map tab's zone alone: this run's spot is said as it always was.
    if (!hs.spot_from_map || hs.spot_outside_px <= 0) return {};
    const std::string out = "Nhân vật đang ở ngoài vùng " + std::to_string(hs.spot_outside_px) + " px";
    const std::string way = std::to_string(hs.spot_way_x) + ", " + std::to_string(hs.spot_way_y);
    // A way that jumps up on the way (2026.11.05b): the jump it starts with
    // said, or that it jumps further on.
    const std::string jump = !hs.spot_way || hs.spot_way_jumps <= 0 ? std::string()
                             : hs.spot_way_jump_first
                                 ? std::string(" (nhảy ") + jump_kind_key(hs.spot_way_jump_kind) + " ở x " +
                                       std::to_string(hs.spot_way_jump_x) + ")"
                                 : std::string(" (có nhảy lên trên đường về)");
    // The owner's Q2 (zone_walks_back_first): far enough out, with a way back
    // the map shows, the walk back comes before any fight.
    if (hs.spot_back_first && !jump.empty()) return out + " — bot về " + way + jump + " trước, rồi mới đánh tiếp.";
    if (hs.spot_back_first) return out + " — bot đi bộ về " + way + " trước, rồi mới đánh tiếp.";
    if (hs.spot_way && !jump.empty())
        return out + " — bot vẫn đánh quái trong tầm; hết quái thì về " + way + jump + ".";
    if (hs.spot_way) return out + " — bot vẫn đánh quái trong tầm; hết quái thì đi bộ về " + way + ".";
    return out + " — bot vẫn đánh quái trong tầm; hết quái thì đi về chỗ về.";
}

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
            // The map tab's zone: its box, which need not be even round the
            // home; this run's spot: ± its halves.
            const std::string line =
                hs.spot_from_map
                    ? "Chỗ đánh quái: vùng của bản đồ " + map_name_of(hs.spot_stem) + " (ngang " + px(hs.spot_left) +
                          "–" + px(hs.spot_right) + ", dọc " + px(hs.spot_top) + "–" + px(hs.spot_bottom) +
                          "), chỗ về " + px(hs.spot_x) + ", " + px(hs.spot_y) + "; " + spot_offset_vi(hs)
                    : "Chỗ đánh quái: " + px(hs.spot_x) + ", " + px(hs.spot_y) + " (vùng ±" +
                          std::to_string(hs.spot_half_w) + " / ±" + std::to_string(hs.spot_half_h) + " px); " +
                          spot_offset_vi(hs);
            // Outside the map tab's box: never in green (2026.11.04d).
            const std::string outside = spot_outside_vi(hs);
            coloured(outside.empty() ? kGood : kWait, (line + ".").c_str());
            if (!outside.empty()) coloured(kWait, outside.c_str());
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
            // own way (Hunter::Spot::login). The map tab's zone comes back
            // with the map known again and the character near it -- set
            // aside by the tab itself when it lost the map (zone_aside).
            if (hs.spot_from_map && hs.spot_map_aside)
                coloured(kWait, ("Chỗ đánh quái tạm dừng: GoBot vừa mất dấu bản đồ (gói tin bắt lại hay bị tắt, hoặc "
                                 "phải nhận ra lại bản đồ) — bot đứng yên, chỉ đánh quái tới trong tầm, tới khi biết lại bản đồ " +
                                 map_name_of(hs.spot_stem) + " và nhân vật ở trong vùng hoặc cách vùng không quá " +
                                 std::to_string(kZoneTakePx) + " px. Muốn đánh ở đây thì bấm Đặt chỗ này.")
                                    .c_str());
            else if (hs.spot_from_map)
                coloured(kWait, ("Chỗ đánh quái tạm dừng: đã sang bản đồ khác hoặc vào lại game — bot đứng yên, chỉ "
                                 "đánh quái tới trong tầm, tới khi nhân vật về lại vùng của bản đồ " +
                                 map_name_of(hs.spot_stem) + ". Muốn đánh ở đây thì bấm Đặt chỗ này.")
                                    .c_str());
            else if (hs.spot_login)
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
        case Why::NoReturn:
            // The map tab's zone, by its map's file: nothing of the floors
            // the character is on walks or falls back into the box.
            // Worked out with jumps up able to go (2026.11.05b): none of
            // walking, falling or jumping up gets back.
            coloured(kWait, ((hs.spot_outside_px > 0 ? "Nhân vật đang ở ngoài vùng " + std::to_string(hs.spot_outside_px) +
                                                           " px, và từ chỗ này"
                                                     : std::string("Từ chỗ nhân vật đứng")) +
                             (hs.spot_way_by_jumps
                                  ? " không có đường đi bộ, rơi xuống hay nhảy lên nào về vùng: bot đánh ở đây. Tự đưa "
                                    "nhân vật về vùng, hoặc bấm Đặt chỗ này."
                                  : " không có đường đi bộ hay rơi xuống nào về vùng: bot đánh ở đây. Tự đưa nhân vật về "
                                    "vùng, hoặc bấm Đặt chỗ này."))
                                .c_str());
            break;
    }
    // A zone of the map tab sent and not taken yet: why.
    switch (hs.zone_wait) {
        case Hunter::ZoneWait::Outside:
            coloured(kWait, ("Nhân vật đang ở ngoài vùng đã lưu của bản đồ này (" + std::to_string(hs.zone_wait_px) +
                             " px) — đánh ở chỗ đang đứng. Đưa nhân vật vào vùng, hoặc bấm “Dời vùng tới nhân vật” "
                             "ở tab Bản đồ.")
                                .c_str());
            break;
        case Hunter::ZoneWait::NotSettled:
        case Hunter::ZoneWait::Ahead:
            hint("Chờ nhân vật đứng yên trên mặt đất để dùng vùng đã lưu.");
            break;
        case Hunter::ZoneWait::None:
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
        out << "spot at " << px(hs.spot_x) << "," << px(hs.spot_y);
        // The map tab's area, by its box; this run's, by its halves.
        if (hs.spot_from_map)
            out << " (area " << hs.spot_stem << " (map tab) " << px(hs.spot_left) << ".." << px(hs.spot_right)
                << " x " << px(hs.spot_top) << ".." << px(hs.spot_bottom) << ")";
        else
            out << " (+-" << hs.spot_half_w << " / +-" << hs.spot_half_h << ")";
        if (placed(hs))
            out << ", you " << px(std::abs(hs.spot_dx)) << " px off (it is " << px(std::abs(hs.spot_dy)) << " px "
                << (hs.spot_dy < 0.0f ? "up" : "down") << ")";
        // Outside the area: how far, and -- the map tab's, by its map's file
        // -- whether the walk back comes first, and to where (2026.11.04d).
        if (hs.spot_outside_px > 0) {
            out << ", outside the area " << hs.spot_outside_px << " px";
            if (hs.spot_back_first)
                out << " (walking back first, to " << hs.spot_way_x << "," << hs.spot_way_y << ")";
        }
        // A way back that jumps up (2026.11.05b): how many, and the one it
        // starts with.
        if (hs.spot_way && hs.spot_way_jumps > 0) {
            out << ", the way back jumps up " << hs.spot_way_jumps << " time" << (hs.spot_way_jumps == 1 ? "" : "s");
            if (hs.spot_way_jump_first)
                out << ", first " << jump_kind_name(hs.spot_way_jump_kind) << " at x " << hs.spot_way_jump_x;
        }
    }
    if (hs.zone_wait == Hunter::ZoneWait::Outside)
        out << ", the map tab's area " << hs.zone_wait_stem << " waits outside " << hs.zone_wait_px << " px";
    else if (hs.zone_wait != Hunter::ZoneWait::None)
        out << ", the map tab's area " << hs.zone_wait_stem << " waits for the character to stand settled";
    out << ", returns " << hs.returns << ", given up " << hs.return_give_ups;
    if (hs.spot_why != Hunter::SpotWhy::None) out << " -- " << spot_why_name(hs.spot_why);
    return out.str();
}

} // namespace gobot
