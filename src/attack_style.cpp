#include "gobot/attack_style.hpp"

#include <algorithm>

namespace gobot {
namespace {

// sample_config.ini, beside GoC's go_client.exe.
const RangePreset kNearWide   { "Near_Wide",   120, 200, 131, 80, 31, 31 };
const RangePreset kNearNarrow { "Near_Narrow", 120, 200,  63, 63, 31, 31 };
const RangePreset kFarWide    { "Far_Wide",    150, 350, 150, 63, 63, 63 };
const RangePreset kFarNarrow  { "Far_Narrow",  150, 350,  63, 63, 31, 31 };

/// The legacy ranges: HunterConfig's own.
constexpr int kLegacyReach = 150;
constexpr int kLegacyHeight = 60;

} // namespace

const char* const kAttackStyleGuideVi =
    "Chọn nghề và vũ khí: bot lấy tầm đánh theo 4 bộ số của GoC.\n"
    "Hướng dẫn config theo bộ lọc (của GoC):\n"
    "- Gần & Rộng: Võ sĩ Đao, Luân, Xạ thủ Pháo (level cao)\n"
    "- Gần & Hẹp: Võ sĩ Kiếm (lv thấp), Xạ thủ Pháo (lv thấp), Vuốt, Rìu\n"
    "- Xa & Rộng: Đạo sĩ\n"
    "- Xa & Hẹp: Võ sĩ Kiếm (lv cao), Xạ thủ Cung, Tiêu\n"
    "\n"
    "Khi đã chọn một kiểu, bot đánh theo luật của GoC: tầm trên và tầm dưới riêng, "
    "đánh một con tới khi nó chết (chỉ bỏ khi nó ở ngoài tầm quá lâu, hoặc đánh 15 "
    "giây mà không thấy mất máu), và chỉ quay người khi chưa quay về phía quái.\n"
    "Như trước: đánh như GoBot từ trước tới giờ.";

const std::vector<AttackStyle>& attack_styles() {
    static const std::vector<AttackStyle> kStyles = {
        { "vo_si_dao",        "Võ Sĩ – Đao",               &kNearWide },
        { "xa_thu_phao_cao",  "Xạ Thủ – Pháo (cấp cao)",   &kNearWide },
        { "luan",             "Luân",                      &kNearWide },
        { "vo_si_kiem_thap",  "Võ Sĩ – Kiếm (cấp thấp)",   &kNearNarrow },
        { "xa_thu_phao_thap", "Xạ Thủ – Pháo (cấp thấp)",  &kNearNarrow },
        { "vuot",             "Vuốt",                      &kNearNarrow },
        { "riu",              "Rìu",                       &kNearNarrow },
        { "dao_si",           "Đạo Sĩ – Quạt hoặc Trượng", &kFarWide },
        { "vo_si_kiem_cao",   "Võ Sĩ – Kiếm (cấp cao)",    &kFarNarrow },
        { "xa_thu_cung",      "Xạ Thủ – Cung",             &kFarNarrow },
        { "tieu",             "Tiêu",                      &kFarNarrow },
        { "near_wide",        "Gần – Rộng",                &kNearWide },
        { "near_narrow",      "Gần – Hẹp",                 &kNearNarrow },
        { "far_wide",         "Xa – Rộng",                 &kFarWide },
        { "far_narrow",       "Xa – Hẹp",                  &kFarNarrow },
    };
    return kStyles;
}

const AttackStyle* find_attack_style(const std::string& id) {
    if (id.empty()) return nullptr;
    for (const AttackStyle& s : attack_styles())
        if (id == s.id) return &s;
    return nullptr;
}

void apply_attack_style(HunterConfig& cfg, const AttackStyle& style) {
    const RangePreset& r = *style.ranges;
    cfg.style = style.id;
    cfg.min_distance = r.min_range;
    cfg.reach = r.max_range;
    cfg.up_reach = r.highest_hit_y;
    cfg.down_reach = r.lowest_hit_y;
    cfg.up_no_jump = r.higher_threshold;
    cfg.down_no_jump = r.lower_threshold;
    cfg.height_tolerance = std::max(r.highest_hit_y, r.lowest_hit_y);
}

void apply_legacy_style(HunterConfig& cfg) {
    cfg.style.clear();
    cfg.reach = kLegacyReach;
    cfg.height_tolerance = kLegacyHeight;
    cfg.up_reach = cfg.down_reach = cfg.up_no_jump = cfg.down_no_jump = -1;
    cfg.min_distance = 0;
}

bool attack_style_matches(const HunterConfig& cfg, const AttackStyle& style) {
    const RangePreset& r = *style.ranges;
    return cfg.min_distance == r.min_range && cfg.reach == r.max_range &&
           cfg.up() == r.highest_hit_y && cfg.down() == r.lowest_hit_y &&
           cfg.up_nj() == r.higher_threshold && cfg.down_nj() == r.lower_threshold;
}

bool legacy_ranges(const HunterConfig& cfg) {
    return cfg.reach == kLegacyReach && cfg.min_distance == 0 && cfg.up() == kLegacyHeight &&
           cfg.down() == kLegacyHeight && cfg.up_nj() == kLegacyHeight / 2 &&
           cfg.down_nj() == kLegacyHeight / 2;
}

} // namespace gobot
