// gobot_test -- "Tự tìm quái trong vùng" (2026.11.05b): the seek's pure parts
// on the game's own t52_s2 -- where a monster is placed on its floor
// (mob_place, a walk run out followed on), the box's floors the character
// gets to from where it stands (seek_map), each monster's attack point
// (seek_attack), the cases of the hunt of 2026-11-05 23:02-23:08, and the
// way back (return_point) unchanged by the legs it now shares with the seek.
//
// Over copies of the game's map files in build\handoff\drop\testdata --
// never the game's install -- and skipped when they are not there.

#include "gobot/common.hpp"
#include "gobot/hunt_seek.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/map_book.hpp"
#include "gobot/map_view.hpp"
#include "gobot/mob_place.hpp"
#include "gobot/packet_world.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

template <typename... Args>
std::string fmt(const char* f, Args... args) {
    char buf[900];
    std::snprintf(buf, sizeof buf, f, args...);
    return buf;
}

/// The game's files copied for the tests, or "" -- never the game's install.
std::string game_data() {
#ifdef GOBOT_SOURCE_DIR
    const std::string dir = std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data";
    if (path_exists(dir + "\\Map\\t52_s2.map") && path_exists(dir + "\\Project\\t52_s2.prj")) return dir;
#endif
    return {};
}

/// The standing point on the floor of column x nearest y: its top - 1.
int stand_y(const GameMapBook::Page& p, int x, int y) {
    int best = y, off = INT_MAX;
    for (const MapFloor& f : p.floors_at(x))
        if (std::abs(f.y - 1 - y) < off) {
            off = std::abs(f.y - 1 - y);
            best = f.y - 1;
        }
    return best;
}

HuntZone owner_box() {
    HuntZone z;
    z.home_x = 880;
    z.home_y = 274;
    z.left = 265;
    z.top = 174;
    z.right = 1399;
    z.bottom = 694;
    return z;
}

/// The owner's hunt.ini: reach 450, the band 60 both ways, one to one.
SeekRules owner_rules(const JumpRules* jumps = nullptr, bool go = true) {
    SeekRules r;
    r.reach = 450;
    r.up = 60;
    r.down = 60;
    r.jumps = jumps;
    r.jumps_go = go;
    return r;
}

/// A monster placed at x, y on floor `top`.
SeekMob mob(int x, int y, int top = -2) {
    SeekMob m;
    m.slot = 1;
    m.gen = 1;
    m.at = { x, y };
    m.floor_y = top == -2 ? y + 1 : top;
    return m;
}

std::string plan_text(const std::optional<SeekPlan>& p, SeekNo why) {
    if (!p) return std::string("none: ") + seek_no_name(why);
    std::string legs;
    for (const SeekLeg& l : p->legs) legs += fmt(" %s@%d", seek_leg_letter(l.kind), l.walk_x);
    return fmt("%s, cost %d, from %d,%d%s", seek_tier_name(p->tier), p->cost, p->attack.x, p->attack.y, legs.c_str());
}

// ------------------------------------------------------------- the oracle

