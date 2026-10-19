#pragma once

// "Kiểu đánh": the attack styles the simple screen offers, and the numbers
// each one gives the hunter.
//
// GoC's "Config Mẫu" asks for a class and a weapon and loads one of four sets
// of ranges from the sample_config.ini beside it -- Near_Wide, Near_Narrow,
// Far_Wide and Far_Narrow. The numbers depend on how far the weapon reaches
// and how wide a band it sweeps, which is nothing a player can be expected to
// work out, so they are taken from that file as they are. GoC's numbers are
// its map units and the hunter's are screen pixels between name plates; they
// are taken as the same (inferred, to be checked live).

#include "gobot/hunter.hpp"

#include <string>
#include <vector>

namespace gobot {

/// One of GoC's four sets of ranges, as sample_config.ini has them.
struct RangePreset {
    const char* name;           ///< its section in sample_config.ini
    int min_range;              ///< minRange: the closest a target should be -> min_distance
    int max_range;              ///< maxRange: how far it is attacked from -> reach
    int highest_hit_y;          ///< highestHitY: how far above it can be hit -> up_reach
    int lowest_hit_y;           ///< lowestHitY: how far below -> down_reach
    int lower_threshold;        ///< lowerThreshold: below, without jumping down -> down_no_jump
    int higher_threshold;       ///< higherThreshold: above, without jumping up -> up_no_jump
};

/// An entry of the "Kiểu đánh" picker: a class and weapon, or one of the
/// four sets by its own name.
struct AttackStyle {
    const char*        id;          ///< ASCII, as hunt.ini keeps it in style=
    const char*        label_vi;    ///< what the picker shows
    const RangePreset* ranges;
};

/// Every style, in the order the picker shows them: the classes and weapons
/// first, then the four sets by name. "Như trước" -- no style -- is not one
/// of them.
const std::vector<AttackStyle>& attack_styles();

/// The style with this id, or null: an empty id is "Như trước", and an id
/// from some other build is no style either.
const AttackStyle* find_attack_style(const std::string& id);

/// Choose `style`: its six numbers into the hunter's config, and its id,
/// which turns GoC's rules on (HunterConfig::goc_rules). height_tolerance
/// becomes the larger of up and down, for an older GoBot reading the file.
void apply_attack_style(HunterConfig& cfg, const AttackStyle& style);

/// "Như trước": no style, and the ranges the hunter always had -- reach 150,
/// height 60 either way, no minimum distance.
void apply_legacy_style(HunterConfig& cfg);

/// Whether `cfg` still has `style`'s numbers. Once one is edited by hand the
/// style stays and the picker says "Tự chỉnh".
bool attack_style_matches(const HunterConfig& cfg, const AttackStyle& style);

/// Whether `cfg` has the ranges of "Như trước".
bool legacy_ranges(const HunterConfig& cfg);

/// What the picker calls no style, and edited numbers.
inline constexpr const char* kLegacyStyleVi = "Như trước";
inline constexpr const char* kCustomStyleVi = "Tự chỉnh";

/// GoC's guide to which set suits which class and weapon, in its words, and
/// what choosing a style changes: the picker's "(?)".
extern const char* const kAttackStyleGuideVi;

} // namespace gobot
