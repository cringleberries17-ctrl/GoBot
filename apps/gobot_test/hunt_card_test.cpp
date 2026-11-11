// gobot_test -- what GoBot's window says and draws of "Nhảy lên bục trên"
// and "Tự tìm quái trong vùng" (2026.11.05b): why a tick cannot work as set,
// which of the card's lines hold (hunt_card.hpp), the Bản đồ tab's jump arrow,
// the seek's grey marks and the floors found solid from below (map_view.hpp)
// -- pure, on made-up statuses and the game's own t52_s2 -- and, where only
// the GUI's source says it, its source: the ticks greyed for their reasons,
// on by default, the full screen's boxes, the keys GoBot lists and its veto,
// the card's calls, the tab's drawing, "Thử ↑ / ↑↑ / Ctrl+↑", the spot's
// lines, and the shipped hunt.ini.
//
// Nothing here presses a key or reads the game: the window's code is read as
// text, and the maps are copies in build\handoff\drop\testdata -- skipped when
// they are not there.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/hunt_card.hpp"
#include "gobot/hunter.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/map_book.hpp"
#include "gobot/map_view.hpp"
#include "gobot/packet_world.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

template <typename... Args>
std::string fmt(const char* f, Args... args) {
    char buf[600];
    std::snprintf(buf, sizeof buf, f, args...);
    return buf;
}

/// A config every tick works with: the packets on, the spot on, ↑ the jump
/// key, the arrows and Z.
HunterConfig ready() {
    HunterConfig c;
    c.packet_positions = PacketPositions::On;
    c.spot = true;
    c.stay_put = false;
    c.jump_key = 0x26;
    c.left_key = 0x25;
    c.right_key = 0x27;
    c.attack_key = 'Z';
    c.jump_up = true;
    c.seek_in_zone = true;
    return c;
}

// ------------------------------------------------------------- the ticks

void test_ticks() {
    section("Hunt card: why Nhảy lên bục trên and Tự tìm quái trong vùng cannot work as set -- in the hunter's order");

    const std::uint32_t own[] = { 121, 123, 122, 0, 0 };
    HunterConfig c = ready();
    check(jump_up_tick(c, own) == JumpUpTick::Ok && seek_tick(c) == SeekTick::Ok && !jump_up_no_ctrl(c),
          "packets on, the spot on, ↑ the jump key, F10/F12/F11 GoBot's own: both work");

    HunterConfig a = c;
    a.stay_put = true;
    a.packet_positions = PacketPositions::Watch;
    a.spot = false;
    a.jump_key = 0;
    check(jump_up_tick(a, own) == JumpUpTick::StayPut && seek_tick(a) == SeekTick::StayPut,
          "Đứng yên first, whatever else is wrong");
    a.stay_put = false;
    check(jump_up_tick(a, own) == JumpUpTick::NoPackets && seek_tick(a) == SeekTick::NoPackets,
          "then the packets not on (Watch)");
    a.packet_positions = PacketPositions::Off;
    check(jump_up_tick(a, own) == JumpUpTick::NoPackets, "... or off");
    a.packet_positions = PacketPositions::On;
    check(jump_up_tick(a, own) == JumpUpTick::NoSpot && seek_tick(a) == SeekTick::NoSpot,
          "then Chỗ đánh quái not ticked: no area of the map tab used");
    a.spot = true;
    check(jump_up_tick(a, own) == JumpUpTick::JumpKey && seek_tick(a) == SeekTick::Ok,
          "then no jump key -- the jumps' alone: the seek works");
    a.jump_key = 32;
    check(jump_up_tick(a, own) == JumpUpTick::JumpKey, "Space as the jump key: not ↑");
    a.jump_key = 0x26;
    check(jump_up_tick(a, own) == JumpUpTick::Ok, "↑: works");

    std::size_t at = 99;
    const std::uint32_t ctrl[] = { 121, 0x11, 122, 0, 0 };
    const std::uint32_t right_ctrl[] = { 121, 123, 122, 0xA3, 0 };
    const std::uint32_t up[] = { 121, 123, 122, 0, 0x26 };
    const std::uint32_t down[] = { 121, 123, 0x28, 0, 0 };
    check(jump_up_tick(c, ctrl, &at) == JumpUpTick::OwnKey && at == 1 &&
              jump_up_tick(c, right_ctrl, &at) == JumpUpTick::OwnKey && at == 3 &&
              jump_up_tick(c, up, &at) == JumpUpTick::OwnKey && at == 4,
          "one of GoBot's own keys on Ctrl (either side) or ↑: refused, and which one said");
    check(jump_up_tick(c, down) == JumpUpTick::Ok, "one on ↓ is the drop's to refuse, not the jumps'");

    HunterConfig k = c;
    k.attack_key = 0x11;
    check(jump_up_no_ctrl(k) && jump_up_tick(k, own) == JumpUpTick::Ok,
          "the attack key Ctrl: only Ctrl + ↑ left out (as Hunter::jump_rules); the tick still works");
    k = c;
    k.left_key = 0xA2;
    check(jump_up_no_ctrl(k), "the left key left Ctrl: the same");

    check(HunterConfig{}.jump_up && HunterConfig{}.seek_in_zone, "both on by default (the owner's answer 5, the seek's Q7)");
}