/// return_point as 2026.11.05b's Part 1 had it, before zone_legs_from: what
/// the refactor is held to, point by point.
std::optional<ZoneReturn> old_return_point(const GameMapBook::Page& p, const HuntZone& z, MapPoint from,
                                           const JumpRules* up) {
    std::optional<MapFloor> home = p.floor_near(z.home_x, z.home_y);
    if (home && home->wall()) home.reset();
    struct Leg {
        MapPoint at;
        int      falls = 0;
        int      jumps = 0;
        int      walked = 0;
        int      first_x = 0;
        std::optional<JumpPlan> first_jump;
    };
    using Rank = std::array<int, 6>;
    std::optional<ZoneReturn> best;
    Rank best_rank{};
    std::vector<Leg> todo{ Leg{ from, 0, 0, 0, 0, std::nullopt } };
    std::vector<Leg> jumped;
    std::vector<std::pair<MapPoint, MapPoint>> seen;
    const int rounds = up != nullptr ? kZoneReturnJumpsMax + 1 : 1;
    for (int round = 0; round < rounds && !todo.empty(); ++round) {
        if (best && best->home) break;
        for (std::size_t i = 0; i < todo.size(); ++i) {
            const Leg leg = todo[i];
            const MapTrail left = p.follow(leg.at.x, leg.at.y, -1);
            const MapTrail right = p.follow(leg.at.x, leg.at.y, 1);
            if (left.pts.empty() || right.pts.empty()) continue;
            const MapPoint l_end = left.pts.back(), r_end = right.pts.back();
            if (std::any_of(seen.begin(), seen.end(), [&](const std::pair<MapPoint, MapPoint>& s) {
                    return s.first.x == l_end.x && s.first.y == l_end.y && s.second.x == r_end.x &&
                           s.second.y == r_end.y;
                }))
                continue;
            seen.emplace_back(l_end, r_end);
            const bool first = leg.falls == 0 && leg.jumps == 0;
            for (const MapTrail* t : { &left, &right }) {
                for (const MapPoint& q : t->pts) {
                    const MapPoint s{ q.x, q.y - 1 };
                    if (!z.contains(s.x, s.y)) continue;
                    const bool at_home = home && q.x == z.home_x && q.y == home->y;
                    const int walked = leg.walked + std::abs(q.x - leg.at.x);
                    const Rank rank{ at_home ? 0 : 1, leg.jumps, leg.falls, std::abs(s.x - z.home_x),
                                     std::abs(s.y - z.home_y), walked };
                    if (best && !(rank < best_rank)) continue;
                    best_rank = rank;
                    ZoneReturn r;
                    r.at = at_home ? MapPoint{ z.home_x, z.home_y } : s;
                    r.floor_y = q.y;
                    r.home = at_home;
                    r.falls = leg.falls;
                    r.jumps = leg.jumps;
                    r.walk_x = first ? r.at.x : leg.first_x;
                    r.jump = leg.first_jump;
                    best = r;
                }
            }
            if (leg.falls < kZoneReturnFallsMax) {
                for (const MapTrail* t : { &left, &right }) {
                    if (!t->ledge) continue;
                    const int dir = t == &left ? -1 : 1;
                    const MapPoint end = t->pts.back();
                    std::optional<MapPoint> land;
                    bool in_box = true;
                    for (const double share : kZoneWalkOffShares) {
                        land = walk_off_landing(p, end, dir, share * kZoneWalkPxPerS);
                        in_box = in_box && land && z.contains(land->x, land->y);
                    }
                    if (!in_box) continue;
                    Leg next = leg;
                    next.at = *land;
                    next.falls = leg.falls + 1;
                    next.walked = leg.walked + std::abs(end.x - leg.at.x) + std::abs(land->x - end.x);
                    if (first) next.first_x = end.x + dir * kZoneWalkOffPastPx;
                    todo.push_back(next);
                }
            }
            if (up != nullptr && leg.jumps < kZoneReturnJumpsMax && !(best && best->home)) {
                for (const JumpPlan& j : jump_legs_from(p, z, leg.at, *up)) {
                    Leg next = leg;
                    next.at = j.land();
                    next.jumps = leg.jumps + 1;
                    next.walked = leg.walked + j.walk_px;
                    if (first) {
                        next.first_x = j.x;
                        next.first_jump = j;
                    }
                    jumped.push_back(next);
                }
            }
        }
        todo = std::move(jumped);
        jumped.clear();
    }
    if (best) return best;
    if (up != nullptr) return std::nullopt;
    if (from.y - z.home_y > Hunter::kOneJumpPx) return std::nullopt;
    ZoneReturn r;
    r.at = { z.home_x, z.home_y };
    r.floor_y = home ? home->y : z.home_y + 1;
    r.home = true;
    r.walk_x = z.home_x;
    r.as_before = true;
    return r;
}

bool same_jump(const std::optional<JumpPlan>& a, const std::optional<JumpPlan>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->kind == b->kind && a->x == b->x && a->feet_y == b->feet_y && a->land_x == b->land_x &&
           a->land_top == b->land_top && a->rise == b->rise && a->headroom == b->headroom && a->run_l == b->run_l &&
           a->run_r == b->run_r && a->walk_px == b->walk_px && a->score == b->score;
}

bool same_way(const std::optional<ZoneReturn>& a, const std::optional<ZoneReturn>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->at.x == b->at.x && a->at.y == b->at.y && a->floor_y == b->floor_y && a->home == b->home &&
           a->falls == b->falls && a->jumps == b->jumps && a->walk_x == b->walk_x && a->as_before == b->as_before &&
           same_jump(a->jump, b->jump);
}

