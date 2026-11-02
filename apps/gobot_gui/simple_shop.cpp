// "Tự mua bình" in Dọn rác: buying Thịt bò khô and Rượu lá tre from the shop
// the Người Giữ Sân item opens, when Tiêu Dùng holds too few slots of them.
//
// The run itself, and every rule it keeps, is the library's (gobot/bag.hpp,
// BagCleaner::Mode::ShopBuy) and the worker's (GuiApp::trash_loop, run_shop).
// Here is what only the window knows: the card and what it writes to
// configs/trash.ini.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/glyph_font.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace gobot {

using namespace simple_ui;

namespace {

/// The file of picture `name` beside GoBot, never looked for elsewhere.
bool beside(const char* name) {
    std::string file = TemplateStore::default_dir() + "\\" + name + ".png";
    std::replace(file.begin(), file.end(), '/', '\\');
    return path_exists(file);
}

/// Whether the purchase can be held to its rules here: the pictures it tells
/// the shop and its questions apart by, and the digits it reads the box with,
/// asked once -- and again every few seconds while the answer is no, so that
/// pictures copied in while GoBot runs are noticed.
bool can_buy() {
    static int known = -1;
    static std::uint64_t asked_ms = 0;
    if (known == 1) return true;
    if (known < 0 || now_ms() - asked_ms > 5000) {
        asked_ms = now_ms();
        known = 0;
        if (!GlyphFont::default_dir().empty() && beside(bag::kShopRow) && beside(bag::kShopButtons) &&
            beside(bag::kShopFull) && beside(bag::kSellQuestion)) {
            TemplateStore pictures;
            for (const char* name : { "grabbed/confirm_button", "grabbed/del_item_confirm_1",
                                      "grabbed/del_item_confirm_2", "grabbed/hoisinh_confirm", "grabbed/drop_confirm",
                                      bag::kSellQuestion, bag::kShopRow, bag::kShopButtons, bag::kShopFull,
                                      bag::kCloseButtons[0], bag::kCloseButtons[1], bag::kCloseButtons[2] }) {
                Frame f;
                if (TemplateStore::load_one(name, exe_relative("configs/templates.ini"), f)) pictures.put(name, f);
            }
            known = bag::can_check_shop(pictures) ? 1 : 0;
        }
    }
    return known == 1;
}

}  // namespace

bool GuiApp::simple_can_buy() { return can_buy(); }