// -------------------------------------------------------------- the card

void test_card() {
    section("Hunt card: the jump up's lines -- off in red, at work, no sure jump a moment ago, the dry run, the counts");

    HunterConfig c = ready();
    Hunter::Status hs;
    JumpUpCard k = jump_up_card(c, hs, 10'000);
    check(k.off == Hunter::JumpUpOff::None && k.kinds_off == 0 && k.now == JumpUpCard::Now::None && !k.would &&
              !k.counts,
          "a run's start: nothing to say");

    hs.jump_off = Hunter::JumpUpOff::Fails;
    check(jump_up_card(c, hs, 10'000).off == Hunter::JumpUpOff::Fails, "3 in a row that did not get up: off, said");
    hs.jump_off = Hunter::JumpUpOff::Physics;
    check(jump_up_card(c, hs, 10'000).off == Hunter::JumpUpOff::Physics, "landed higher than planned: said");
    hs.jump_off = Hunter::JumpUpOff::MapChanged;
    const bool changed = jump_up_card(c, hs, 10'000).off == Hunter::JumpUpOff::None;
    hs.jump_off = Hunter::JumpUpOff::KeysStuck;
    check(changed && jump_up_card(c, hs, 10'000).off == Hunter::JumpUpOff::None,
          "a map change or a stuck key: not here -- the hunt stopped, said in red under BẮT ĐẦU");
    hs.jump_off = Hunter::JumpUpOff::None;
    hs.jump_kinds_off = 1u << static_cast<unsigned>(JumpKind::CtrlUp);
    check(jump_up_card(c, hs, 10'000).kinds_off == 4, "Ctrl + ↑ off for the run: said");
    hs.jump_kinds_off = 0;

    hs.jump_phase = Hunter::JumpUpPhase::Approach;
    const bool approach = jump_up_card(c, hs, 10'000).now == JumpUpCard::Now::Approach;
    hs.jump_phase = Hunter::JumpUpPhase::Settle;
    const bool settle = jump_up_card(c, hs, 10'000).now == JumpUpCard::Now::Settle;
    hs.jump_phase = Hunter::JumpUpPhase::Air;
    check(approach && settle && jump_up_card(c, hs, 10'000).now == JumpUpCard::Now::Air,
          "walking to the take-off, settled there, in the air");

    // No sure jump to a monster above, a moment ago -- the tick on, not on
    // the way back, and only while nothing is under way.
    hs.jump_wait = Hunter::JumpUpWhy::Plan;
    hs.jump_wait_no = JumpNo::Short;
    hs.jump_wait_px = 520;
    hs.jump_wait_ms = 9'000;
    hs.jump_wait_for = Hunter::JumpUpFor::Target;
    check(jump_up_card(c, hs, 10'000).now == JumpUpCard::Now::Air, "in the air: that, not the last refusal");
    hs.jump_phase = Hunter::JumpUpPhase::Idle;
    const bool target = jump_up_card(c, hs, 10'000).now == JumpUpCard::Now::NoPlan;
    hs.jump_wait_for = Hunter::JumpUpFor::Seek;
    const bool sought = jump_up_card(c, hs, 10'000).now == JumpUpCard::Now::NoPlan;
    hs.jump_wait_for = Hunter::JumpUpFor::Home;
    const bool home = jump_up_card(c, hs, 10'000).now == JumpUpCard::Now::None;
    hs.jump_wait_for = Hunter::JumpUpFor::Target;
    check(target && sought && home,
          "no sure jump to a monster above, or one sought: said; on the way back the spot's line says it");
    const bool stale = jump_up_card(c, hs, 9'000 + kJumpUpCardFreshMs + 1).now == JumpUpCard::Now::None;
    HunterConfig off = c;
    off.jump_up = false;
    hs.jump_wait = Hunter::JumpUpWhy::MinuteCap;
    const bool other = jump_up_card(c, hs, 10'000).now == JumpUpCard::Now::None;
    hs.jump_wait = Hunter::JumpUpWhy::Plan;
    check(stale && other && jump_up_card(off, hs, 10'000).now == JumpUpCard::Now::None,
          fmt("said while %llu ms fresh, for the planner's reason alone, and never with the tick off",
              static_cast<unsigned long long>(kJumpUpCardFreshMs)));

    hs.jumps_would = 4;
    check(jump_up_card(off, hs, 10'000).would && !jump_up_card(c, hs, 10'000).would,
          "the tick off with jumps it would have made: the grey line; ticked, none");
    hs.jumps_would = 0;
    check(!jump_up_card(off, hs, 10'000).would, "the tick off with none: nothing");
    hs.jumps = 3;
    check(jump_up_card(c, hs, 10'000).counts && jump_up_card(off, hs, 10'000).counts, "presses this run: counted");

    // The seek's monsters above: why not jumped to.
    Hunter::Status s;
    check(seek_above(c, s) == SeekAbove::None, "none above with no way up: nothing to say");
    s.seek_above_no_way = 2;
    check(seek_above(off, s) == SeekAbove::TickOff && seek_above(c, s) == SeekAbove::Latched,
          "two above only a jump up gets to: the tick off -- tick it; ticked, jumps up are off for the run");
}

// ------------------------------------------------------------ the map tab

void test_arrow_and_marks() {
    section("Hunt card: the Bản đồ tab's jump arrow -- under way, or would-have-gone while fresh -- and the seek's "
            "grey marks");

    Hunter::Status hs;
    check(!jump_arrow(hs, 1'000), "no jump, none that would have gone: no arrow");
    hs.jump_phase = Hunter::JumpUpPhase::Approach;
    hs.jump_kind = JumpKind::CtrlUp;
    hs.jump_x = 802;
    hs.jump_feet_y = 633;
    hs.jump_land_x = 802;
    hs.jump_land_top = 275;
    std::optional<JumpArrow> a = jump_arrow(hs, 1'000);
    check(a && !a->would && a->kind == JumpKind::CtrlUp && a->take.x == 802 && a->take.y == 633 && a->land.x == 802 &&
              a->land.y == 274,
          "walking to the take-off: from 802,633 onto 802,274 (the floor's top less 1), Ctrl + ↑");

    hs.jump_phase = Hunter::JumpUpPhase::Idle;
    hs.jump_would_plan.kind = JumpKind::CtrlUp;
    hs.jump_would_plan.x = 810;
    hs.jump_would_plan.feet_y = 603;
    hs.jump_would_plan.land_x = 809;
    hs.jump_would_plan.land_top = 274;
    hs.jump_would_ms = 5'000;
    a = jump_arrow(hs, 6'000);
    check(a && a->would && a->take.x == 810 && a->take.y == 603 && a->land.x == 809 && a->land.y == 273,
          "the tick off: the jump that would have gone, a moment ago, dashed");
    check(!jump_arrow(hs, 5'000 + kJumpArrowFreshMs + 1) && !jump_arrow(hs, 4'000),
          fmt("not past %llu ms, nor from a clock behind it",
              static_cast<unsigned long long>(kJumpArrowFreshMs)));

    // The seek's marks, by slot and generation.
    Hunter::Status s;
    s.seek_active = true;
    Hunter::Status::SeekGrey above;
    above.slot = 7;
    above.gen = 3;
    above.why = SeekNo::NeedsJumpOff;
    Hunter::Status::SeekGrey passed;
    passed.slot = 9;
    passed.gen = 1;
    passed.why = SeekNo::Passed;
    passed.left_ms = 12'000;
    s.seek_grey = { above, passed };
    PacketWorld::Mob m;
    m.slot = 7;
    m.gen = 3;
    const Hunter::Status::SeekGrey* g = seek_grey_of(s, m);
    check(g != nullptr && g->why == SeekNo::NeedsJumpOff, "slot 7 #3 on a floor above with no way up: greyed, why said");
    m.gen = 4;
    check(seek_grey_of(s, m) == nullptr, "slot 7 respawned (#4): not that one");
    m.slot = 9;
    m.gen = 1;
    g = seek_grey_of(s, m);
    check(g != nullptr && g->why == SeekNo::Passed && g->left_ms == 12'000, "slot 9 passed over: 12 s left");
    s.seek_on = true;
    s.seek_slot = 9;
    s.seek_gen = 1;
    check(seek_grey_of(s, m) == nullptr, "the one gone for: never grey");
    m.slot = 7;
    m.gen = 3;
    m.dead = true;
    check(seek_grey_of(s, m) == nullptr, "a dead one: not the seek's");
    m.dead = false;
    s.seek_active = false;
    check(seek_grey_of(s, m) == nullptr, "the seek not on at the last pass: nothing of it");
}

/// The game's files copied for the tests, or "" -- never the game's install.
std::string game_data() {
#ifdef GOBOT_SOURCE_DIR
    const std::string dir = std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data";
    if (path_exists(dir + "\\Map\\t52_s2.map")) return dir;
#endif
    return {};
}

void test_solid_marks() {
    section("Hunt card: a floor found solid from below drawn end to end -- t52_s2's spawn ledge from 880,274");
    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): the solid floors drawn\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    if (!page) {
        skip("t52_s2 does not read here\n");
        return;
    }
    const MapPoint on[] = { { 880, 274 }, { 880, 100 } };
    const std::vector<MapRun> runs = solid_marks(*page, on);
    bool ordered = !runs.empty();
    if (ordered)
        for (std::size_t i = 1; i < runs[0].pts.size(); ++i) ordered = ordered && runs[0].pts[i].x > runs[0].pts[i - 1].x;
    check(runs.size() == 1 && runs[0].x0 == 472 && runs[0].x1 == 1104 && ordered,
          fmt("the ledge followed both ways, its hole at 711 walked over: x %d..%d, left to right; a point on no floor "
              "left out (%zu runs)",
              runs.empty() ? 0 : runs[0].x0, runs.empty() ? 0 : runs[0].x1, runs.size()));
}

// ------------------------------------------------------------ the source

#ifdef GOBOT_SOURCE_DIR
std::string source(const char* rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string body(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

/// Each of `parts` in `text`, in that order.
bool in_order(const std::string& text, std::initializer_list<const char*> parts) {
    std::size_t at = 0;
    for (const char* p : parts) {
        const std::size_t found = text.find(p, at);
        if (found == std::string::npos) return false;
        at = found + 1;
    }
    return true;
}
#endif

void test_gui_source() {
    section("Hunt card: the window's ticks, card, keys, map tab and Thử for the jumps up and the seek (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string style = source("/apps/gobot_gui/simple_style.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string keys = source("/apps/gobot_gui/simple_hotkeys.cpp");
    const std::string map = source("/apps/gobot_gui/simple_map.cpp");
    const std::string spot = source("/apps/gobot_gui/simple_spot.cpp");
    if (style.empty() || view.empty() || gui.empty() || keys.empty() || map.empty() || spot.empty()) {
        skip("the window's source is not here to read\n");
        return;
    }

    // The ticks: greyed out while they cannot work as set, the reason their
    // tooltip -- from hunt_card's rules.
    const std::string up_tick = body(style, "void GuiApp::simple_jump_up_tick() {");
    const std::string seek = body(style, "void GuiApp::simple_seek_tick() {");
    check(in_order(up_tick, { "const char* const kJumpUp = \"Nhảy lên bục trên (↑ / ↑↑ / Ctrl+↑)\";",
                              "const std::string why = simple_jump_up_refusal_vi();",
                              "ImGui::BeginDisabled(!why.empty() && !hunt_config_.jump_up);",
                              "ImGui::Checkbox(kJumpUp, &hunt_config_.jump_up)", "simple_changed();",
                              "ImGui::EndDisabled();", "trial_hint(kTrialJumpUp,",
                              "ImGui::SetItemTooltip(\"%s\", tip.c_str());" }) &&
              in_order(seek, { "const char* const kSeek = \"Tự tìm quái trong vùng\";",
                               "const std::string why = simple_seek_refusal_vi();",
                               "ImGui::BeginDisabled(!why.empty());",
                               "ImGui::Checkbox(kSeek, &hunt_config_.seek_in_zone)", "simple_changed();",
                               "trial_hint(kTrialSeek," }),
          "\"Nhảy lên bục trên (↑ / ↑↑ / Ctrl+↑)\" and \"Tự tìm quái trong vùng\": greyed with their reason as the "
          "tooltip, saved when ticked -- a greyed jump up still unticked when ticked");
    check(in_order(gui, { "const std::string why = simple_jump_up_refusal_vi();",
                          "ImGui::BeginDisabled(!why.empty() && !hunt_config_.jump_up);",
                          "ImGui::Checkbox(\"Jump up onto floors above (Up / Up Up / Ctrl+Up)\", &hunt_config_.jump_up);" }),
          "the full screen's jump up box: greyed, but a ticked one may still be unticked");
    check(has(body(style, "std::string GuiApp::simple_jump_up_refusal_vi() {"), "jump_up_tick(hunt_config_, own.vk, &at)") &&
              has(body(style, "std::string GuiApp::simple_seek_refusal_vi() {"), "seek_tick(hunt_config_)") &&
              has(style, "\"Cần dấu Vị trí từ gói tin.\"") &&
              has(style, "\"Cần dấu Chỗ đánh quái, với vùng đánh quái vẽ ở tab Bản đồ.\"") &&
              has(style, "Phím nhảy phải là ↑") &&
              has(style, "\"Phím đánh hoặc phím đi đang là Ctrl nên GoBot không dùng Ctrl+↑ (↑ và ↑↑ vẫn dùng).\"") &&
              !has(style, "Ctrl + ↑ gần cửa") && !has(style, "Có bục phía trên hoặc cửa gần đây"),
          "their reasons as hunt_card has them -- no gate reason for Ctrl + ↑ (the owner's answer 2)");
    check(in_order(view, { "simple_drop_tick();", "simple_jump_up_tick();", "simple_spot_tick();",
                           "simple_seek_tick();" }) &&
              in_order(view, { "simple_drop_hints();", "simple_jump_up_hints();", "simple_spot_hint();" }),
          "on the Tự đánh quái row: after Nhảy xuống bậc dưới, and after Chỗ đánh quái; their lines under it");

    // The card.
    check(in_order(view, { "case Hunter::State::Approaching:", "if (hs.seek_on)",
                           "coloured(kGood, seek_going_vi(hs).c_str());", "hs.dropping ? " }) &&
              in_order(view, { "case Hunter::State::NoTargets:", "if (hs.seek_none)", "simple_seek_none(hs);",
                               "else if (hs.passed_over)", "else if (hs.outside_area)" }),
          "the card's Approaching line the seek's when it goes to a monster, its NoTargets line when nothing in the "
          "area is got to -- the rest as before");
    check(has(view, "simple_jump_up_state(hs);") &&
              has(view, "std::string(\" Về: nhảy \") + jump_kind_key(hs.spot_way_jump_kind)") &&
              in_order(view, { "simple_spot_state(hs);", "simple_jump_up_state(hs);" }),
          "the card: the jump up's lines after the spot's, and a way back that jumps");
    const std::string lines = body(style, "std::vector<std::pair<bool, std::string>> GuiApp::jump_up_card_vi(");
    check(has(lines, "jump_up_card(hunt_config_, hs, now_ms())") && has(lines, "Nhảy lên đã tắt tới lần BẮT ĐẦU sau: ") &&
              has(lines, "\"Nhảy lên (đang tắt): lẽ ra đã nhảy \"") && has(lines, "\" lần lên tới bục, \"") &&
              has(lines, "\"Nhảy lên: không có chỗ nhảy chắc chắn tới bục của \"") &&
              has(body(style, "void GuiApp::simple_jump_up_state(const Hunter::Status& hs) {"),
                  "if (red) coloured(kBad, text.c_str());"),
          "the jump up's card lines from jump_up_card: off in red, the rest grey -- the dry run's among them");
    check(has(body(style, "std::string GuiApp::seek_going_vi(const Hunter::Status& hs) {"),
              "\"Đang đi tìm quái trong vùng: \"") &&
              has(body(style, "void GuiApp::simple_seek_none(const Hunter::Status& hs) {"),
                  "\"Trong vùng không còn con quái nào tới được — đứng chờ ở đây.\"") &&
              has(style, "\" con ở bục trên: chưa nhảy lên được — bật “Nhảy lên bục trên”.\""),
          "the seek's card lines: going, and nothing got to -- with the jump up to tick for the monsters above");

    // The full screen's boxes, greyed for the same reasons.
    check(in_order(gui, { "ImGui::Checkbox(\"Jump at things above you\", &hunt_config_.jump_to_reach);",
                          "const std::string why = simple_jump_up_refusal_vi();",
                          "ImGui::Checkbox(\"Jump up onto floors above (Up / Up Up / Ctrl+Up)\", &hunt_config_.jump_up);" }) &&
              in_order(gui, { "ImGui::Checkbox(\"Drop to things below you (Ctrl+Down)\", &hunt_config_.drop_down);",
                              "const std::string why = simple_seek_refusal_vi();",
                              "ImGui::Checkbox(\"Seek monsters anywhere in the area\", &hunt_config_.seek_in_zone);" }),
          "the full screen: the jump up's box beside the old hop's, the seek's after the drop's, greyed as the ticks");

    // Ctrl and ↑ in use, and in the veto, with the jumps up on; a map change
    // after one unticks and saves; the stray keys let go of.
    const std::string in_use = body(keys, "std::vector<NamedKey> GuiApp::game_keys_in_use() {");
    check(in_order(in_use, { "if (hunt_config_.jump_up) {", "k.push_back({ VK_CONTROL, \"nhảy lên (Ctrl + ↑)\" });",
                             "k.push_back({ VK_UP, kJumpUpKeyVi });" }) &&
              has(in_use, "if (hunt_config_.drop_down) k.push_back({ VK_CONTROL, \"nhảy xuống (Ctrl + ↓)\" });") &&
              has(body(style, "void GuiApp::simple_jump_checklist() {"), "k.what_vi != kJumpUpKeyVi"),
          "the keys GoBot uses: Ctrl and ↑ with the jumps up on -- the jump key's line leaving that ↑ out");
    const std::string dtick = body(gui, "void GuiApp::drop_tick() {");
    check(has(dtick, "if (hunt_config_.drop_down || hunt_config_.jump_up) {") &&
              has(dtick, "const bool up = hunt_config_.jump_up;") && has(dtick, "(up && low == 0x26u)") &&
              in_order(dtick, { "} else if (stop == Hunter::DropStop::JumpMapChanged) {", "hunt_config_.jump_up = false;",
                                "hunter_->set_config(hunt_config_);",
                                "hunt_config_.save(exe_relative(\"configs/hunt.ini\"))",
                                "drop_stopped_vi_ = std::string(\"Lần nhảy lên lúc \")" }),
          "the veto: a GoBot key on ↑ too with the jumps up on; a map change after a jump: unticked, saved, said in red");
    check(has(body(gui, "void GuiApp::start_hunt() {"),
              "if (hunt_config_.jump_up && !hunt_config_.drop_down) input_.release_stray_drop_keys();") &&
              has(body(gui, "void GuiApp::initialise() {"),
                  "if (hunt_config_.jump_up && !hunt_config_.drop_down) input_.release_stray_drop_keys();") &&
              has(body(gui, "bool GuiApp::any_job_alive() const {"), "jump_kind_running()"),
          "a stray ↑ and Ctrl let go of at GoBot's start and each BẮT ĐẦU; a Thử of a kind counted as a job");

    // "Thử ↑", "Thử ↑↑", "Thử Ctrl + ↑": gated as "Thử", always ↑, the floor
    // above read on the check's own thread.
    const std::string kind = body(style, "void GuiApp::simple_jump_kind_try(JumpKind kind) {");
    check(in_order(kind, { "if (keeper_entering()) {", "if (!keys_may_start()) return;", "map_page_now()",
                           "std::async(std::launch::async,", "eyes.air = [this] { return packet_air_counts(); };",
                           "eyes.floor_above = [this, page]()", "game_maps_.get(", "floor_above(*p,",
                           "JumpCheck::press_with(input_, k, go_on)", "check.run_kind(kind, stop)" }) &&
              !has(kind, "jump_key") && !has(kind, "0x28") && !has(kind, "VK_DOWN"),
          "Thử of a kind: not into the login pages, through keys_may_start, on a thread of its own -- the map read "
          "there -- always ↑ by press_with, never the jump key's setting, never ↓");
    const std::string hints = body(style, "void GuiApp::simple_jump_up_hints() {");
    check(in_order(hints, { "if (busy)", "else if (!input_.window_valid())", "else if (simple_running())",
                            "else if (keeper_entering())", "else if (coord_.critical_active())",
                            "ImGui::BeginDisabled(!wait.empty());", "simple_jump_kind_try(k);" }) &&
              has(body(style, "void GuiApp::simple_style_tick() {"), "ui.kind_said = JumpCheck::kind_said_vi(r);") &&
              has(body(style, "void GuiApp::simple_style_stop() {"), "style_ui_->kind_stop = true;") &&
              has(style, "bool GuiApp::jump_kind_running() const { return style_ui_ && style_ui_->kind.valid(); }"),
          "its buttons greyed while the bot runs, the keeper enters, a hold or another Thử; its result said; F12 "
          "stops it");
    check(!has(hints, "if (!hunt_config_.jump_up) return;") &&
              in_order(hints, { "const std::string why = simple_jump_up_refusal_vi();", "if (!why.empty()) {",
                                "if (hunt_config_.jump_up && hunt_config_.spot)", "return;", "if (hunt_config_.jump_up) {",
                                "label(\"Đo cú nhảy:\");", "simple_jump_kind_try(k);" }),
          "the Đo cú nhảy row wherever the jumps up can work as set, ticked or not -- a map change unticks it, and a "
          "measure may be wanted then");

    // The map tab: the arrow, the seek's line and marks, what was learned.
    const std::string canvas = body(map, "void GuiApp::map_canvas(MapUi& ui, MapView& mv) {");
    check(has(canvas, "jump_arrow(hs, mv.now)") && has(canvas, "arrow(dl, from, to, col, jump->would, lo, hi);") &&
              has(canvas, "(mv.hunt_on && !target && seek_grey_of(hs, m) != nullptr)") &&
              has(canvas, "mob_grey(hunt_config_, hs, area_on, mv.you, mv.you_x, mv.you_y, m, target)") &&
              has(canvas, "dashed_line(dl, me, at, MapColors::kTarget, 1.5f, lo, hi);") &&
              has(canvas, "if (hs.seek_projected) dashed_ring(dl, mob, 8.0f, MapColors::kTarget, 1.5f);") &&
              has(canvas, "for (const MapRun& r : ui.cur.solid_runs) {") && has(canvas, "ui.cur.no_jumps") &&
              has(canvas, "tip += \" · \" + why;") && has(map, "\"Tạm bỏ qua, còn \"") &&
              has(map, "\"Không tới được: bục trên, chưa nhảy lên\""),
          "the map tab: the jump's arrow, the dashed line to the attack point and on to the monster with its ring -- "
          "dashed when projected -- grey for the seek's, what jumps up learned, and why in the hover");
    check(has(map, "{ Sym::Jump, \"nhảy lên\" }") && has(map, "{ Sym::Seek, \"đi tìm quái\" }") &&
              has(map, "\"Mũi tên: chỗ GoBot sẽ nhảy lên") && has(map, "\"Đường nét đứt: GoBot đang đi tới chỗ đứng đánh"),
          "and its legend says them");
    const std::string page = body(map, "std::shared_ptr<const GameMapBook::Page> GuiApp::map_page_now() const {");
    check(has(page, "return ui.cur.page;") && !has(page, "get(") && has(page, "u.file_doubtful"),
          "map_page_now: the page the tab's worker read, never a file read on the GUI thread, never one found off");

    // The floors the seek and the plates place the monsters on: set by the
    // tab's tick only while the hunter goes by this map's area with the seek
    // on, handed to PacketEyes as Inputs::floors -- without them every monster
    // is on no floor (SeekNo::NoFloor) and the seek would stand idle.
    const std::string tick = body(map, "void GuiApp::simple_map_tick() {");
    check(has(tick, "plate_floors_.reset();") &&
              in_order(tick, { "std::shared_ptr<const GameMapBook::Page> floors;",
                               "if (hunting_ && hunter_ && hunt_config_.spot && hunt_config_.seek_in_zone && "
                               "packets_wanted() && ui.cur.page) {",
                               "if (u.by != KnownMap::By::None && !u.file_doubtful && ui.cur.id.stem == u.stem) {",
                               "const Hunter::Status hs = hunter_->status();",
                               "if (hs.spot_from_map && !hs.spot_suspended && hs.spot_stem == u.stem) floors = "
                               "ui.cur.page;",
                               "std::lock_guard<std::mutex> lock(plate_floors_mx_);",
                               "plate_floors_ = std::move(floors);" }) &&
              in_order(body(gui, "void GuiApp::initialise() {"),
                       { "in.floors = [this] {", "std::lock_guard<std::mutex> lock(plate_floors_mx_);",
                         "return plate_floors_;", "packet_eyes_ = std::make_unique<PacketEyes>(" }),
          "the seek's floors: the tab's page while the hunter goes by this map's area with the seek on, none "
          "otherwise -- handed to PacketEyes as Inputs::floors");

    // The hunt stopped by the drop and the jump up: a map change with a drop
    // and a jump both under watch unticks both, before the save; a high
    // jump's ↑ owed says ↑, not ↓ -- in drop_tick and in tick()'s own halt.
    check(in_order(dtick, { "} else if (stop == Hunter::DropStop::BothMapChanged) {", "hunt_config_.drop_down = false;",
                            "hunt_config_.jump_up = false;", "hunter_->set_config(hunt_config_);",
                            "hunt_config_.save(exe_relative(\"configs/hunt.ini\"))",
                            "drop_stopped_vi_ = std::string(\"Lần nhảy lúc \")",
                            "bỏ dấu Nhảy xuống và Nhảy lên.\";" }) &&
              in_order(dtick, { "} else if (stop == Hunter::DropStop::JumpKeysStuck) {",
                                "\"GoBot đã dừng đánh quái: phím ↑ hoặc Ctrl có thể vẫn đang bị giữ — nhấn rồi nhả ↑ và \"",
                                "} else {", "phím ↓ hoặc Ctrl có thể vẫn đang bị giữ" }),
          "drop_tick: a map change with both under watch unticks Nhảy xuống and Nhảy lên and saves; a high jump's "
          "keys owed name ↑ and Ctrl, a drop's ↓ and Ctrl");
    const std::string gtick = body(gui, "void GuiApp::tick() {");
    check(in_order(gtick, { "input_.owed_tries() >= InputManager::kOwedTries && hunting_", "stop_hunt();",
                            "const bool up = input_.up_owed();", "drop_stopped_vi_ = up ?", "nhấn rồi nhả ↑ \"",
                            "nhấn rồi nhả ↓ \"", "simple_note(drop_stopped_vi_, 15000);",
                            "\"Hunter: keys owed after a jump up (Up or Ctrl up not sent, %d tries)" }),
          "tick()'s own halt on owed ups: ↑ named for a high jump's, ↓ for a drop's, in the red line and the log");

    // The spot's lines: the way back that jumps, and none of three ways.
    check(has(spot, "\" — bot ngưng đánh, về vùng trước: về \" + way + jump + \", vào tới trong vùng rồi mới đánh "
                    "tiếp.\"") &&
              has(spot, "\" không có đường đi bộ, rơi xuống hay nhảy lên nào về vùng: bot đánh ở đây. Tự đưa \"") &&
              has(spot, "\"Tự tìm quái: bật — bot đi tới quái ở bất kỳ chỗ nào trong vùng mà nó tới được, \"") &&
              has(spot, "out << \", the way back jumps up \" << hs.spot_way_jumps"),
          "the spot's lines: a way back by a jump up, no way back by walking, falling or jumping up, the seek on, "
          "and the Positions line");
#else
    skip("no source folder\n");
#endif
}

void test_shipped() {
    section("Hunt card: the shipped hunt.ini -- jump_up=true and seek_in_zone=true, with their notes");
#ifdef GOBOT_SOURCE_DIR
    const std::string path = std::string(GOBOT_SOURCE_DIR) + "/configs/hunt.ini";
    const std::string text = source("/configs/hunt.ini");
    HunterConfig c;
    c.jump_up = false;
    c.seek_in_zone = false;
    IniFile ini;
    if (text.empty() || !ini.load(path)) {
        skip("the source tree's configs are not here\n");
        return;
    }
    check(c.load(path) && c.jump_up && c.seek_in_zone && ini.has_key("Hunt", "jump_up") &&
              ini.has_key("Hunt", "seek_in_zone"),
          "hunt.ini: jump_up=true and seek_in_zone=true, there and read as on");
    check(has(text, "# \"Tự tìm quái trong vùng\" (since 2026.11.05b)") && has(text, "# Đi tới quái ở bất kỳ chỗ nào") &&
              has(text, "# safe on a gate, and so is Ctrl + Up"),
          "with its notes in English and Vietnamese, and the gate's note saying Ctrl + Up too");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_hunt_card(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_ticks();
    test_card();
    test_arrow_and_marks();
    test_solid_marks();
    test_gui_source();
    test_shipped();
}