// ------------------------------------------------------------- the tests

void test_place() {
    section("Seek: where a monster is placed (mob_place) -- a ramp walker on its ramp, a walk run out followed on to "
            "the ramp's foot and held there, never by a blow's speed (t52_s2)");
    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): the seek's placing\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    check(page != nullptr, "t52_s2 read");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    const std::uint64_t now = 1'000'000;

    // A ramp walker of 23:04:42: its frame on the ramp at 1046, its walk to
    // 1007 -- on the ramp there, at about 578, not 551 nor the ground's 687.
    PacketWorld::Mob m;
    m.slot = 18;
    m.gen = 3;
    m.frame_x = 1046.0f;
    m.frame_y = static_cast<float>(stand_y(p, 1046, 551));
    m.x = 1007.0f;
    m.y = m.frame_y;
    m.facing = -1;
    m.speed = 2.6f;
    m.moving = true;
    m.event_ms = now - 400;
    MapDot d = mob_place(&p, m, now, true);
    check(d.why == DotWhy::Followed && std::abs(d.x - 1007.0) < 0.01 && std::abs(d.y - 578.0) <= 3.0 &&
              d.top == static_cast<int>(std::lround(d.y)) + 1,
          fmt("a ramp walker framed at 1046,%.0f, walked to 1007: on the ramp at %.0f,%.0f (floor %d)",
              static_cast<double>(m.frame_y), d.x, d.y, d.top));

    // A walk of 23:03 from the ramp's far end, 22 s old, the world holding it
    // at 2594 since its 5 s: followed on, held at the ramp's foot (992).
    PacketWorld::Mob far;
    far.slot = 32;
    far.gen = 7;
    far.frame_x = 3114.0f;
    far.frame_y = static_cast<float>(stand_y(p, 3114, 511));
    far.x = 3114.0f - 520.0f;
    far.y = far.frame_y;
    far.facing = -1;
    far.speed = 2.6f;
    far.moving = false;
    far.event_ms = now - 22'000;
    d = mob_place(&p, far, now, true);
    const int foot = stand_y(p, 992, 588);
    check(d.why == DotWhy::Projected && std::abs(d.x - 992.0) < 0.01 && std::abs(d.y - foot) <= 2.0,
          fmt("a walk run out 22 s ago from 3114: projected, held at the ramp's foot -- %.0f,%.0f (foot %d)", d.x, d.y,
              foot));
    far.event_ms = now - 20'000;
    d = mob_place(&p, far, now, true);
    check(d.why == DotWhy::Projected && std::abs(d.x - 1034.0) < 0.01 && d.top > 0 &&
              std::abs(d.y - (d.top - 1)) <= 2.0,
          fmt("20 s old: 2,080 px on, at 1034 on the ramp -- %.0f,%.0f", d.x, d.y));
    far.event_ms = now - 60'000;
    const MapDot capped = mob_place(&p, far, now, true);
    far.event_ms = now - kPlaceWalkOnMaxMs;
    const MapDot at_cap = mob_place(&p, far, now, true);
    check(std::abs(capped.x - at_cap.x) < 0.01 && capped.why == DotWhy::Projected,
          "a minute old: no further than kPlaceWalkOnMaxMs carries it");
    d = mob_place(&p, far, now, false);
    check(d.why == DotWhy::Followed && std::abs(d.x - 2594.0) < 0.01,
          fmt("not projected (the map tab's rule): where the world holds it, 2594 -- %.0f", d.x));
    // The moment the world holds it: the same x, the place never jumps.
    far.event_ms = now - 5000;
    d = mob_place(&p, far, now, true);
    check(d.why == DotWhy::Projected && std::abs(d.x - 2594.0) < 0.01,
          fmt("run out just now: projected to where the world holds it -- %.0f", d.x));
    // A blow's 7 sent as a walk, a walk under way, one standing: never projected.
    PacketWorld::Mob blow = far;
    blow.speed = 7.0f;
    PacketWorld::Mob under_way = far;
    under_way.moving = true;
    PacketWorld::Mob standing = far;
    standing.x = standing.frame_x;
    check(mob_place(&p, blow, now, true).why != DotWhy::Projected &&
              mob_place(&p, under_way, now, true).why != DotWhy::Projected &&
              mob_place(&p, standing, now, true).why != DotWhy::Projected && kPlaceProjectWalks &&
              kPlaceWalkOnMaxMs == 25000 && kPlatesOnFloors,
          "a blow's speed, a walk under way, a monster standing: never projected");
    // No page: where the packets put it.
    d = mob_place(nullptr, far, now, true);
    check(d.why == DotWhy::AsIs && std::abs(d.x - 2594.0) < 0.01 && d.top == -1, "no map file: as it is");
    // The map tab's dot: mob_place's rule, never projected (2026.11.05's).
    far.event_ms = now - 22'000;
    bool same = true;
    for (const PacketWorld::Mob& w : { m, far, blow, under_way, standing }) {
        const MapDot a = mob_dot(&p, w, now), b = mob_place(&p, w, now, false);
        same = same && std::abs(a.x - b.x) < 0.01 && std::abs(a.y - b.y) < 0.01 && a.why == b.why;
    }
    check(same && mob_dot(&p, far, now).why == DotWhy::Followed,
          "mob_dot forwards to mob_place with no walk projected: the tab's dots as 2026.11.05 drew them");
}

