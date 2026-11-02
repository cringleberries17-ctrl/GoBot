// "Dùng" in Dọn rác: registering fragments in Yêu Quái Bách Khoa with the trash
// pass -- the options in the Dọn rác tab's two pickers, and what the trash
// worker says of a pass.
//
// The pass itself, and every rule it keeps, is the library's (gobot/bag.hpp,
// TrashList::Action::Use). Here is what only the window knows: the tab's lists
// and the bag picture it was given.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/bag.hpp"
#include "gobot/common.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace gobot {
namespace {

using namespace simple_ui;

const char* const kUseOption = "Dùng — đăng ký vào Yêu Quái Bách Khoa (chuột phải, không lấy lại được)";
const char* const kUseThenDelete = "Dùng, rồi bỏ phần còn lại vào thùng rác";

bool has(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void take_out(std::vector<std::string>& ids, const std::string& id) {
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
}

/// The file of picture `name` beside GoBot, never looked for elsewhere.
bool beside_gobot(const char* name) {
    std::string file = TemplateStore::default_dir() + "\\" + name + ".png";
    std::replace(file.begin(), file.end(), '/', '\\');
    return path_exists(file);
}

/// Whether the registration question can be told from the others here, and
/// the monster book it opens closed again, asked once: the few pictures
/// can_check(Use) wants, loaded one by one rather than the thousand others --
/// and the registration's and the book's only when they are files beside
/// GoBot, never looked for in another program's assembly.
bool can_register() {
    static int known = -1;
    if (known < 0) {
        known = 0;
        if (beside_gobot(bag::kRegisterDialog) && beside_gobot(bag::kBookTitle) && beside_gobot(bag::kBookClose)) {
            // The other questions too, which can_check makes sure the
            // registration's words are none of.
            TemplateStore pictures;
            for (const char* name : { "grabbed/confirm_button", "grabbed/del_item_confirm_1",
                                      "grabbed/del_item_confirm_2", "grabbed/hoisinh_confirm",
                                      "grabbed/drop_confirm", "grabbed/sell_confirm", bag::kRegisterDialog }) {
                Frame f;
                if (TemplateStore::load_one(name, exe_relative("configs/templates.ini"), f)) pictures.put(name, f);
            }
            known = bag::can_check(pictures, TrashList::Action::Use) ? 1 : 0;
        }
    }
    return known == 1;
}

/// Whether the charm run can be held to its rules here: the pictures it tells
/// questions apart by and the close buttons it looks for another window with,
/// asked once, the registration question's and the close buttons only when they
/// are files beside GoBot.
bool can_charm() {
    // A yes is kept; a no is asked again every few seconds, so that pictures
    // copied in while GoBot runs are noticed.
    static int known = -1;
    static std::uint64_t asked_ms = 0;
    if (known == 1) return true;
    if (known < 0 || now_ms() - asked_ms > 5000) {
        asked_ms = now_ms();
        known = 0;
        // The digits the run reads the stack with are part of it.
        if (beside_gobot(bag::kRegisterDialog) && !GlyphFont::default_dir().empty()) {
            TemplateStore pictures;
            for (const char* name : { "grabbed/confirm_button", "grabbed/del_item_confirm_1",
                                      "grabbed/del_item_confirm_2", "grabbed/hoisinh_confirm",
                                      "grabbed/drop_confirm", "grabbed/sell_confirm", bag::kRegisterDialog,
                                      bag::kCloseButtons[0], bag::kCloseButtons[1], bag::kCloseButtons[2] }) {
                Frame f;
                if (TemplateStore::load_one(name, exe_relative("configs/templates.ini"), f)) pictures.put(name, f);
            }
            known = bag::can_check_charm(pictures) ? 1 : 0;
        }
    }
    return known == 1;
}

/// What a slot cut from the bag picture may be used as, as a pass decides it for
/// a picture on the Dùng list (bag::fragment_cut, bag::charm_cut): Tiêu Dùng
/// holds potions, scrolls and boxes as well, and a right-click there drinks,
/// reads or opens them. Worked out once for each cut -- the popup asks on every
/// frame it is drawn -- by a fingerprint of its pixels.
UseKind cut_kind(const Frame& cut, const BagLayout& l, const ItemCatalog& catalog) {
    static std::uint64_t known_print = 0;
    static UseKind known = UseKind::None;
    std::uint64_t print = 1469598103934665603ull;
    for (const std::uint8_t b : cut.pixels) {
        print ^= b;
        print *= 1099511628211ull;
    }
    if (print != known_print) {
        known_print = print;
        known = UseKind::None;
        if (cut.valid()) {
            if (bag::fragment_cut(cut, l, catalog)) known = UseKind::Fragment;
            else if (bag::charm_cut(cut, l, catalog)) known = UseKind::Charm;
        }
    }
    return known;
}

/// "15:05" for a time in seconds since 1970, in this PC's own time.
std::string clock_hhmm(std::int64_t at_s) {
    const std::time_t t = static_cast<std::time_t>(at_s);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

const char* const kCharmOption = "Dùng bùa (chuột phải trong ngăn Tiêu Dùng, mỗi giờ một cái)";

} // namespace

bool GuiApp::simple_use_options(const std::string& id, const char* folder) {
    // Fragments and charms only: a right-click on anything else may use it, or
    // equip it, there and then.
    if (folder != nullptr && std::string(folder) == "spend/buff") {
        const bool use = has(simple_clean_.use_ids, id);
        ImGui::PushStyleColor(ImGuiCol_Text, kGood);
        const bool taken = ImGui::Selectable(kCharmOption, use);
        ImGui::PopStyleColor();
        ImGui::PushFont(nullptr, kSmall);
        coloured(kWait, trial_hint(kTrialUse, "Mỗi lần dùng mất một cái bùa và có hiệu lực một giờ.").c_str());
        coloured(kDim, "GoBot dùng mỗi loại bùa một lần rồi chờ hết giờ mới dùng lại, kể cả khi tắt rồi mở "
                       "lại GoBot. Chỉ bấm Xác Nhận khi thấy hộp thoại giống hộp thoại mảnh ghép.");
        if (!simple_can_use_charm())
            coloured(kDim, "GoBot chưa có đủ hình của game để dùng bùa, nên lúc này chỉ báo chỗ có bùa, chưa dùng.");
        else if (!simple_clean_.use_charms)
            coloured(kDim, "Ô \"Dùng bùa\" ở đầu thẻ này đang tắt, nên lúc này chỉ báo chỗ có bùa, chưa dùng.");
        ImGui::PopFont();
        if (!taken) return false;
        take_out(simple_clean_.use_ids, id);
        take_out(simple_clean_.delete_ids, id);
        take_out(simple_clean_.drop_ids, id);
        simple_clean_.use_ids.push_back(id);
        simple_save_ids();
        simple_reload_trash();
        simple_note("Sẽ dùng bùa này mỗi giờ một cái khi ô \"Dùng bùa\" được bật.", 8000);
        ImGui::CloseCurrentPopup();
        return true;
    }
    if (folder == nullptr || std::string(folder) != "spend/fragment") return false;
    const bool use = has(simple_clean_.use_ids, id), del = has(simple_clean_.delete_ids, id);
    int how = 0;            // 1 use, 2 use and then delete
    ImGui::PushStyleColor(ImGuiCol_Text, kGood);
    if (ImGui::Selectable(kUseOption, use && !del)) how = 1;
    if (ImGui::Selectable(kUseThenDelete, use && del)) how = 2;
    ImGui::PopStyleColor();
    ImGui::PushFont(nullptr, kSmall);
    coloured(kWait, trial_hint(kTrialUse, "Mảnh ghép đã đăng ký không lấy lại được. Mảnh đã đăng ký rồi thì "
                                          "không đăng ký lại được.").c_str());
    // A fragment's icon says only which of a monster's nine pieces it is, so
    // two slots drawn alike are mostly two monsters' pieces.
    coloured(kDim, "Bỏ phần còn lại: chỉ xoá số mảnh còn lại ở đúng ô vừa đăng ký. Ô khác cùng hình (thường là "
                   "mảnh cùng số của một ma vật khác) được để lại cho tới lượt chính nó được đăng ký.");
    if (!can_register())
        coloured(kDim, "GoBot chưa có hình hộp thoại đăng ký mảnh ghép hoặc hình Yêu Quái Bách Khoa của game, "
                       "nên lúc này chỉ báo chỗ có mảnh, chưa dùng.");
    else if (!simple_clean_.use_fragments)
        coloured(kDim, "Ô \"Dùng mảnh ghép\" ở đầu thẻ này đang tắt, nên lúc này chỉ báo chỗ có mảnh, chưa dùng.");
    ImGui::PopFont();
    if (how == 0) return false;

    take_out(simple_clean_.use_ids, id);
    take_out(simple_clean_.delete_ids, id);
    take_out(simple_clean_.drop_ids, id);
    simple_clean_.use_ids.push_back(id);
    if (how == 2) simple_clean_.delete_ids.push_back(id);
    simple_save_ids();
    simple_reload_trash();
    std::string said = how == 2 ? "Sẽ đăng ký mảnh này vào Yêu Quái Bách Khoa, rồi bỏ phần còn lại vào thùng rác."
                                : "Sẽ đăng ký mảnh này vào Yêu Quái Bách Khoa.";
    // A picture of this very piece cut from the bag earlier for the list of
    // things to get rid of: a slot holding it is registered now, not deleted
    // whole (TrashList::judge). Said, rather than the picture taken off the
    // player's list behind their back -- "Không bỏ món này" brings it back
    // into force.
    int older = 0;
    for (const std::size_t i : simple_trash_.yielding_to_use()) {
        const TrashList::Item& it = simple_trash_.items[i];
        if (!it.file.empty() && has(it.kin, id)) ++older;
    }
    if (older > 0)
        said += " " + std::to_string(older) + " hình chụp từ túi đồ trong danh sách đồ bỏ đi cũng là mảnh này: ô có "
                "mảnh này sẽ được đăng ký trước, không bị xoá cả chồng. Không cần hình đó nữa thì bấm × ở nó.";
    simple_note(said, older > 0 ? 15000 : 8000);
    ImGui::CloseCurrentPopup();
    return true;
}

bool GuiApp::simple_use_cut_option(const ScreenRect& at) {
    // Only what was photographed on Tiêu Dùng: the tab the pass right-clicks
    // on, and the only one where a right-click is no equipping.
    if (simple_bag_view_.selected_tab != kUseTab) return false;
    // And only a fragment: the rest of Tiêu Dùng is used by a right-click
    // there and then. The pass would leave such a picture alone anyway; it is
    // not offered, so that it is never on the list.
    const Frame cut = bag::cut(simple_bag_frame_, at);
    const UseKind kind = cut_kind(cut, simple_clean_.layout, simple_catalog_);
    if (kind == UseKind::None) {
        ImGui::Selectable("Dùng (chỉ cho mảnh ghép và bùa)", false, ImGuiSelectableFlags_Disabled);
        ImGui::SetItemTooltip("Món này không giống mảnh ghép hay bùa nào. Chuột phải vào món khác ở Tiêu Dùng là "
                              "dùng luôn món đó: uống bình, mở hộp, đọc giấy.");
        return false;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kGood);
    const bool taken = ImGui::Selectable(kind == UseKind::Charm ? kCharmOption : kUseOption);
    ImGui::PopStyleColor();
    if (!taken) return false;
    const std::string file = simple_trash_.add(exe_relative("configs/trash"), cut,
                                               TrashList::Action::Use, simple_clean_.layout);
    simple_reload_trash();
    simple_note(file.empty() ? std::string("Không lưu được hình món đồ.")
                : kind == UseKind::Charm
                    ? std::string("Đã thêm vào danh sách Dùng: GoBot sẽ dùng bùa này mỗi giờ một cái.")
                    : std::string("Đã thêm vào danh sách Dùng: GoBot sẽ đăng ký nó vào Yêu Quái Bách Khoa."),
                8000);
    ImGui::CloseCurrentPopup();
    return true;
}

void GuiApp::simple_use_switch(const std::function<void(const char*, const std::string&)>& put) {
    // Off as shipped, and the picture of the question being there switches
    // nothing on: only this does. Shown with the tab's other settings, on its
    // own line, whether or not anything is on the Dùng list yet.
    if (ImGui::Checkbox("Dùng mảnh ghép", &simple_clean_.use_fragments))
        put("use_fragments", simple_clean_.use_fragments ? "true" : "false");
    ImGui::SetItemTooltip("%s", trial_hint(kTrialUse,
                                           "Đăng ký vào Yêu Quái Bách Khoa các mảnh ghép trong danh sách Dùng: chuột "
                                           "phải vào mảnh, Xác Nhận đúng câu hỏi đăng ký của game, rồi đóng Yêu Quái "
                                           "Bách Khoa bằng nút X. Mảnh đã đăng ký không lấy lại được. Tắt thì chỉ báo "
                                           "chỗ có mảnh, không bấm gì.")
                                    .c_str());
    if (simple_clean_.use_fragments && !can_register()) {
        ImGui::SameLine();
        coloured(kWait, "GoBot chưa có hình của game để làm việc này, nên chưa dùng.");
    }
}

bool GuiApp::simple_can_use_charm() {
    return can_charm();
}

void GuiApp::simple_charm_switch(const std::function<void(const char*, const std::string&)>& put) {
    // Its own switch and off as shipped, like the fragments': a charm costs one
    // and lasts an hour, and GoBot uses it by itself while the bot runs.
    if (ImGui::Checkbox("Dùng bùa", &simple_clean_.use_charms)) {
        put("use_charms", simple_clean_.use_charms ? "true" : "false");
        // The worker does the charms between its passes, so it runs for them
        // too: started with the bot, ended when neither is ticked.
        if (simple_clean_.use_charms && simple_running()) start_trash();
        if (!simple_clean_.use_charms && !simple_clean_.enabled && !simple_clean_.buy_potions && trash_running_ && !trash_oneshot_) stop_trash();
    }
    ImGui::SetItemTooltip("%s", trial_hint(kTrialUse,
                                           "Dùng các bùa trong danh sách Dùng: chuột phải vào bùa ở ngăn Tiêu Dùng, "
                                           "Xác Nhận đúng hộp thoại giống của mảnh ghép. Mỗi lần mất một cái, có "
                                           "hiệu lực một giờ, nên mỗi loại chỉ dùng một cái rồi chờ hết giờ. Trong "
                                           "lúc dùng bùa, kỹ năng, bình và buff tạm dừng vài giây (tối đa 20 giây "
                                           "nếu nhiều loại bùa); không dùng khi máu thấp, đang chat hay bot đang "
                                           "bận việc khác. Tắt thì chỉ báo chỗ có bùa, không bấm gì.")
                                    .c_str());
    ImGui::SameLine(0, 12);
    label("dùng lại sau");
    ImGui::SameLine();
    {
        // Not under the hour a charm lasts: a second one would be spent on top
        // of the first.
        static const int kMinutes[] = { 60, 75, 90, 120, 180 };
        const std::string shown = std::to_string(simple_clean_.charm_every_minutes) + " phút";
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##charmevery", shown.c_str())) {
            for (const int m : kMinutes) {
                const std::string item = std::to_string(m) + " phút";
                if (ImGui::Selectable(item.c_str(), m == simple_clean_.charm_every_minutes)) {
                    simple_clean_.charm_every_minutes = m;
                    put("charm_every_minutes", std::to_string(m));
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Bùa có hiệu lực bao lâu thì chờ bấy lâu mới dùng cái cùng loại tiếp theo. Bùa của "
                              "game này kéo dài 1 giờ.");
    }
    if (simple_clean_.use_charms && !can_charm()) {
        ImGui::SameLine();
        coloured(kWait, "GoBot chưa có hình của game để làm việc này, nên chưa dùng.");
    }
    const std::string line = simple_charm_line();
    if (!line.empty()) hint(line.c_str());
}

std::string GuiApp::simple_charm_line() {
    // Re-read now and then, not on every frame: it is a file.
    const std::uint64_t now = now_ms();
    if (simple_charm_line_ms_ != 0 && now - simple_charm_line_ms_ < 5000) return simple_charm_line_;
    simple_charm_line_ms_ = now;
    simple_charm_line_.clear();
    std::vector<std::string> ids;
    for (const TrashList::Item& it : simple_trash_.items) {
        if (it.action != TrashList::Action::Use || !it.charm()) continue;
        const std::string cid = it.clock_id();
        if (std::find(ids.begin(), ids.end(), cid) == ids.end()) ids.push_back(cid);
    }
    if (ids.empty()) return simple_charm_line_;
    CharmClock clock;
    clock.load();
    const std::int64_t wall = static_cast<std::int64_t>(std::time(nullptr));
    std::int64_t soonest = 0;
    int waiting = 0;
    for (const std::string& cid : ids) {
        const std::int64_t next = clock.next(cid);
        if (next > wall) {
            ++waiting;
            if (soonest == 0 || next < soonest) soonest = next;
        }
    }
    simple_charm_line_ = std::to_string(ids.size()) + " loại bùa trong danh sách";
    if (waiting > 0)
        simple_charm_line_ += ", " + std::to_string(waiting) + " đang chờ (đã dùng, hoặc thử lại sau), loại sớm "
                              "nhất tới lượt lúc " + clock_hhmm(soonest);
    else
        simple_charm_line_ += ", chưa loại nào đang chờ";
    simple_charm_line_ += ".";
    return simple_charm_line_;
}

std::string GuiApp::trash_use_said_vi(const BagCleaner::Result& r, bool look_only) {
    std::string out;
    if (look_only) {
        if (r.use_matched > 0) out += " Thấy " + std::to_string(r.use_matched) + " ô sẽ dùng.";
        return out;
    }
    if (r.charms_used > 0) out += " Đã dùng " + std::to_string(r.charms_used) + " bùa.";
    if (r.used > 0) out += " Đã dùng " + std::to_string(r.used) + " món (đăng ký vào Yêu Quái Bách Khoa).";
    if (r.use_unsure > 0)
        out += " " + std::to_string(r.use_unsure) + " món đã bấm dùng mà không chắc đã đăng ký, nên dừng lại.";
    if (r.use_matched > 0) out += " " + std::to_string(r.use_matched) + " ô có món để dùng mà chưa dùng.";
    return out;
}

} // namespace gobot
