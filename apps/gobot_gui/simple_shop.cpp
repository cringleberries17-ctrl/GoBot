// "Tự mua bình" in Dọn rác: buying Thịt bò khô and Rượu lá tre from the shop
// the Người Giữ Sân item opens -- the number of purchases set for each, every
// so often. Nothing in the bag is counted (2026.11.09): the game's message
// that the bag is full ends a round, and the next time buys again.
//
// The run itself, and every rule it keeps, is the library's (gobot/bag.hpp,
// BagCleaner::Mode::ShopBuy) and the worker's (GuiApp::trash_loop, run_shop).
// Here is what only the window knows: the card, what it writes to
// configs/trash.ini, and the plan's line, read from the worker's atomics --
// never from a file.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/glyph_font.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
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

/// "23:15" for a time in seconds since 1970, in this PC's own time.
std::string hhmm(std::int64_t at_s) {
    const std::time_t t = static_cast<std::time_t>(at_s);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return "?";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

/// The card's line on the plan: when the next one begins, and what is left
/// of this one and when it is bought -- or, with no worker to buy it, what a
/// round cut short left for the next run. Empty when there is nothing to say.
std::string plan_line(std::int64_t plan_at_s, std::int64_t look_at_s, int owed, std::int64_t now_s) {
    if (plan_at_s > 0) {
        std::string line = "Lần mua tới: " + hhmm(plan_at_s);
        if (owed > 0 && look_at_s > 0) {
            const std::int64_t in_s = std::max<std::int64_t>(0, look_at_s - now_s);
            line += " (còn " + std::to_string(owed) + " lần của lượt này, mua tiếp sau " +
                    std::to_string(std::max<std::int64_t>(1, (in_s + 59) / 60)) + " phút)";
        }
        return line + ".";
    }
    // With no worker, at the first timed look after BẮT ĐẦU -- a whole
    // interval on since 2026.11.12 -- where a plan come due begins afresh.
    if (owed > 0)
        return "Còn " + std::to_string(owed) +
               " lần mua của lượt trước: GoBot mua ở lần mua theo giờ đầu tiên sau khi bấm BẮT ĐẦU.";
    return std::string();
}

}  // namespace

bool GuiApp::simple_can_buy() { return can_buy(); }