void test_pieces() {
    section("Seek: the box's floors got to (seek_map) on t52_s2 -- the ground alone without jumps, the ledge by "
            "Ctrl + Up and the ramp off its end with them, the ledge's own floor from the home");
    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here: the seek's floors\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    const HuntZone z = owner_box();
    const MapPoint ground{ 880, stand_y(p, 880, 634) };

    SeekMap m = seek_map(p, z, ground, owner_rules());
    const auto describe = [](const SeekMap& sm) {
        std::string s;
        for (const SeekPiece& k : sm.pieces)
            s += fmt(" [%d..%d use %d..%d cost %d falls %d jumps %d]", k.floor_l, k.floor_r, k.use_l, k.use_r,
                     k.entry_cost, k.falls, k.jumps);
        return s;
    };
    check(m.pieces.size() == 1 && m.pieces[0].floor_l == 265 && m.pieces[0].floor_r == 1034 &&
              m.pieces[0].use_l == 313 && m.pieces[0].use_r == 986,
          "from 880,634, no jumps: the ground alone, x 265..1034, usable 313..986 --" + describe(m));
    // No usable column within the inset of where the ground leaves the box,
    // and every usable one keeps a walk of kSeekEdgeInsetPx inside it.
    bool inset = true;
    for (const SeekPiece& k : m.pieces)
        for (std::size_t i = 0; i < k.pts.size(); ++i) {
            if (!k.usable[i]) continue;
            const MapPoint s{ k.pts[i].x, k.pts[i].y - 1 };
            if ((s.x >= 987 && s.x <= 1034) || !walk_stays_in(p, z, s, -1, Hunter::kZoneWalkSlackPx).ok ||
                !walk_stays_in(p, z, s, 1, Hunter::kZoneWalkSlackPx).ok)
                inset = false;
        }
    check(inset && kSeekEdgeInsetPx == Hunter::kZoneWalkSlackPx,
          "no usable column at 987..1034; from each, a walk of 48 px either way stays in the box");

    JumpRules up;
    m = seek_map(p, z, ground, owner_rules(&up));
    const SeekPiece* ledge = nullptr;
    const SeekPiece* ramp = nullptr;
    for (const SeekPiece& k : m.pieces) {
        if (k.floor_l == 472 && k.floor_r == 1104 && ledge == nullptr) ledge = &k;
        if (k.floor_l == 992 && ramp == nullptr) ramp = &k;
    }
    const bool jump_ok = ledge != nullptr && ledge->jumps == 1 && ledge->legs.size() == 1 &&
                         ledge->legs[0].kind == SeekLeg::Kind::Jump && ledge->legs[0].jump &&
                         ledge->legs[0].jump->kind == JumpKind::CtrlUp && std::abs(ledge->legs[0].walk_x - 810) <= 16;
    check(m.pieces.size() == 3 && jump_ok && ledge->use_l == 472 && ledge->use_r == 1104,
          fmt("with jump rules: 3 floors -- the ledge 472..1104, all usable, by one Ctrl+Up at %d (about 810) --",
              ledge && !ledge->legs.empty() ? ledge->legs[0].walk_x : 0) +
              describe(m));
    check(ramp != nullptr && ramp->use_l == 992 && ramp->use_r == 1351 && ramp->falls == 1 && ramp->jumps == 1 &&
              ramp->legs.size() == 2 && ramp->legs[1].kind == SeekLeg::Kind::Fall && ramp->legs[1].from.x == 1104,
          "the ramp, usable 992..1351, by the jump and then off the ledge's right end at 1104");

    // From the home on the ledge: the ledge its own floor, the ground and the
    // ramp below by the falls off its ends.
    m = seek_map(p, z, { 880, 274 - 0 }, owner_rules(&up));
    const SeekPiece* own = m.pieces.empty() ? nullptr : &m.pieces[0];
    bool left_fall = false, right_fall = false;
    for (const SeekPiece& k : m.pieces) {
        if (k.falls == 1 && k.jumps == 0 && !k.legs.empty() && k.legs[0].from.x == 472) left_fall = true;
        if (k.falls == 1 && k.jumps == 0 && !k.legs.empty() && k.legs[0].from.x == 1104) right_fall = true;
    }
    check(own != nullptr && own->floor_l == 472 && own->floor_r == 1104 && own->falls == 0 && own->jumps == 0 &&
              left_fall && right_fall,
          "from 880,274: the ledge is its own floor; the ground and the ramp below by the falls at 472 and 1104 --" +
              describe(m));
}

