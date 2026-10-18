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

/// Whether the registration question can be told from the others here, asked
/// once: the few pictures can_check(Use) wants, loaded one by one rather than
/// the thousand others -- and the registration's only when it is a file
/// beside GoBot, never looked for in another program's assembly.
bool can_register() {
    static int known = -1;
    if (known < 0) {
        known = 0;
        std::string file = TemplateStore::default_dir() + "\\" + bag::kRegisterDialog + ".png";
        std::replace(file.begin(), file.end(), '/', '\\');
        if (path_exists(file)) {
            TemplateStore pictures;
            for (const char* name : { "grabbed/confirm_button", "grabbed/del_item_confirm_1",
                                      "grabbed/del_item_confirm_2", bag::kRegisterDialog }) {
                Frame f;
                if (TemplateStore::load_one(name, exe_relative("configs/templates.ini"), f)) pictures.put(name, f);
            }
            known = bag::can_check(pictures, TrashList::Action::Use) ? 1 : 0;
        }
    }
    return known == 1;
}

} // namespace

bool GuiApp::simple_use_options(const std::string& id, const char* folder) {
    // Fragments only: a right-click on anything else may use it, or equip it,
    // there and then.
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
    if (!can_register())
        coloured(kDim, "GoBot chưa có hình hộp thoại đăng ký mảnh ghép của game, nên lúc này chỉ báo chỗ có "
                       "mảnh, chưa dùng.");
    ImGui::PopFont();
    if (how == 0) return false;

    take_out(simple_clean_.use_ids, id);
    take_out(simple_clean_.delete_ids, id);
    take_out(simple_clean_.drop_ids, id);
    simple_clean_.use_ids.push_back(id);
    if (how == 2) simple_clean_.delete_ids.push_back(id);
    simple_save_ids();
    simple_reload_trash();
    simple_note(how == 2 ? "Sẽ đăng ký mảnh này vào Yêu Quái Bách Khoa, rồi bỏ phần còn lại vào thùng rác."
                         : "Sẽ đăng ký mảnh này vào Yêu Quái Bách Khoa.",
                8000);
    ImGui::CloseCurrentPopup();
    return true;
}

bool GuiApp::simple_use_cut_option(const ScreenRect& at) {
    // Only what was photographed on Tiêu Dùng: the tab the pass right-clicks
    // on, and the only one where a right-click is no equipping.
    if (simple_bag_view_.selected_tab != kUseTab) return false;
    ImGui::PushStyleColor(ImGuiCol_Text, kGood);
    const bool taken = ImGui::Selectable(kUseOption);
    ImGui::PopStyleColor();
    if (!taken) return false;
    const std::string file = simple_trash_.add(exe_relative("configs/trash"), bag::cut(simple_bag_frame_, at),
                                               TrashList::Action::Use, simple_clean_.layout);
    simple_reload_trash();
    simple_note(file.empty() ? std::string("Không lưu được hình món đồ.")
                             : std::string("Đã thêm vào danh sách Dùng: GoBot sẽ đăng ký nó vào Yêu Quái Bách Khoa."),
                8000);
    ImGui::CloseCurrentPopup();
    return true;
}

std::string GuiApp::trash_use_said_vi(const BagCleaner::Result& r, bool look_only) {
    std::string out;
    if (look_only) {
        if (r.use_matched > 0) out += " Thấy " + std::to_string(r.use_matched) + " ô sẽ dùng.";
        return out;
    }
    if (r.used > 0) out += " Đã dùng " + std::to_string(r.used) + " món (đăng ký vào Yêu Quái Bách Khoa).";
    if (r.use_matched > 0) out += " " + std::to_string(r.use_matched) + " ô có món để dùng mà chưa dùng.";
    return out;
}

} // namespace gobot