void GuiApp::simple_shop_card(const std::function<void(const char*, const std::string&)>& put) {
    if (!begin_card("##trashshop")) { end_card(); return; }
    heading(strong_font_, "Tự mua bình");

    // The switch, off as shipped, and how often a plan is bought.
    if (ImGui::Checkbox("Tự mua bình", &simple_clean_.buy_potions)) {
        put("buy_potions", simple_clean_.buy_potions ? "true" : "false");
        // Ticked on: nothing owed, the shop's clock -- a whole interval to the
        // first look (2026.11.12) -- and its waits for a quiet second afresh,
        // in the worker running or the next (trash_loop).
        if (simple_clean_.buy_potions) {
            ++shop_ticked_;
            shop_owed_ = 0;
        }
        // The worker does it between its passes, so it runs for it too: started
        // with the bot, ended when nothing else asks for it.
        if (simple_clean_.buy_potions && simple_running()) start_trash();
        if (!simple_clean_.buy_potions && !simple_clean_.enabled && !simple_clean_.use_charms && trash_running_ &&
            !trash_oneshot_)
            stop_trash();
    }
    {
        static const std::string tip = trial_hint(
            kTrialUse,
            ("Mỗi khoảng thời gian bên cạnh, GoBot bấm chuột phải vào món Người Giữ Sân ở ngăn Khác và mua số ô đã "
             "đặt bên dưới, xen kẽ Thịt bò khô và Rượu lá tre (mỗi ô là một lần mua " +
             std::to_string(CleanSettings::kBuyQuantity) +
             "). Giờ mua đếm từ lúc bấm BẮT ĐẦU (hay lúc đánh dấu ô này): lần mua đầu là một khoảng \"mua mỗi\" "
             "sau đó, không mua ngay lúc bật; muốn mua ngay thì bấm Mua ngay. DỪNG rồi BẮT ĐẦU lại khi chưa tới "
             "giờ mua thì giờ đang đếm được giữ. "
             "Khi Dọn rác bật, tới giờ mua GoBot dọn rác trước rồi mua liền sau đó, trong cùng một lần dừng "
             "phím. GoBot không đếm bình trong túi nữa. Khi game báo túi đồ đã đầy, GoBot đóng thông báo và cửa hàng "
             "rồi dừng lượt đó; tới giờ sau mua lại. Mỗi lượt giữ phím mua tối đa " +
             std::to_string(BagCleaner::kShopBuysPerHold) + " lần, phần còn lại mua tiếp sau " +
             std::to_string(BagCleaner::kShopPartGapMs / 60'000) +
             " phút. Trong lúc mua, bot đứng yên; các phím chữ cái, Space, Home, End, PgUp, PgDn, Insert và Delete "
             "của kỹ năng, bình và buff vẫn bấm giữa hai lần mua, nhưng không bao giờ từ lúc bấm đúp vào bình tới "
             "khi hộp số lượng đã đóng; phím số chờ mua xong. GoBot mua ở mọi mức máu, không chờ máu lên: bình HP "
             "và MP bấm xen giữa các lần mua, kể cả lúc bot đang dừng (khi đó GoBot tự bấm bình theo các dòng bình "
             "của bạn). Chỉ khi nhân vật đã chết thì GoBot chờ (thử lại sau " +
             std::to_string(bag::kHpStoppedAgainMs / 1000) +
             " giây). Bình ở phím số thì không được bấm trong lúc mua và dọn rác; muốn bình được bấm thì đặt nó ở "
             "phím chữ cái hoặc Home, End, PgUp, PgDn, Insert, Delete. Không mua khi đang chat, "
             "đang có bảng Sự kiện và điểm danh hoặc bot đang bận việc khác. Tắt thì không làm gì.")
                .c_str());
        ImGui::SetItemTooltip("%s", tip.c_str());
    }
    // "Mua ngay sau khi dọn rác" (2026.11.13b) ticked, with Dọn rác on: no
    // clock to set -- the plan is bought right after each timed pass.
    const bool after_pass = simple_clean_.buy_after_trash && simple_clean_.enabled;
    ImGui::SameLine(0, 16);
    ImGui::BeginDisabled(after_pass);
    label("mua mỗi");
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
    ImGui::EndDisabled();
    if (after_pass) ImGui::SetItemTooltip("Không dùng khi \"Mua ngay sau khi dọn rác\" đang đánh dấu và Dọn rác bật.");
    if (simple_clean_.buy_potions && !can_buy()) {
        ImGui::SameLine();
        coloured(kWait, "GoBot chưa có hình của game để làm việc này, nên chưa mua.");
    }
    // Right after each timed pass instead of by the clock (2026.11.13b; users,
    // via the owner: "Bằng checkbox này thì không cần thiết định thời gian mua
    // HP/MP nữa, sẽ ngay lập tức thực hiện mua sau khi dọn rác xong").
    if (ImGui::Checkbox("Mua ngay sau khi dọn rác", &simple_clean_.buy_after_trash))
        put("buy_after_trash", simple_clean_.buy_after_trash ? "true" : "false");
    ImGui::SetItemTooltip(
        "%s", "Đánh dấu: mỗi lần Dọn rác theo giờ xong, GoBot mua bình liền sau đó, trong cùng một lần dừng phím, "
              "không chờ thêm; không cần đặt giờ mua (ô \"mua mỗi\" không dùng). Phần còn lại của một lượt mua "
              "nhiều ô vẫn mua tiếp như cũ. Danh sách đồ bỏ đi trống thì tới giờ dọn vẫn mua. Lần mua đó chưa chạy "
              "được (chưa đọc được máu, đang có bảng Sự kiện...) "
              "thì GoBot thử lại theo luật mua bình. \"Dọn ngay\" và \"Thử tìm\" không mua. Cần bật cả Dọn rác: Dọn "
              "rác tắt thì GoBot mua theo giờ \"mua mỗi\" như cũ.");
    if (simple_clean_.buy_after_trash && simple_clean_.buy_potions && !simple_clean_.enabled) {
        ImGui::SameLine();
        coloured(kWait, "Dọn rác đang tắt: mua theo giờ \"mua mỗi\".");
    }

    // How many purchases of 100 of each potion each time -- a slot each -- 0
    // leaving that potion alone. No cap of GoBot's own (the owner, 2026-11-10:
    // "Không giới hạn"): the game's message that the bag is full ends a round,
    // and kBuySlotsPerRunMost only bounds the box. Each saves itself alone, as
    // it is typed.
    static const std::string slots_tip =
        "Số lần mua " + std::to_string(CleanSettings::kBuyQuantity) + " mỗi khi tới giờ, từ 0 tới " +
        std::to_string(CleanSettings::kBuySlotsPerRunMost) +
        ". 0: không mua món này. GoBot không giới hạn số này: HP và MP chung ngăn Tiêu thụ, game báo túi đồ đã "
        "đầy thì GoBot dừng lượt đó.";
    const auto slots = [&](const char* name, const char* id, int& n, const char* key) {
        ImGui::PushID(id);
        label(name);
        ImGui::SameLine(0, 10);
        label("mỗi lần mua");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(95);
        const bool changed = ImGui::InputInt("##slots", &n, 1, 10);
        if (changed) n = std::clamp(n, 0, CleanSettings::kBuySlotsPerRunMost);
        ImGui::SetItemTooltip("%s", slots_tip.c_str());
        ImGui::SameLine();
        label("ô");
        ImGui::PopID();
        if (changed) put(key, std::to_string(n));
    };
    slots("Thịt bò khô (HP):", "hp", simple_clean_.buy_hp_slots, "buy_hp_slots");
    slots("Rượu lá tre (MP):", "mp", simple_clean_.buy_mp_slots, "buy_mp_slots");
    const int planned = simple_clean_.buy_hp_slots + simple_clean_.buy_mp_slots;
    if (simple_clean_.buy_potions && planned <= 0)
        coloured(kWait, "Cả hai đều 0 ô: tới giờ GoBot không mua gì.");
    // More than "Mua ngay" buys in its one hold: how the rest is bought.
    if (planned > BagCleaner::kShopMaxBuys) {
        const std::string over =
            "Tổng " + std::to_string(planned) + " lần: một lượt giữ phím mua tối đa " +
            std::to_string(BagCleaner::kShopBuysPerHold) + " lần (\"Mua ngay\": " +
            std::to_string(BagCleaner::kShopMaxBuys) + " lần), phần còn lại mua tiếp, mỗi " +
            std::to_string(BagCleaner::kShopPartGapMs / 60'000) +
            " phút một lượt, khi bot chạy, cho tới khi đủ số hoặc game báo túi đồ đã đầy.";
        hint(over.c_str());
    }
    hint("Cần có món Người Giữ Sân trong ngăn Khác của túi đồ, và phím bình của bạn phải uống Thịt bò khô và Rượu "
         "lá tre. Mỗi lần mua GoBot gõ 100 vào hộp số lượng, đọc lại rồi mới bấm Xác Nhận; có gì lạ thì bấm Hủy Bỏ "
         "và dừng, giữ ảnh trong thư mục trash_debug. Thử mua một lần trước khi bật, và lần đầu nên đặt 1 ô mỗi "
         "loại.");
    // The plan, as the worker last left it: atomics, no file.
    if (simple_clean_.buy_potions && after_pass) {
        // Buying after each pass (2026.11.13b): no plan time; a part still
        // owed, and when it is bought.
        const int owed = shop_owed_.load();
        const std::int64_t look_at = shop_look_at_s_.load();
        std::string line = "Mua bình ngay sau mỗi lần Dọn rác theo giờ.";
        if (owed > 0 && look_at > 0) {
            const std::int64_t in_s = std::max<std::int64_t>(0, look_at - static_cast<std::int64_t>(std::time(nullptr)));
            line += " Còn " + std::to_string(owed) + " lần của lượt này, mua tiếp sau " +
                    std::to_string(std::max<std::int64_t>(1, (in_s + 59) / 60)) + " phút.";
        }
        hint(line.c_str());
    } else if (simple_clean_.buy_potions) {
        const std::string line = plan_line(shop_plan_at_s_.load(), shop_look_at_s_.load(), shop_owed_.load(),
                                           static_cast<std::int64_t>(std::time(nullptr)));
        if (!line.empty()) hint(line.c_str());
    }
    // With Dọn rác on, the round rides a pass (2026.11.11, the owner: "Tới giờ
    // mua: dọn rồi mua"; GuiApp::trash_loop, bag::ShopRide).
    if (simple_clean_.buy_potions && simple_clean_.enabled && !after_pass) {
        static const std::string ride =
            "Dọn rác đang bật: tới giờ mua, GoBot dọn rác trước rồi mua bình liền sau đó, trong cùng một lần dừng "
            "phím, không chờ thêm. Vừa dọn xong chưa tới " + std::to_string(bag::kRideFreshMs / 60'000) +
            " phút thì mua luôn, không dọn lại.";
        hint(ride.c_str());
    }

    // The pet's HP food on reaching TổCổLong (2026.11.24, the owner: "Khi ra
    // tới bản đồ TổCổLong thì cần mua lại HP cho pet. Cái này ở trong tab Linh
    // Thú của người giữ sân và là item "Thịtbò" => cần có lựa chọn để mua và
    // mua bao nhiêu slot tương tự như mua HP MP"): its own box, off as shipped,
    // and its slots as the potions' -- a purchase of 100 a slot, nothing
    // counted, the game's message that its bag is full the end of it. Bought
    // at each arrival there with the bot running (GuiApp::pet_arrival_tick,
    // PetRestock), whatever "Tự mua bình" and the other bag jobs say.
    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::Checkbox("Mua HP pet (Thịt bò) khi ra tới TổCổLong", &simple_clean_.buy_pet))
        put("buy_pet", simple_clean_.buy_pet ? "true" : "false");
    {
        static const std::string pet_tip =
            "Mỗi lần nhân vật ra tới bản đồ TổCổLong (sảnh Cổ Long, chỗ ra sau mỗi lượt phó bản) khi bot đang chạy, "
            "GoBot mở cửa hàng của món Người Giữ Sân, sang ngăn Linh Thú và mua số ô đặt bên dưới của món Thịtbò "
            "(thức ăn hồi HP cho pet; không phải Thịt bò khô của nhân vật), mỗi ô một lần mua " +
            std::to_string(CleanSettings::kBuyQuantity) +
            ", gõ số rồi đọc lại trước khi bấm Xác Nhận như khi mua bình. Phó bản chờ lượt mua này, tối đa " +
            std::to_string(PetRestock::kWaitMostMs / 1000) +
            " giây rồi đi tiếp. Mua chưa được thì thử lại sau " + std::to_string(PetRestock::kAgainMs / 1000) +
            " giây nếu còn ở TổCổLong (trong " + std::to_string(PetRestock::kWaitMostMs / 1000) +
            " giây đầu), không thì ở lần ra TổCổLong sau. Game báo túi đồ [Linh Thú] đã đầy thì GoBot "
            "đóng thông báo và thôi mua lượt đó. Chạy cả khi các ô khác của thẻ này tắt. Tắt thì không làm gì.";
        ImGui::SetItemTooltip("%s", pet_tip.c_str());
    }
    ImGui::PushID("pet");
    label("Thịt bò (HP pet):");
    ImGui::SameLine(0, 10);
    label("mỗi lần mua");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95);
    if (ImGui::InputInt("##petslots", &simple_clean_.buy_pet_slots, 1, 10)) {
        simple_clean_.buy_pet_slots = std::clamp(simple_clean_.buy_pet_slots, 0, CleanSettings::kBuySlotsPerRunMost);
        put("buy_pet_slots", std::to_string(simple_clean_.buy_pet_slots));
    }
    {
        static const std::string pet_slots_tip =
            "Số lần mua " + std::to_string(CleanSettings::kBuyQuantity) + " Thịtbò mỗi lần ra tới TổCổLong, từ 0 tới " +
            std::to_string(CleanSettings::kBuySlotsPerRunMost) + ". 0: không mua.";
        ImGui::SetItemTooltip("%s", pet_slots_tip.c_str());
    }
    ImGui::SameLine();
    label("ô");
    ImGui::PopID();
    if (simple_clean_.buy_pet && simple_clean_.buy_pet_slots <= 0)
        coloured(kWait, "0 ô: ra tới TổCổLong GoBot không mua gì cho pet.");
    if (simple_clean_.buy_pet && !can_buy()) coloured(kWait, "GoBot chưa có hình của game để làm việc này, nên chưa mua.");

    // Now, and a trial that buys nothing.
    const bool busy = trash_running_ && (trash_oneshot_ || trash_request_ != 0 || trash_pass_);
    const bool chat_open = chat_doubt_;
    ImGui::BeginDisabled(busy || chat_open || !input_.window_valid());
    if (ImGui::Button("Thử mua (không mua gì)")) request_shop(true);
    ImGui::SetItemTooltip("Mở túi đồ, mở cửa hàng Người Giữ Sân, bấm đúp vào từng món, rồi bấm Hủy Bỏ ở hộp số "
                          "lượng và đóng cửa hàng. Không gõ số và không mua. Giữ ảnh các bước trong trash_debug.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!simple_clean_.buy_potions);
    if (ImGui::Button("Mua ngay")) request_shop(false);
    ImGui::EndDisabled();
    // At most kShopMaxBuys in its one hold, the rest as a timed round buys it.
    static const std::string buy_now_tip =
        "Mua ngay bây giờ số ô đã đặt, không chờ tới giờ; giờ mua kế tiếp tính lại từ lúc này. Lượt này mua tối "
        "đa " + std::to_string(BagCleaner::kShopMaxBuys) + " lần; phần còn lại mua tiếp, mỗi " +
        std::to_string(BagCleaner::kShopPartGapMs / 60'000) + " phút một lượt " +
        std::to_string(BagCleaner::kShopBuysPerHold) +
        " lần, khi bot chạy (bot đang dừng thì ở lần mua theo giờ đầu tiên sau khi bấm BẮT ĐẦU). "
        "Game báo túi đồ đã đầy thì dừng lượt đó. "
        "Trong lượt mua, phím chữ cái, Space, Home, End, PgUp, PgDn, Insert và Delete của kỹ năng, bình và buff "
        "chỉ bấm giữa hai lần mua; phím số chờ xong lượt.";
    ImGui::SetItemTooltip("%s", simple_clean_.buy_potions ? buy_now_tip.c_str() : "Bật ô \"Tự mua bình\" trước.");
    ImGui::EndDisabled();
    if (chat_open)
        coloured(kWait, "Ô chat trong game có thể đang mở: chưa mua được cho đến khi ô chat đóng.");
    end_card();
}

}  // namespace gobot