void test_attack_points() {
    section("Seek: attack points on t52_s2 with margins 24 and 8 -- the cases of the hunt of 2026-11-05 23:02-23:08");
    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here: the seek's attack points\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    const HuntZone z = owner_box();
    JumpRules up;
    const auto at = [&](int x, int y) { return MapPoint{ x, stand_y(p, x, y) }; };

    // 23:04:42: from 945,666 the target at 1007,578 on the ramp's foot -- hit
    // from up the slope, walking LEFT; 05 walked right into the box's bottom
    // edge at 1035. The prototype's 873,630 is the band's edge: the turn to
    // the monster after the walk left walks the character 21 px back down
    // the slope, 10 px of height, out of it -- so the point is kSeekTurnPx
    // further up, about 849, and the turn ends near 870.
    SeekNo why = SeekNo::None;
    SeekMap m = seek_map(p, z, at(945, 666), owner_rules());
    std::optional<SeekPlan> a = seek_attack(m, z, mob(1007, 578, 579), owner_rules(), &why);
    check(a && a->tier == SeekTier::OwnFloor && std::abs(a->attack.x - 849) <= 4 && a->legs.empty() &&
              a->first.kind == SeekLeg::Kind::Walk && a->attack.x < 945,
          "23:04:42, 945,666 -> 1007,578: " + plan_text(a, why) + " (about 849, walking left; the turn back to it "
                                                                  "ends near 870)");
    SeekRules no_turn = owner_rules();
    no_turn.turn_px = 0;
    a = seek_attack(m, z, mob(1007, 578, 579), no_turn, &why);
    check(a && std::abs(a->attack.x - 873) <= 2,
          "... with no room for the turn (turn_px 0): the prototype's " + plan_text(a, why));

    // 23:05:19: at 709,579 the left ground's monster is in reach already --
    // stood at, no walk back.
    m = seek_map(p, z, at(709, 579), owner_rules());
    a = seek_attack(m, z, mob(435, 546), owner_rules(), &why);
    check(a && a->tier == SeekTier::OwnFloor && a->attack.x == 709 && a->cost == 0,
          "23:05:19, 709,579 -> 435,546: " + plan_text(a, why) + " (709 itself)");

    // 23:05:25: 1132,486 on the ramp from 941,664 -- no place on the ground
    // without jumps; with them, above: Ctrl + Up onto the ledge, off its end
    // onto the ramp, hit from about 1205,434.
    m = seek_map(p, z, at(941, 664), owner_rules());
    a = seek_attack(m, z, mob(1132, 486), owner_rules(), &why);
    check(!a && why == SeekNo::NoAttackPoint, "23:05:25, 941,664 -> 1132,486, no jumps: " + plan_text(a, why));
    m = seek_map(p, z, at(941, 664), owner_rules(&up));
    a = seek_attack(m, z, mob(1132, 486), owner_rules(&up), &why);
    check(a && a->tier == SeekTier::Above && std::abs(a->attack.x - 1205) <= 24 && a->legs.size() == 2 &&
              a->first.kind == SeekLeg::Kind::Jump,
          "... with jumps: " + plan_text(a, why) + " (above, about 1205,434)");
    // The same with jumps worked out but off (jump_up unticked): NeedsJumpOff,
    // the plan kept for the log.
    std::optional<SeekPlan> would;
    a = seek_attack(m, z, mob(1132, 486), owner_rules(&up, false), &why, &would);
    check(!a && why == SeekNo::NeedsJumpOff && would && would->tier == SeekTier::Above,
          "... jumps worked out but off: " + plan_text(a, why) + "; would: " + plan_text(would, SeekNo::None));

    // Outside the box: never gone to, wherever the character is.
    m = seek_map(p, z, at(947, 667), owner_rules(&up));
    bool outside = true;
    for (const MapPoint o : { MapPoint{ 1237, 729 }, MapPoint{ 1429, 589 }, MapPoint{ 1122, 715 }, MapPoint{ 174, 548 } }) {
        a = seek_attack(m, z, mob(o.x, o.y), owner_rules(&up), &why);
        outside = outside && !a && why == SeekNo::Outside;
    }
    check(outside && !kSeekOutsideHit, "1237,729, 1429,589, 1122,715 and 174,548: outside the box, never gone to");

    // A ledge monster from under the home: above, by Ctrl + Up at about 810.
    m = seek_map(p, z, at(880, 634), owner_rules(&up));
    a = seek_attack(m, z, mob(769, 272), owner_rules(&up), &why);
    check(a && a->tier == SeekTier::Above && a->first.kind == SeekLeg::Kind::Jump && a->first.jump &&
              a->first.jump->kind == JumpKind::CtrlUp && std::abs(a->first.walk_x - 810) <= 16,
          "880,634 -> 769,272 on the ledge: " + plan_text(a, why) + " (above, Ctrl+Up about 810)");
    // Phantoms and another player's monster.
    a = seek_attack(m, z, mob(700, 400, -1), owner_rules(&up), &why);
    check(!a && why == SeekNo::NoFloor, "a monster on no floor: " + plan_text(a, why));
    SeekMob theirs = mob(600, 578);
    theirs.hurt_by_other = true;
    a = seek_attack(m, z, theirs, owner_rules(&up), &why);
    check(!a && why == SeekNo::Others && kSeekOthersHurtMs == 10000, "another player's: " + plan_text(a, why));
    // From the home, the ground below by a fall: below.
    m = seek_map(p, z, { 880, 274 }, owner_rules(&up));
    a = seek_attack(m, z, mob(580, 578), owner_rules(&up), &why);
    check(a && a->tier == SeekTier::Below && a->first.kind == SeekLeg::Kind::Fall,
          "880,274 -> 580,578 on the ground: " + plan_text(a, why) + " (below, a fall off the ledge)");
}