void GuiApp::simple_shop_card(const std::function<void(const char*, const std::string&)>& put) {
    if (!begin_card("##trashshop")) { end_card(); return; }
    heading(strong_font_, "Tự mua bình");

    // The switch, off as shipped, and how often the bag is looked at.
    if (ImGui::Checkbox("Tự mua bình", &simple_clean_.buy_potions)) {
        put("buy_potions", simple_clean_.buy_potions ? "true" : "false");
        // The worker does it between its passes, so it runs for it too: started
        // with the bot, ended when nothing else asks for it.
        if (simple_clean_.buy_potions && simple_running()) start_trash();
        if (!simple_clean_.buy_potions && !simple_clean_.enabled && !simple_clean_.use_charms && trash_running_ &&
            !trash_oneshot_)
            stop_trash();
    }
    ImGui::SetItemTooltip("%s", trial_hint(kTrialUse,
                                           "Mỗi khoảng thời gian bên cạnh, GoBot mở túi đồ, đếm số ô Thịt bò khô và "
                                           "Rượu lá tre ở ngăn Tiêu Dùng; ô nào ít hơn mức \"dưới\" thì bấm chuột "
                                           "phải vào món Người Giữ Sân ở ngăn Khác, mua tới mức \"tới\" (mỗi lần mua "
                                           "100, mỗi lần đầy một ô), rồi bấm ESC đóng cửa hàng. Trong lúc mua, kỹ "
                                           "năng, bình và buff tạm dừng (có thể vài chục giây): GoBot gõ số vào game "
                                           "và các phím đó sẽ gõ lẫn vào. Không mua khi máu thấp, đang chat hay bot "
                                           "đang bận việc khác. Tắt thì không làm gì.")
                                    .c_str());
    ImGui::SameLine(0, 16);
    label("kiểm tra mỗi");
    ImGui::SameLine();
    {
        static const int kMinutes[] = { 15, 30, 60, 90, 120, 180 };
        const std::string shown = std::to_string(simple_clean_.buy_every_minutes) + " phút";
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##buyevery", shown.c_str())) {
            for (const int m : kMinutes) {
                const std::string item = std::to_string(m) + " phút";
                if (ImGui::Selectable(item.c_str(), m == simple_clean_.buy_every_minutes)) {
                    simple_clean_.buy_every_minutes = m;
                    put("buy_every_minutes", std::to_string(m));
                }
            }
            ImGui::EndCombo();
        }
    }
    if (simple_clean_.buy_potions && !can_buy()) {
        ImGui::SameLine();
        coloured(kWait, "GoBot chưa có hình của game để làm việc này, nên chưa mua.");
    }

    // Slots of each potion, in Tiêu Dùng: under `below`, more are bought up to
    // `to`. 0 for `below` leaves that potion alone.
    const auto limits = [&](const char* name, const char* id, int& below, int& to, const char* below_key,
                            const char* to_key) {
        ImGui::PushID(id);
        label(name);
        ImGui::SameLine(0, 10);
        label("mua khi dưới");
        ImGui::SameLine();
        // Each number saves itself alone, as it is typed: the other is never moved
        // under the hand -- typing 12 passes through 1 -- and a threshold over
        // its limit is read as the limit, as the file is when it is loaded.
        const char* changed_key = nullptr;
        ImGui::SetNextItemWidth(95);
        if (ImGui::InputInt("##below", &below, 1, 5)) {
            below = std::clamp(below, 0, CleanSettings::kBuySlotsMost);
            changed_key = below_key;
        }
        ImGui::SetItemTooltip("Số ô thấp nhất cho phép. Ít hơn số này thì mua. 0: không mua món này.");
        ImGui::SameLine();
        label("ô, mua tới");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(95);
        if (ImGui::InputInt("##to", &to, 1, 5)) {
            to = std::clamp(to, 0, CleanSettings::kBuySlotsMost);
            changed_key = to_key;
        }
        ImGui::SetItemTooltip("Mua cho tới khi túi có chừng này ô. Mỗi lần mua 100, thường đầy một ô.");
        ImGui::SameLine();
        label("ô");
        if (below > to) {
            ImGui::SameLine();
            coloured(kWait, "(\"dưới\" lớn hơn \"tới\": GoBot lấy \"dưới\" bằng \"tới\")");
        }
        ImGui::PopID();
        if (changed_key != nullptr) put(changed_key, std::to_string(changed_key == below_key ? below : to));
    };
    limits("Thịt bò khô (HP):", "hp", simple_clean_.buy_hp_below, simple_clean_.buy_hp_to, "buy_hp_below",
           "buy_hp_to");
    limits("Rượu lá tre (MP):", "mp", simple_clean_.buy_mp_below, simple_clean_.buy_mp_to, "buy_mp_below",
           "buy_mp_to");
    hint("Cần có món Người Giữ Sân trong ngăn Khác của túi đồ. Mỗi lần mua GoBot gõ 100 vào hộp số lượng và "
         "đọc lại rồi mới bấm Xác Nhận; có gì lạ thì bấm Hủy Bỏ và dừng, giữ ảnh trong thư mục trash_debug. "
         "Thử mua một lần trước khi bật: nút bên dưới đi tới hộp số lượng rồi bấm Hủy Bỏ, không mua gì.");

    // Now, and a trial that buys nothing.
    const bool busy = trash_running_ && (trash_oneshot_ || trash_request_ != 0 || trash_pass_);
    const bool chat_open = chat_doubt_;
    ImGui::BeginDisabled(busy || chat_open || !input_.window_valid());
    if (ImGui::Button("Thử mua (không mua gì)")) request_shop(true);
    ImGui::SetItemTooltip("Mở túi đồ, đếm bình, mở cửa hàng Người Giữ Sân, bấm đúp vào từng món, rồi bấm Hủy Bỏ "
                          "ở hộp số lượng và đóng cửa hàng. Không gõ số và không mua. Giữ ảnh các bước trong "
                          "trash_debug.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!simple_clean_.buy_potions);
    if (ImGui::Button("Mua ngay")) request_shop(false);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", simple_clean_.buy_potions
                                    ? "Kiểm tra và mua ngay bây giờ, không chờ tới giờ. Kỹ năng, bình và buff tạm "
                                      "dừng cho tới khi xong."
                                    : "Bật ô \"Tự mua bình\" trước.");
    ImGui::EndDisabled();
    if (chat_open)
        coloured(kWait, "Ô chat trong game có thể đang mở: chưa mua được cho đến khi ô chat đóng.");
    end_card();
}

}  // namespace gobot