void test_way_back_same() {
    section("Seek: the way back (return_point) the same as before zone_legs_from, point by point over t52_s2 -- the "
            "owner's box and the box down to 810, with jump rules and without");
    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here: the way back against its old self\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    HuntZone low = owner_box();
    low.bottom = 810;
    JumpRules up;
    int points = 0, differ = 0, with_jumps = 0;
    std::string first_diff;
    for (const HuntZone& z : { owner_box(), low }) {
        for (int x = 0; x < p.floors.w_px(); x += 8) {
            for (const MapFloor& f : p.floors_at(x)) {
                if (f.wall()) continue;
                const MapPoint s{ x, f.y - 1 };
                ++points;
                if (!same_way(old_return_point(p, z, s, nullptr), return_point(p, z, s))) {
                    ++differ;
                    if (first_diff.empty()) first_diff = fmt(" (first at %d,%d, no jumps)", s.x, s.y);
                }
                // With jump rules: every 32nd column, the planner's work being more.
                if (x % 32 != 0) continue;
                ++with_jumps;
                if (!same_way(old_return_point(p, z, s, &up), return_point(p, z, s, &up))) {
                    ++differ;
                    if (first_diff.empty()) first_diff = fmt(" (first at %d,%d, with jumps)", s.x, s.y);
                }
            }
        }
    }
    check(points > 1000 && with_jumps > 200 && differ == 0,
          fmt("%d standing points, %d of them with jump rules too: %d differ%s", points, with_jumps, differ,
              first_diff.c_str()));
}

} // namespace

void test_hunt_seek(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_place();
    test_pieces();
    test_attack_points();
    test_way_back_same();
}
