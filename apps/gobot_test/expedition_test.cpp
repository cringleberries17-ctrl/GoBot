// gobot_test -- Đào khoáng and phó bản Cổ Thụ (2026.11.19): the frames read
// for the bag's other tabs and for digging, the settings, GoC's route table
// and dig spot, the two planners step by step, and the .prj's map name.
//
// No window, no key, no game: the planners are fed snapshots made here.

#include "gobot/auto_chat.hpp"
#include "gobot/bag.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/expedition.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/map_book.hpp"
#include "gobot/menu_text.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/template_matcher.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

using namespace gobot;
using namespace gobot::expedition;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;
void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4);
    std::memcpy(&b[at], &v, 4);
}
void put16(Bytes& b, std::size_t at, std::uint16_t v) {
    if (b.size() < at + 2) b.resize(at + 2);
    std::memcpy(&b[at], &v, 2);
}

/// A tab frame as 0x0068 has it, 1528 bytes: slot j's id at 12 + 4j and its
/// count at 300 + 2j, from the frame's first byte.
GamePacket tab_frame(std::uint16_t opcode, const std::vector<std::pair<int, std::pair<std::uint32_t, std::uint16_t>>>& slots) {
    GamePacket p;
    p.opcode = opcode;
    p.from_server = true;
    p.depth = 1;
    p.payload.assign(1528 - 8, 0);
    for (const auto& s : slots) {
        put32(p.payload, 12 - 8 + 4 * static_cast<std::size_t>(s.first), s.second.first);
        put16(p.payload, 300 - 8 + 2 * static_cast<std::size_t>(s.first), s.second.second);
    }
    return p;
}

GamePacket worn_frame(std::uint32_t item_in_slot3) {
    GamePacket p;
    p.opcode = 0x0065;
    p.from_server = true;
    p.depth = 1;
    p.payload.assign(400, 0);
    put32(p.payload, 12 - 8 + 4 * 3, item_in_slot3);
    return p;
}

GamePacket dig_frame(std::uint32_t id, std::int32_t value) {
    GamePacket p;
    p.opcode = 0x0132;
    p.from_server = true;
    p.depth = 1;
    p.payload.assign(12, 0);
    put32(p.payload, 4, id);
    put32(p.payload, 8, static_cast<std::uint32_t>(value));
    return p;
}

void test_packets() {
    section("Expedition: the bag's other tabs and digging, from the packets");
    TripPackets tp;
    tp.on_packets({ tab_frame(0x0066, { { 5, { 8090111, 1 } }, { 6, { 8090121, 2 } } }) });
    const TripPackets::Tab e = tp.equip1();
    check(e.known && e.slots.size() == 72 && e.slots[5].item == 8090111 && e.slots[6].count == 2,
          "0x0066 (Trang bị 1): ids at 12 + 4j, counts at 300 + 2j, 72 slots");
    check(!tp.worn().known && !tp.other().known, "the other tabs not known before their frames");

    tp.on_packets({ worn_frame(8090131) });
    const TripPackets::Tab w = tp.worn();
    check(w.known && w.slots.size() == 17 && w.slots[3].item == 8090131 && is_pickaxe(w.slots[3].item),
          "0x0065 (worn): 17 ids at 12 + 4j, a pickaxe among them");

    GamePacket client = tab_frame(0x0069, {});
    client.from_server = false;
    tp.on_packets({ client });
    check(!tp.other().known, "a frame from the client is not the bag");
    GamePacket cut = tab_frame(0x0069, {});
    cut.payload.resize(200);
    tp.on_packets({ cut });
    check(!tp.other().known, "a frame cut short of the counts is not read");

    tp.on_packets({ dig_frame(777, 1) });
    check(tp.dig(777u).state == 2, "0x0132 value 1: digging");
    tp.on_packets({ dig_frame(777, 8920111) });
    check(tp.dig(777u).state == 2 && tp.dig(777u).last_item == 8920111, "an ore's id: dug, still digging");
    tp.on_packets({ dig_frame(888, 1) });
    tp.on_packets({ dig_frame(777, -1) });
    check(tp.dig(777u).state == 1 && tp.dig(888u).state == 2, "-1: over, for that character alone");
    check(tp.dig(std::nullopt).state == 1, "with no id known, the newest digger's");
    const std::uint64_t seq = tp.dig(777u).seq;
    tp.on_packets({ dig_frame(777, 1) });
    check(tp.dig(777u).seq > seq, "each event moves the sequence on");
    GamePacket entry;
    entry.opcode = 0x0086;
    tp.on_packets({ entry });
    check(tp.dig(777u).state == 0, "a map entry: no digging known");
    GamePacket odd = dig_frame(1, 1);
    odd.payload.push_back(0);
    check(!TripPackets::read_dig(odd), "0x0132 of another size is not read");
}

std::string temp_path(const char* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return wide_to_utf8(dir) + name;
}

void test_settings() {
    section("Expedition: configs\\expedition.ini");
    ExpeditionSettings s;
    s.mining.enabled = true;
    s.mining.map_key = "t20_s72";
    s.mining.auto_buy = true;
    s.mining.buy_pickaxe = 8090141;
    s.mining.buy_when = 2;
    s.cothu.enabled = true;
    s.cothu.role = 1;
    s.cothu.minimum_members = 4;
    s.cothu.boss_position = 1;
    s.cothu.talk_key = 0x5A;
    s.colong.enabled = true;
    s.colong.wait_seconds = 33;
    const std::string path = temp_path("gobot_expedition_test.ini");
    check(s.save(path), "saved");
    ExpeditionSettings r;
    check(r.load(path), "read back");
    check(r.mining.enabled && r.mining.map_key == "t20_s72" && r.mining.auto_buy && r.mining.buy_pickaxe == 8090141 &&
              r.mining.buy_when == 2,
          "[Mining] as written");
    check(r.cothu.enabled && r.cothu.role == 1 && r.cothu.minimum_members == 4 && r.cothu.boss_position == 1 &&
              r.cothu.talk_key == 0x5A,
          "[CoThu] as written, the key in hex");
    check(r.colong.enabled && r.colong.wait_seconds == 33 && r.colong.role == 0,
          "[CoLong] as written, apart from [CoThu]");
    DeleteFileW(utf8_to_wide(path).c_str());

    IniFile bad;
    bad.set("Mining", "map_key", "t99_s1");
    bad.set("Mining", "buy_pickaxe_id", "123");
    bad.set("CoThu", "role", "7");
    MiningConfig m;
    m.load(bad);
    CoThuConfig c;
    c.load(bad);
    check(m.map_key == "t21_s71" && m.buy_pickaxe == 8090111 && c.role == 1,
          "a mine or pickaxe GoBot does not know is not taken; a role is held to 0..1");
    check(!MiningConfig{}.enabled && !CoThuConfig{}.enabled, "both off until ticked");
}

TripSnap at(const std::string& stem, std::uint32_t epoch, float x, float y) {
    TripSnap s;
    s.world = true;
    s.stem = stem;
    s.epoch = epoch;
    s.x = x;
    s.y = y;
    s.settled = true;
    s.hp_known = true;
    s.hp = 1000;
    return s;
}

void wear(TripSnap& s, bool pickaxe, int in_bag) {
    s.worn.known = true;
    s.worn.slots.assign(17, TripSlot{});
    if (pickaxe) s.worn.slots[3].item = 8090111;
    s.equip1.known = true;
    s.equip1.slots.assign(72, TripSlot{});
    for (int i = 0; i < in_bag; ++i) s.equip1.slots[static_cast<std::size_t>(10 + i)] = TripSlot{ 8090121, 1 };
}

void test_route_and_spot() {
    section("Expedition: GoC's route table and dig spot");
    const auto r1 = MiningPlanner::route(kTown, "t21_s71");
    check(r1 && r1->teleporter && r1->x == kTownTeleX && std::string(r1->row) == kToHubRow,
          "Thanh Âm Quan to a mine: the Teleporter at 975,1525, its row to Mỏ Quặng Thủy Quật");
    const auto r2 = MiningPlanner::route(kHub, "t20_s71");
    check(r2 && !r2->teleporter && r2->x == 600.0f && r2->y == 655.0f, "the hub to a mine: that mine's gate");
    const auto r3 = MiningPlanner::route("t21_s72", "t21_s71");
    check(r3 && !r3->teleporter && r3->x == 1730.0f, "a mine to anywhere: its own gate out");
    const auto r4 = MiningPlanner::route(kTown4, kHub);
    check(r4 && r4->x == kTown4GateX, "t1_s4: its gate to Thanh Âm Quan");
    check(!MiningPlanner::route(kHub, kHub), "already there: no route");

    const Mine& m = *mine("t21_s71");     // exit gate at x 1665
    TripSnap s = at("t21_s71", 1, 1000.0f, 700.0f);
    const auto here = MiningPlanner::dig_spot(s, m, 1, 0);
    check(here && *here == 1000.0f, "clear of the gate: where you stand");
    s.x = 1600.0f;
    const auto away = MiningPlanner::dig_spot(s, m, 1, 0);
    check(away && std::abs(*away - 1665.0f) >= 192.0f, "near the gate: a step that keeps 192 px across from it");
    s.x = 1000.0f;
    s.players.push_back({ 1010.0f, 690.0f });
    const auto other = MiningPlanner::dig_spot(s, m, 1, 0);
    check(other && std::abs(*other - 1010.0f) >= 64.0f, "another player on the spot: a step away");
    s.players.clear();
    s.same_floor = [](int, int, int x2) { return x2 <= 1000; };
    const auto floor = MiningPlanner::dig_spot(s, m, 1, 1);
    check(floor && *floor < 1000.0f, "the map's floor: only steps along your own floor");
    const auto skipped = MiningPlanner::dig_spot(s, m, 1, 2);
    check(skipped && floor && *skipped != *floor, "a spot given up on: the next");
}

void test_mining_planner() {
    section("Expedition: the mining planner, GoC's order");
    MiningConfig c;
    c.enabled = true;
    c.map_key = "t21_s71";
    MiningPlanner p;
    std::uint64_t t = 1'000'000;

    TripSnap s = at("t99_s9", 1, 0, 0);
    TripJob j = p.next(s, c, t);
    check(j.kind == TripJobKind::None && p.status().find("khu đào khoáng") != std::string::npos,
          "outside the zone: waits, saying where to stand");

    s = at(kTown, 1, 300.0f, 1500.0f);
    wear(s, true, 0);
    j = p.next(s, c, t);
    check(j.kind == TripJobKind::Walk && j.x == kTownTeleX && j.y == kTownTeleY, "in town: to the Teleporter");
    check(p.next(s, c, t).kind == TripJobKind::None, "never a second job before the first is done");
    p.done(j, TripResult::Ok, s, t);
    j = p.next(s, c, t);
    check(j.kind == TripJobKind::Talk && j.item == 0x20, "there: the normal attack, Space, talks to it");
    p.done(j, TripResult::Ok, s, t);
    j = p.next(s, c, t);
    check(j.kind == TripJobKind::PickRow && j.text == kToHubRow && j.confirm, "its row, then its box");
    p.done(j, TripResult::Ok, s, t);
    j = p.next(s, c, t + 1000);
    check(j.kind == TripJobKind::None && p.status() == "Chờ chuyển map", "then the map change is waited for");

    s = at(kHub, 2, 100.0f, 655.0f);
    wear(s, true, 0);
    j = p.next(s, c, t + 2000);
    check(j.kind == TripJobKind::Walk && j.x == 225.0f && j.y == 650.0f, "on the hub: to the mine's gate");
    p.done(j, TripResult::Ok, s, t + 2000);
    j = p.next(s, c, t + 2000);
    check(j.kind == TripJobKind::Gate && j.count > 0, "at the gate: ↓");
    p.done(j, TripResult::Ok, s, t + 2000);

    s = at("t21_s71", 3, 900.0f, 700.0f);
    wear(s, true, 0);
    j = p.next(s, c, t + 3000);
    check(j.kind == TripJobKind::Walk && j.tol_x == kSpotTolX && j.tol_y == kSpotTolY, "in the mine: to a dig spot");
    p.done(j, TripResult::Ok, s, t + 3000);
    j = p.next(s, c, t + 3000);
    check(j.kind == TripJobKind::StartDig, "on it: the normal attack to dig");
    p.done(j, TripResult::Ok, s, t + 3000);
    s.dig.state = 2;
    j = p.next(s, c, t + 4000);
    check(j.kind == TripJobKind::None && p.status() == "Đang đào khoáng", "digging: idle");

    // The pickaxe breaks: no longer worn.
    s.dig.state = 1;
    wear(s, false, 1);
    j = p.next(s, c, t + 5000);
    check(j.kind == TripJobKind::CloseNotice, "worn and then not: the broken pickaxe's notice closed");
    p.done(j, TripResult::Ok, s, t + 5000);
    j = p.next(s, c, t + 5000);
    check(j.kind == TripJobKind::Equip && j.slot == 10 && j.item == 8090121,
          "then another from Trang bị 1, by its slot");

    // None left, no buying: a wait that says so -- no key -- and on by itself
    // once a pickaxe shows (2026.11.24: it stopped Đào khoáng until it was
    // unticked).
    MiningPlanner q;
    TripSnap e = at("t21_s71", 1, 900.0f, 700.0f);
    wear(e, false, 0);
    j = q.next(e, c, t);
    check(j.kind == TripJobKind::None && q.status().find("hết cuốc") != std::string::npos &&
              q.status().find("GoBot đào tiếp khi thấy cuốc") != std::string::npos && !q.resting(t),
          "no pickaxe anywhere and Tự mua cuốc off: waits, saying so -- never a stop: " + q.status());
    j = q.next(e, c, t + 600'000);
    check(j.kind == TripJobKind::None && q.status().find("hết cuốc") != std::string::npos,
          "ten minutes on: still waiting, nothing pressed");
    wear(e, false, 1);
    j = q.next(e, c, t + 600'200);
    check(j.kind == TripJobKind::Equip && j.slot == 10 && j.item == 8090121,
          "a pickaxe put in Trang bị 1: equipped at the next turn -- no BẮT ĐẦU, no unticking");
    q.done(j, TripResult::Ok, e, t + 600'200);
    wear(e, true, 0);
    j = q.next(e, c, t + 601'000);
    check(j.kind == TripJobKind::Walk && j.tol_x == kSpotTolX, "worn: on to a dig spot");

    // Trang bị 1 not read yet: waited for -- never taken for none left (it was
    // "hết cuốc", a stop), Tự mua cuốc on or off.
    {
        TripSnap unread = at("t21_s71", 1, 900.0f, 700.0f);
        unread.worn.known = true;
        unread.worn.slots.assign(17, TripSlot{});
        MiningConfig buying = c;
        buying.auto_buy = true;
        bool waits = true;
        for (const MiningConfig* cfg : { &c, &buying }) {
            MiningPlanner n;
            const TripJob w = n.next(unread, *cfg, t);
            waits = waits && w.kind == TripJobKind::None &&
                    n.status().find("chờ gói tin túi Trang bị 1") != std::string::npos &&
                    n.status().find("hết cuốc") == std::string::npos && !n.resting(t);
        }
        check(waits, "Trang bị 1 not read: \"chờ gói tin túi Trang bị 1\", never \"hết cuốc\", nothing bought");
    }

    // Broken with Tự động thay thế cuốc off: waits for the player; the box
    // ticked, on by itself (the settings are re-read every second).
    {
        MiningConfig keep = c;
        keep.auto_replace = false;
        MiningPlanner k;
        TripSnap w = at("t21_s71", 1, 900.0f, 700.0f);
        wear(w, true, 1);
        j = k.next(w, keep, t);
        k.done(j, TripResult::Ok, w, t);          // on the spot
        j = k.next(w, keep, t);
        k.done(j, TripResult::Ok, w, t);          // digging
        wear(w, false, 1);                         // it breaks
        j = k.next(w, keep, t + 1000);
        check(j.kind == TripJobKind::CloseNotice, "broken: its notice closed");
        k.done(j, TripResult::Ok, w, t + 1000);
        j = k.next(w, keep, t + 2000);
        check(j.kind == TripJobKind::None && k.status().find("Tự động thay thế cuốc đang tắt") != std::string::npos &&
                  k.status().find("chờ bạn") != std::string::npos,
              "Tự động thay thế cuốc off: waits for you, saying so -- never a stop: " + k.status());
        j = k.next(w, c, t + 3000);
        check(j.kind == TripJobKind::Equip && j.slot == 10, "the box ticked: the one in Trang bị 1 equipped");
    }

    // Buying: to Thợ Mỏ on the hub.
    MiningConfig b = c;
    b.auto_buy = true;
    MiningPlanner u;
    TripSnap h = at(kHub, 1, 300.0f, 655.0f);
    wear(h, false, 0);
    j = u.next(h, b, t);
    check(j.kind == TripJobKind::Walk && j.x == kMinerX, "no pickaxe and Tự mua cuốc: to Thợ Mỏ");
    u.done(j, TripResult::Ok, h, t);
    check(u.next(h, b, t).kind == TripJobKind::Talk, "talk to him");
    u.done(TripJob{ TripJobKind::Talk }, TripResult::Ok, h, t);
    j = u.next(h, b, t);
    check(j.kind == TripJobKind::PickRow && j.text == kShopRowText, "his Shop row");
    u.done(j, TripResult::Ok, h, t);
    j = u.next(h, b, t);
    check(j.kind == TripJobKind::Buy && j.item == 8090111 && j.count == 1, "and buy the pickaxe set");

    // The money guard (2026.11.24): bought, and Trang bị 1 not come again --
    // no second purchase while its frame is the same; a newer one with nothing
    // new in it says the purchase did not go: bought again.
    u.done(j, TripResult::Ok, h, t);
    check(u.pickaxe_unconfirmed(), "bought: the guard on until Trang bị 1 says what came of it");
    j = u.next(h, b, t + 1000);
    const bool on_to_mine = j.kind == TripJobKind::Walk && j.x == 225.0f;
    u.done(j, TripResult::Retry, h, t + 1000);
    j = u.next(h, b, t + 4000);
    check(on_to_mine && j.kind == TripJobKind::Walk && j.x == 225.0f && u.pickaxe_unconfirmed(),
          "Trang bị 1 the same frame: no second purchase -- on to the mine's gate, never back to Thợ Mỏ");
    u.done(j, TripResult::Retry, h, t + 4000);
    TripSnap newer = h;
    newer.equip1.seq = h.equip1.seq + 1;
    j = u.next(newer, b, t + 7000);
    check(j.kind == TripJobKind::Walk && j.x == kMinerX && !u.pickaxe_unconfirmed(),
          "a newer frame of Trang bị 1, still none and nothing new: the purchase did not go -- to Thợ Mỏ again");

    // To the Buy of a fresh planner on the hub.
    const auto to_buy = [](MiningPlanner& mp, const TripSnap& at_hub, const MiningConfig& cfg, std::uint64_t when) {
        for (int step = 0; step < 3; ++step) {
            const TripJob next_job = mp.next(at_hub, cfg, when);
            if (next_job.kind != (step == 0 ? TripJobKind::Walk : step == 1 ? TripJobKind::Talk : TripJobKind::PickRow))
                return TripJob{};
            mp.done(next_job, TripResult::Ok, at_hub, when);
        }
        return mp.next(at_hub, cfg, when);
    };
    {
        // Trang bị 1 holding something new after the Buy -- the shop said no,
        // or the item came under an id GoBot does not know: no second
        // purchase, on a newer frame too.
        MiningPlanner g;
        j = to_buy(g, h, b, t);
        TripSnap got = h;
        got.equip1.slots[20] = TripSlot{ 8123456, 1 };
        got.equip1.seq = 1;
        g.done(j, TripResult::Retry, got, t);
        const TripJob w = g.next(got, b, t + 4000);     // past the Retry's kBackoffMs (3 s), not a 30 s gap
        check(j.kind == TripJobKind::Buy && g.pickaxe_unconfirmed() && !g.resting(t + 1000) && !g.resting(t + 4000) &&
                  w.kind == TripJobKind::Walk && w.x == 225.0f,
              "Trang bị 1 gained an item since the Buy: no second purchase -- and no 30 s gap promising one (the "
              "2026.11.24 review): on to the mine at once");
        // The guard is about pickaxes (2026.11.24 review): a newer frame with
        // no more of them -- what came was something else, a drop picked up
        // on the trip -- clears it; it waited for ever before.
        g.done(w, TripResult::Busy, got, t + 4000);
        TripSnap later = got;
        later.equip1.seq = 2;
        const TripJob again = g.next(later, b, t + 5000);
        check(!g.pickaxe_unconfirmed() && again.kind == TripJobKind::Walk && again.x == kMinerX,
              "a newer frame of Trang bị 1 with no more pickaxes than before the Buy: the guard off, buying again -- "
              "an unrelated item gained never holds it for ever");
    }
    {
        // Purchases that do not go: tried again 30 s apart, with no limit.
        MiningPlanner g;
        bool again = true;
        std::uint64_t when = t;
        for (int i = 0; i < 5; ++i) {
            j = to_buy(g, h, b, when);
            again = again && j.kind == TripJobKind::Buy;
            g.done(j, TripResult::Retry, h, when);
            again = again && !g.pickaxe_unconfirmed() && g.resting(when + kTripRetryMs - 1) &&
                    g.next(h, b, when + kTripRetryMs - 1).kind == TripJobKind::None &&
                    g.status().find("Chưa mua được") != std::string::npos &&
                    g.status().find("30 giây") != std::string::npos;
            when += kTripRetryMs;
        }
        check(again, "a purchase that does not go: tried again 30 s on (the owner's shop trouble), five times and "
                     "on -- never \"mua 3 lần chưa được\", never a stop");
        MiningPlanner r;
        j = r.next(h, b, t);
        r.done(j, TripResult::Ok, h, t);
        j = r.next(h, b, t);
        r.done(j, TripResult::Ok, h, t);
        j = r.next(h, b, t);
        r.done(j, TripResult::Retry, h, t);
        check(j.kind == TripJobKind::PickRow && r.resting(t + kTripRetryMs - 1) && !r.resting(t + kTripRetryMs) &&
                  r.status().find("Shop của Thợ Mỏ") != std::string::npos,
              "Thợ Mỏ's Shop row not found: 30 s too, not 3");
    }
    {
        // "Mua khi còn" 1: the pickaxe bought seen in Trang bị 1 clears the
        // guard -- and one more is bought, as the numbers say; one worn all
        // along clears nothing.
        MiningConfig b1 = b;
        b1.buy_when = 1;
        MiningPlanner g;
        j = to_buy(g, h, b1, t);
        g.done(j, TripResult::Ok, h, t);
        TripSnap one = h;
        wear(one, false, 1);
        one.equip1.seq = 1;
        const TripJob w = g.next(one, b1, t + 1000);
        check(j.kind == TripJobKind::Buy && !g.pickaxe_unconfirmed() && w.kind == TripJobKind::Walk && w.x == kMinerX,
              "the pickaxe bought seen in Trang bị 1: the guard off -- with 'Mua khi còn' 1 another bought");
        MiningPlanner o;
        TripSnap wearing = h;
        wear(wearing, true, 0);
        j = to_buy(o, wearing, b1, t);
        o.done(j, TripResult::Ok, wearing, t);
        const TripJob v = o.next(wearing, b1, t + 1000);
        check(j.kind == TripJobKind::Buy && o.pickaxe_unconfirmed() && v.kind == TripJobKind::Walk && v.x == 225.0f,
              "one worn all along: the guard stays -- no second purchase before Trang bị 1 says");
    }

    // Three failures in a row: a 30 s gap, then again -- with no limit
    // (2026.11.24: "thao tác thất bại 3 lần" stopped Đào khoáng).
    MiningPlanner f;
    TripSnap g = at(kTown, 1, 0, 1500.0f);
    wear(g, true, 0);
    std::uint64_t tt = t;
    bool rounds = true;
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 3; ++i) {
            j = f.next(g, c, tt);
            rounds = rounds && j.kind == TripJobKind::Walk;
            f.done(j, TripResult::Ok, g, tt);         // the walk
            j = f.next(g, c, tt);
            rounds = rounds && j.kind == TripJobKind::Talk;
            f.done(j, TripResult::Retry, g, tt);      // the talk fails
            tt += 3'000;
        }
        rounds = rounds && f.resting(tt) && f.next(g, c, tt).kind == TripJobKind::None &&
                 f.status().find("thất bại 3 lần") != std::string::npos &&
                 f.status().find("thử lại sau 5 giây") != std::string::npos;
        tt += kRoundGapMs;
    }
    check(rounds && kRoundGapMs == 5'000,
          "a job failing three times in a row: a gap of a few seconds (kRoundGapMs, not the shop's 30 s -- the "
          "2026.11.24 review), the planner asking again after it -- four rounds and on, never a stop");
    MiningConfig off = c;
    off.enabled = false;
    f.next(g, off, tt - kRoundGapMs + 1000);
    check(!f.resting(tt - kRoundGapMs + 1000) && f.next(g, c, tt - kRoundGapMs + 1000).kind == TripJobKind::Walk,
          "unticked in a gap: forgotten, and ticked again it starts afresh at once");

    // Digging that does not start: another spot each time, never the same
    // one again and again (2026.11.24).
    {
        MiningPlanner d;
        TripSnap m1 = at("t21_s71", 1, 900.0f, 700.0f);
        wear(m1, true, 0);
        j = d.next(m1, c, t);
        const bool first = j.kind == TripJobKind::Walk && j.x == 900.0f;
        d.done(j, TripResult::Ok, m1, t);
        j = d.next(m1, c, t);
        const bool dig1 = j.kind == TripJobKind::StartDig;
        d.done(j, TripResult::Retry, m1, t);
        j = d.next(m1, c, t + 3000);
        const float second = j.x;
        check(first && dig1 && j.kind == TripJobKind::Walk && std::abs(second - 900.0f) >= 64.0f,
              "digging did not start at 900: a walk to another spot (" + std::to_string(static_cast<int>(second)) + ")");
        TripSnap m2 = m1;
        m2.x = second;
        d.done(j, TripResult::Ok, m2, t + 3000);
        j = d.next(m2, c, t + 3000);
        const bool dig2 = j.kind == TripJobKind::StartDig;
        d.done(j, TripResult::Retry, m2, t + 3000);
        j = d.next(m2, c, t + 6000);
        check(dig2 && j.kind == TripJobKind::Walk && std::abs(j.x - 900.0f) >= 32.0f && std::abs(j.x - second) >= 32.0f,
              "not there either: a third spot, neither of the two (" + std::to_string(static_cast<int>(j.x)) + ")");
        TripSnap m3 = m2;
        m3.x = j.x;
        d.done(j, TripResult::Ok, m3, t + 6000);
        j = d.next(m3, c, t + 6000);
        d.done(j, TripResult::Retry, m3, t + 6000);
        check(j.kind == TripJobKind::StartDig && d.resting(t + 7000) && d.status().find("thất bại 3 lần") != std::string::npos,
              "the third in a row: a gap of 5 s -- then the spots again");
    }
    // No spot at all: a gap, then looked for again -- never a stop.
    {
        MiningPlanner n;
        TripSnap m = at("t21_s71", 1, 900.0f, 700.0f);
        wear(m, true, 0);
        m.same_floor = [](int, int, int) { return false; };
        j = n.next(m, c, t);
        check(j.kind == TripJobKind::None && n.resting(t + kRoundGapMs - 1) && !n.resting(t + kRoundGapMs) &&
                  n.status().find("Chưa tìm được chỗ đào") != std::string::npos &&
                  n.status().find("thử lại sau 5 giây") != std::string::npos,
              "no spot on the floor: a gap of 5 s, saying so -- it was \"Đào khoáng dừng\"");
        m.same_floor = nullptr;
        j = n.next(m, c, t + kRoundGapMs);
        check(j.kind == TripJobKind::Walk && j.tol_x == kSpotTolX, "5 s on, a spot to be had: walked to");
    }
    // The map change after the Teleporter's row waited for 30 s (15 s
    // before: a slow load taken for a failure, the row picked -- and paid --
    // again); three that never came: a gap, then again.
    {
        MiningPlanner w;
        TripSnap town = at(kTown, 1, 975.0f, 1525.0f);
        wear(town, true, 0);
        std::uint64_t when = t;
        bool slow_ok = true, failed = true;
        for (int i = 0; i < 3; ++i) {
            for (int step = 0; step < 3; ++step) {
                const TripJob x = w.next(town, c, when);
                failed = failed && x.kind == (step == 0 ? TripJobKind::Walk
                                              : step == 1 ? TripJobKind::Talk
                                                          : TripJobKind::PickRow);
                w.done(x, TripResult::Ok, town, when);
            }
            slow_ok = slow_ok && w.next(town, c, when + 25'000).kind == TripJobKind::None &&
                      w.status() == "Chờ chuyển map";
            w.next(town, c, when + 31'000);
            failed = failed && w.status().find("Không chuyển được map") != std::string::npos;
            when += 34'000;
        }
        check(slow_ok, "25 s after the Teleporter's row: still waiting for the map (laggy PCs)");
        check(failed && w.resting(when - 34'000 + 31'000 + 1000) && w.status().find("(3 lần)") != std::string::npos &&
                  w.status().find("thử lại sau 5 giây") != std::string::npos,
              "no map change three times: a gap of 5 s -- not \"Đào khoáng dừng\"");
    }
}

void test_cothu_planner() {
    section("Expedition: the Cổ Thụ planner, GoC's 25 steps");
    CoThuConfig c;
    c.enabled = true;
    c.role = 0;
    c.minimum_members = 2;
    c.wait_seconds = 10;
    CoThuPlanner p;
    std::uint64_t t = 2'000'000;

    TripSnap s = at(kCoThuLobby, 1, 500.0f, 2864.0f);
    s.party = 1;
    TripJob j = p.next(s, c, t);
    check(j.kind == TripJobKind::None && p.status().find("cần tối thiểu 2") != std::string::npos,
          "the leader alone, two wanted: waits");
    s.other.known = true;
    s.other.slots.assign(72, TripSlot{});
    s.party = 2;
    j = p.next(s, c, t);
    check(j.kind == TripJobKind::None && p.status().find("Đèn Cấp Cao") != std::string::npos,
          "no Đèn Cấp Cao in Khác: says so");
    s.other.slots[4] = TripSlot{ kLampItem, 2 };
    p.next(s, c, t);
    j = p.next(s, c, t + 5000);
    check(j.kind == TripJobKind::None && p.status().find("sau") != std::string::npos, "enough: the countdown");
    j = p.next(s, c, t + 11'000);
    check(j.kind == TripJobKind::Walk && j.x == kCoThuNpcX && j.y == kCoThuNpcY, "then to the NPC");
    p.done(j, TripResult::Ok, s, t + 11'000);
    j = p.next(s, c, t + 11'000);
    check(j.kind == TripJobKind::Talk, "talk");
    p.done(j, TripResult::Ok, s, t + 11'000);
    j = p.next(s, c, t + 11'000);
    check(j.kind == TripJobKind::PickRow && j.text == kCoThuRow, "Ngọc Thụ Thần - Level trên 160");
    p.done(j, TripResult::Ok, s, t + 11'000);
    j = p.next(s, c, t + 11'000);
    check(j.kind == TripJobKind::ClickOk, "its Xác Nhận");
    p.done(j, TripResult::Ok, s, t + 11'000);
    j = p.next(s, c, t + 11'000);
    check(j.kind == TripJobKind::WatchRefusal, "then a watch for a refusal");
    p.done(j, TripResult::Refused, s, t + 12'000);
    j = p.next(s, c, t + 13'000);
    check(j.kind == TripJobKind::None && p.status().find("thử lại sau") != std::string::npos,
          "refused: tried again after entry_retry_seconds");

    // Into Phòng chờ.
    s = at(kCoThuWait, 2, 300.0f, 2298.0f);
    j = p.next(s, c, t + 80'000);
    check(p.runs() == 1, "lobby to Phòng chờ: one run");
    check(j.kind == TripJobKind::Walk && j.x == kCoThuDoorX, "to the boss door");
    p.done(j, TripResult::Ok, s, t + 80'000);
    j = p.next(s, c, t + 80'000);
    check(j.kind == TripJobKind::Gate && j.count == 34, "↓ every 300 ms, 34 times at most");
    p.done(j, TripResult::Ok, s, t + 80'000);

    // The boss.
    c.boss_position = 1;
    s = at(kCoThuBoss, 3, 1000.0f, 800.0f);
    j = p.next(s, c, t + 90'000);
    check(j.kind == TripJobKind::Fight && j.x == kCoThuLowerX && j.y == kCoThuLowerY, "Tầng dưới: fight there");
    p.done(j, TripResult::Ok, s, t + 90'000);
    TripMob boss;
    boss.type = kCoThuBossType;
    boss.hp = 50000;
    s.mobs.push_back(boss);
    j = p.next(s, c, t + 91'000);
    check(j.kind == TripJobKind::None && p.status() == "Đang đánh boss", "the boss seen: fighting");
    s.mobs.clear();
    j = p.next(s, c, t + 91'500);
    check(p.step() == CoThuPlanner::Step::Fighting, "gone half a second: still fighting");
    j = p.next(s, c, t + 93'000);
    j = p.next(s, c, t + 93'000);
    check(j.kind == TripJobKind::Loot && j.count == c.loot_presses, "gone 1.5 s: dead, the loot");
    p.done(j, TripResult::Ok, s, t + 93'000);

    // The reward.
    s = at(kCoThuReward, 4, 100.0f, 2280.0f);
    j = p.next(s, c, t + 100'000);
    check(j.kind == TripJobKind::Walk && j.x == kCoThuRewardX, "to the reward NPC");
    p.done(j, TripResult::Ok, s, t + 100'000);
    j = p.next(s, c, t + 100'000);
    p.done(j, TripResult::Ok, s, t + 100'000);
    j = p.next(s, c, t + 100'000);
    check(j.kind == TripJobKind::CompleteQuest, "talk, then the quest's complete button");
    p.done(j, TripResult::Ok, s, t + 100'000);
    j = p.next(s, c, t + 100'000);
    check(j.kind == TripJobKind::ExitRoom && j.count == 40, "then out: 40 tries at most");

    // A member waits in the lobby.
    CoThuConfig m = c;
    m.role = 1;
    CoThuPlanner q;
    j = q.next(at(kCoThuLobby, 1, 0, 0), m, t);
    check(j.kind == TripJobKind::None && q.status().find("chờ đội trưởng") != std::string::npos,
          "a member: ready, waiting for the leader");
    TripSnap dead = at(kCoThuBoss, 1, 0, 0);
    dead.hp = 0;
    j = q.next(dead, m, t);
    check(j.kind == TripJobKind::None && q.status().find("chết") != std::string::npos, "dead: waits for the revive");
}

void test_colong_planner() {
    section("Expedition: the Cổ Long planner (Ngọc Long Tổ)");
    CoThuConfig c;
    c.enabled = true;
    c.role = 0;
    c.minimum_members = 1;
    c.wait_seconds = 0;
    DungeonPlanner p(kCoLong);
    std::uint64_t t = 3'000'000;

    check(p.zone(kCoLongLobby) == DungeonPlanner::Zone::Lobby && p.zone(kCoLongBoss) == DungeonPlanner::Zone::Boss &&
              p.zone(kCoThuLobby) == DungeonPlanner::Zone::None,
          "its own maps: t60_s19, t1901_s1..s3");
    TripSnap s = at(kCoLongLobby, 1, 200.0f, 802.0f);
    TripJob j = p.next(s, c, t);
    check(j.kind == TripJobKind::None && p.status().find("túi Khác") != std::string::npos,
          "bag Khác not read yet: waits for it a while");
    s.other.known = true;
    s.other.slots.assign(72, TripSlot{});
    j = p.next(s, c, t + 1000);
    check(j.kind == TripJobKind::Walk && j.x == kCoLongNpcX && j.y == kCoLongNpcY,
          "no Lư Hương: to the NPC to buy one");
    p.done(j, TripResult::Ok, s, t + 1000);
    j = p.next(s, c, t + 1000);
    check(j.kind == TripJobKind::Talk, "talk to it");
    p.done(j, TripResult::Ok, s, t + 1000);
    j = p.next(s, c, t + 1000);
    check(j.kind == TripJobKind::PickRow && j.text == kShopRowText, "its Shop row");
    p.done(j, TripResult::Ok, s, t + 1000);
    j = p.next(s, c, t + 1000);
    check(j.kind == TripJobKind::Buy && j.item == kIncenseItem && j.item2 == kIncenseItem2 && j.other_tab &&
              j.count == 1 && j.text == kIncenseName,
          "buy one Lư Hương Long Mỹ, into Khác");
    p.done(j, TripResult::Ok, s, t + 1000);
    s.other.slots[7] = TripSlot{ kIncenseItem2, 1 };
    j = p.next(s, c, t + 2000);
    j = p.next(s, c, t + 2000);
    check(j.kind == TripJobKind::Walk && j.x == kCoLongNpcX, "with it (either id): to the NPC to go in");
    p.done(j, TripResult::Ok, s, t + 2000);
    j = p.next(s, c, t + 2000);
    p.done(j, TripResult::Ok, s, t + 2000);
    j = p.next(s, c, t + 2000);
    check(j.kind == TripJobKind::PickRow && j.text == kCoLongRow, "Ngọc Long Tổ - Level trên 160");
    p.done(j, TripResult::Ok, s, t + 2000);
    j = p.next(s, c, t + 2000);
    check(j.kind == TripJobKind::ClickOk && j.count == 40, "its Xác Nhận, 40 looks");

    // The boss: no boss known, fought until the reward's room.
    DungeonPlanner b(kCoLong);
    c.boss_position = 1;
    TripSnap r = at(kCoLongBoss, 5, 500.0f, 633.0f);
    j = b.next(r, c, t);
    check(j.kind == TripJobKind::Fight && j.x == kCoLongRightX && j.y == kCoLongRightY, "Bên phải: fight there");
    b.done(j, TripResult::Ok, r, t);
    j = b.next(r, c, t + 60'000);
    check(j.kind == TripJobKind::None && b.step() == DungeonPlanner::Step::Fighting,
          "no mob to watch: still fighting a minute on");
    r = at(kCoLongReward, 6, 108.0f, 689.0f);
    j = b.next(r, c, t + 61'000);
    check(j.kind == TripJobKind::Talk && j.by_click && j.stem == kCoLongReward && j.x == kCoLongRewardX &&
              j.y == kCoLongRewardY,
          "moved to the reward's room: its NPC clicked from where you come in, no walk (the owner's way)");

    // Out within 30 s (the owner, 2026-11-19): the reward given up at 15 s.
    b.done(j, TripResult::Ok, r, t + 62'000);
    j = b.next(r, c, t + 62'000);
    check(j.kind == TripJobKind::CompleteQuest && j.item == c.talk_key,
          "its Hoàn Thành, the talk key for when it is not seen");
    b.done(j, TripResult::Retry, r, t + 70'000);
    j = b.next(r, c, t + 70'000);
    check(j.kind == TripJobKind::Talk && j.by_click, "not taken: the NPC clicked again, no 3 s wait");
    b.done(j, TripResult::Ok, r, t + 71'000);
    b.next(r, c, t + 71'000);
    j = b.next(r, c, t + 76'000);
    check(j.kind == TripJobKind::None, "(the Complete job still out)");
    b.done(TripJob{ TripJobKind::CompleteQuest }, TripResult::Retry, r, t + 76'000);
    j = b.next(r, c, t + 76'500);
    check(j.kind == TripJobKind::ExitRoom && j.text == "Thoát khỏi Cõi Vàng" && j.by_click && j.max_ms > 0 &&
              j.max_ms <= 10'000,
          "15 s in the room: the reward given up -- the NPC clicked, its row \"Thoát khỏi Cõi Vàng\", Xác Nhận");
    bool still_out = true;
    for (int i = 0; i < 6; ++i) {
        b.done(j, TripResult::Retry, r, t + 80'000 + i * 1000);
        j = b.next(r, c, t + 80'500 + i * 1000);
        still_out = still_out && j.kind == TripJobKind::ExitRoom;
    }
    check(still_out && !b.resting(t + 86'000),
          "the way out not working: tried again every 0.3 s, never back to the reward, never a gap");

    // The NPC never clicked (no name plate seen): walked to after three.
    DungeonPlanner w(kCoLong);
    std::uint64_t tw = t;
    for (int i = 0; i < 3; ++i) {
        j = w.next(r, c, tw);
        check(j.kind == TripJobKind::Talk, "a click on the NPC, try " + std::to_string(i + 1));
        w.done(j, TripResult::Retry, r, tw);
        tw += 400;
    }
    j = w.next(r, c, tw);
    check(j.kind == TripJobKind::Walk && j.x == kCoLongRewardX && j.max_ms > 0 && j.max_ms <= 15'000,
          "three clicks that did not go: walked to it (12 s at most), the click again from there");

    // Cổ Thụ keeps its walk.
    DungeonPlanner ct(kCoThu);
    j = ct.next(at(kCoThuReward, 4, 100.0f, 2280.0f), c, t);
    check(j.kind == TripJobKind::Walk && !j.by_click, "Cổ Thụ: still walks to its reward NPC");
    // Purchases that do not go: tried again 30 s apart, with no limit
    // (2026.11.24: three of them stopped Phó bản until it was unticked).
    DungeonPlanner f(kCoLong);
    TripSnap e = at(kCoLongLobby, 1, 847.0f, 802.0f);
    e.other.known = true;
    e.other.slots.assign(72, TripSlot{});
    std::uint64_t tt = t;
    bool again = true;
    for (int i = 0; i < 5; ++i) {
        for (int k = 0; k < 4; ++k) {
            j = f.next(e, c, tt);
            again = again && j.kind == (k == 0   ? TripJobKind::Walk
                                        : k == 1 ? TripJobKind::Talk
                                        : k == 2 ? TripJobKind::PickRow
                                                 : TripJobKind::Buy);
            f.done(j, k == 3 ? TripResult::Retry : TripResult::Ok, e, tt);
        }
        const TripJob gap = f.next(e, c, tt + kTripRetryMs - 1000);
        again = again && gap.kind == TripJobKind::None && f.resting(tt + kTripRetryMs - 1000) &&
                !f.ticket_unconfirmed() &&
                f.status().find("(lần " + std::to_string(i + 1) + ")") != std::string::npos &&
                f.status().find("thử lại sau 30 giây") != std::string::npos;
        tt += kTripRetryMs;
    }
    check(again, "a purchase that does not go: tried again 30 s on, five times and on -- \"(lần N) -- thử lại sau 30 "
                 "giây\", never \"3 lần mua\", never a stop: " + f.status());

    // The money guard (2026.11.24): one purchase -- or what may have been one
    // -- before bag Khác says what came of it.
    const auto to_buy = [&](DungeonPlanner& dp, const TripSnap& lobby, std::uint64_t from) {
        for (int step = 0; step < 3; ++step) {
            const TripJob next_job = dp.next(lobby, c, from);
            if (next_job.kind == TripJobKind::None) return TripJob{};
            dp.done(next_job, TripResult::Ok, lobby, from);
        }
        return dp.next(lobby, c, from);
    };
    // Turns from `from`, 4 s apart, each job answered `answer`: whether any
    // was a purchase or a step towards one. Through a whole way in and the
    // entry's 30 s run out, and the leader's Ready again.
    const auto bought_again = [&](DungeonPlanner& dp, const TripSnap& lobby, std::uint64_t from, TripResult answer) {
        bool any = false;
        for (int turn = 0; turn < 24; ++turn) {
            const std::uint64_t when = from + static_cast<std::uint64_t>(turn) * 4000;
            const TripJob next_job = dp.next(lobby, c, when);
            const DungeonPlanner::Step now_step = dp.step();
            any = any || next_job.kind == TripJobKind::Buy || now_step == DungeonPlanner::Step::BuyWalk ||
                  now_step == DungeonPlanner::Step::BuyTalk || now_step == DungeonPlanner::Step::BuyShop ||
                  now_step == DungeonPlanner::Step::BuyTicket;
            if (next_job.kind != TripJobKind::None) dp.done(next_job, answer, lobby, when);
        }
        return any;
    };
    {
        // Bought, and Khác not come again: no second purchase -- the leader
        // goes on to the NPC's entry; the guard goes with the ticket.
        DungeonPlanner g(kCoLong);
        j = to_buy(g, e, t);
        const bool buy = j.kind == TripJobKind::Buy;
        g.done(j, TripResult::Ok, e, t);
        j = g.next(e, c, t + 1000);
        check(buy && g.ticket_unconfirmed() && j.kind == TripJobKind::None &&
                  g.status().find("không mua thêm") != std::string::npos &&
                  g.status().find("game sẽ báo nếu thiếu") != std::string::npos,
              "bought, bag Khác the same frame: no second purchase, saying so -- " + g.status());
        j = g.next(e, c, t + 1000);
        check(j.kind == TripJobKind::Walk && g.step() == DungeonPlanner::Step::WalkNpc,
              "the countdown over: to the NPC to go in, not to buy");
        g.done(j, TripResult::Ok, e, t + 1000);
        j = g.next(e, c, t + 1000);
        g.done(j, TripResult::Ok, e, t + 1000);
        j = g.next(e, c, t + 1000);
        check(j.kind == TripJobKind::PickRow && j.text == kCoLongRow, "its Ngọc Long Tổ row: the entry -- the NPC says "
                                                                     "if the ticket is missing");
        g.done(j, TripResult::Ok, e, t + 1000);
        j = g.next(e, c, t + 1000);
        g.done(j, TripResult::Ok, e, t + 1000);
        j = g.next(e, c, t + 1000);
        g.done(j, TripResult::Ok, e, t + 1000);     // the watch: no refusal
        g.next(at(kCoLongWait, 2, 300.0f, 689.0f), c, t + 5000);
        check(!g.ticket_unconfirmed() && g.runs() == 1, "into the waiting room: the ticket used, the guard gone");
    }
    {
        // A newer frame of Khác with nothing new: the purchase did not go --
        // bought again.
        DungeonPlanner g(kCoLong);
        j = to_buy(g, e, t);
        g.done(j, TripResult::Ok, e, t);
        TripSnap newer = e;
        newer.other.seq = e.other.seq + 1;
        j = g.next(newer, c, t + 1000);
        check(j.kind == TripJobKind::Walk && g.step() == DungeonPlanner::Step::BuyWalk && !g.ticket_unconfirmed(),
              "a newer frame of bag Khác with nothing new: bought again");
    }
    {
        // Khác holding something new under an id GoBot does not know -- VTC's
        // Lư Hương, perhaps -- after a purchase the shop said no to: no
        // second purchase, on newer frames too; the leader goes in.
        DungeonPlanner g(kCoLong);
        j = to_buy(g, e, t);
        TripSnap got = e;
        got.other.slots[9] = TripSlot{ 8999999, 1 };
        got.other.seq = e.other.seq + 1;
        g.done(j, TripResult::Retry, got, t);
        // No 30 s gap (2026.11.24 review): no purchase follows, so none
        // promising one -- the leader goes on at once, unconfirmed.
        const bool held = g.ticket_unconfirmed() && !g.resting(t + 1000) &&
                          g.status().find("chưa có gói tin túi Khác xác nhận") != std::string::npos;
        TripSnap later = got;
        later.other.seq = got.other.seq + 1;
        check(held && !bought_again(g, later, t + 1000, TripResult::Ok),
              "bag Khác gained an item under an unknown id: no second purchase, newer frames too, no gap -- the "
              "leader goes on to the entry at once");
    }
    {
        // Busy after the purchase -- the hold lost with the ticket in Khác
        // under an unknown id: looked at from Ready, never bought again.
        DungeonPlanner g(kCoLong);
        j = to_buy(g, e, t);
        TripSnap got = e;
        got.other.slots[11] = TripSlot{ 8999998, 1 };
        got.other.seq = e.other.seq + 1;
        g.done(j, TripResult::Busy, got, t);
        check(g.ticket_unconfirmed() && g.step() == DungeonPlanner::Step::Ready &&
                  !bought_again(g, got, t + 1000, TripResult::Ok),
              "Busy with bag Khác holding more: the guard on, never bought again");
        DungeonPlanner n(kCoLong);
        j = to_buy(n, e, t);
        n.done(j, TripResult::Busy, e, t);
        check(!n.ticket_unconfirmed() && n.step() == DungeonPlanner::Step::BuyTicket && !n.resting(t + 1000) &&
                  n.next(e, c, t + 1000).kind == TripJobKind::None && n.next(e, c, t + 5000).kind == TripJobKind::Buy,
              "Busy with nothing come: the purchase asked again after a moment for bag Khác's frame (5 s), no 30 s "
              "gap");
        // The 2026.11.24 review: Busy with nothing seen yet, then a newer
        // frame of Khác holding an unknown id -- the first purchase, its
        // frame late: taken as bought against the Buy's own baseline, never
        // a second one.
        DungeonPlanner late(kCoLong);
        j = to_buy(late, e, t);
        late.done(j, TripResult::Busy, e, t);
        TripSnap came = e;
        came.other.slots[13] = TripSlot{ 8999997, 1 };
        came.other.seq = e.other.seq + 1;
        const TripJob after_busy = late.next(came, c, t + 5000);
        check(after_busy.kind != TripJobKind::Buy && late.ticket_unconfirmed() &&
                  late.step() == DungeonPlanner::Step::Ready && !bought_again(late, came, t + 6000, TripResult::Ok),
              "Busy, then a newer bag Khác frame with an unknown id: taken as the purchase -- no second Buy");
    }

    // A member never buys.
    CoThuConfig m = c;
    m.role = 1;
    DungeonPlanner q(kCoLong);
    j = q.next(at(kCoLongLobby, 1, 0, 0), m, t);
    check(j.kind == TripJobKind::None && q.status().find("chờ đội trưởng") != std::string::npos,
          "a member: waits for the leader");
}

/// 2026.11.21: the dungeon planner and the other features -- the restarts no
/// longer held against a later run (B5), a refusal that was not one (B7), the
/// way out past 30 s given room (B11).
void test_dungeon_blocks() {
    section("Expedition (2026.11.21): the dungeon not stopped for good by other features' holds");
    CoThuConfig c;
    c.enabled = true;
    c.role = 0;
    c.minimum_members = 1;
    c.wait_seconds = 0;
    std::uint64_t t = 5'000'000;

    // B5: walks to the boss door run out under holds -- three in one visit a
    // restart. Four restarts in one run, then the next run: three more fails
    // are its first restart, not the fifth.
    {
        DungeonPlanner p(kCoThu);
        const TripSnap lobby = at(kCoThuLobby, 1, 500.0f, 2864.0f);
        p.next(lobby, c, t);
        TripSnap wait = at(kCoThuWait, 2, 300.0f, 2298.0f);
        const auto fail_walks = [&](int n) {
            for (int i = 0; i < n; ++i) {
                t += 4000;
                const TripJob j = p.next(wait, c, t);
                if (j.kind != TripJobKind::Walk) return false;
                p.done(j, TripResult::Retry, wait, t);
            }
            return true;
        };
        const bool walked = fail_walks(12);
        check(walked && !p.resting(t) && p.runs() == 1, "four restarts in the first run's waiting room: going on");
        t += 4000;
        p.next(at(kCoThuLobby, 3, 500.0f, 2864.0f), c, t);
        wait = at(kCoThuWait, 4, 300.0f, 2298.0f);
        const bool again = fail_walks(3);
        check(again && !p.resting(t) && p.runs() == 2,
              "the next run's first restart: not the fifth of the night -- Phó bản goes on");
        // Five restarts within one run (2026.11.24): a 30 s gap, then the
        // room's steps from the start -- round after round, never a stop (it
        // was "Cổ Thụ dừng: ... thử nhiều lần vẫn chưa được").
        bool rounds = true;
        for (int round = 0; round < 3; ++round) {
            const bool more = fail_walks(round == 0 ? 12 : 15);
            const std::uint64_t failed_at = t;
            const TripJob gap = p.next(wait, c, failed_at + 1000);
            const TripJob late = p.next(wait, c, failed_at + kRoundGapMs - 1);
            rounds = rounds && more && gap.kind == TripJobKind::None && late.kind == TripJobKind::None &&
                     p.resting(failed_at + 1000) &&
                     p.status().find("chưa được sau nhiều lần -- thử lại từ đầu sau 5 giây") != std::string::npos;
            t = failed_at + kRoundGapMs - 4000;    // fail_walks steps 4 s on: the first walk at the gap's end
            rounds = rounds && !p.resting(t + 4000);
        }
        check(rounds, "five restarts within one run: a gap of 5 s (kRoundGapMs), then from the waiting room's start again -- three "
                      "rounds and on, never stopped: " + p.status());
        const TripJob door = p.next(wait, c, t + 4000);
        check(door.kind == TripJobKind::Walk && door.x == kCoThuDoorX, "after the gap: to the boss door again");
    }

    // B7: a refusal seen with entry_retry off stopped the dungeon (and the
    // party moved in all the same undid it). Since 2026.11.24 the old setting
    // is unread: tried again after entry_retry_seconds, as with it on.
    {
        CoThuConfig off = c;
        off.entry_retry = false;
        off.entry_retry_seconds = 45;
        DungeonPlanner p(kCoThu);
        TripSnap s = at(kCoThuLobby, 1, 500.0f, 2864.0f);
        s.other.known = true;
        s.other.slots.assign(72, TripSlot{});
        s.other.slots[4] = TripSlot{ kLampItem, 2 };
        TripJob j = p.next(s, off, t);
        for (int i = 0; i < 8 && j.kind != TripJobKind::WatchRefusal; ++i) {
            if (j.kind != TripJobKind::None) p.done(j, TripResult::Ok, s, t);
            t += 1000;
            j = p.next(s, off, t);
        }
        check(j.kind == TripJobKind::WatchRefusal, "the leader's way in, to the watch for a refusal");
        p.done(j, TripResult::Refused, s, t);
        j = p.next(s, off, t + 1000);
        check(j.kind == TripJobKind::None && p.step() == DungeonPlanner::Step::RetryWait &&
                  p.status().find("thử lại sau 44 giây") != std::string::npos,
              "refused with Tự thử lại unticked in an old file: tried again all the same, after its 45 s -- " +
                  p.status());
        TripJob walk;
        for (int i = 0; i < 4 && walk.kind == TripJobKind::None; ++i) walk = p.next(s, off, t + 45'000);
        check(walk.kind == TripJobKind::Walk && walk.x == kCoThuNpcX && p.step() == DungeonPlanner::Step::WalkNpc,
              "45 s on: to the NPC again -- never stopped");
        p.done(walk, TripResult::Ok, s, t + 46'000);
        j = p.next(at(kCoThuWait, 2, 300.0f, 2298.0f), off, t + 61'000);
        check(j.kind == TripJobKind::Walk && j.x == kCoThuDoorX && p.runs() == 1,
              "in the waiting room: going on to the boss door, the run counted");
    }

    // The reward room's failures are never a restart nor a gap (2026.11.24):
    // four restarts in the waiting room, then three clicks on the reward NPC
    // that did not go -- the NPC walked to 0.3 s on, where the fifth restart
    // of the run stopped Phó bản in the room (out within 30 s, the owner's
    // rule).
    {
        DungeonPlanner p(kCoLong);
        p.next(at(kCoLongLobby, 1, 200.0f, 802.0f), c, t);
        const TripSnap wait = at(kCoLongWait, 2, 300.0f, 689.0f);
        bool walked = true;
        for (int i = 0; i < 12; ++i) {
            t += 4000;
            const TripJob j = p.next(wait, c, t);
            walked = walked && j.kind == TripJobKind::Walk;
            p.done(j, TripResult::Retry, wait, t);
        }
        t += 4000;
        const TripSnap boss = at(kCoLongBoss, 3, 500.0f, 633.0f);
        TripJob j = p.next(boss, c, t);
        const bool fight = j.kind == TripJobKind::Fight;
        p.done(j, TripResult::Ok, boss, t);
        const TripSnap reward = at(kCoLongReward, 4, 108.0f, 689.0f);
        t += 60'000;
        bool clicks = true;
        for (int i = 0; i < 3; ++i) {
            j = p.next(reward, c, t);
            clicks = clicks && j.kind == TripJobKind::Talk && j.by_click;
            p.done(j, TripResult::Retry, reward, t);
            t += 400;
        }
        j = p.next(reward, c, t);
        check(walked && fight && clicks && !p.resting(t) && j.kind == TripJobKind::Walk && j.x == kCoLongRewardX,
              "restarts at four, three failed clicks on the reward NPC: walked to it 0.3 s on -- no restart counted, "
              "no gap");
    }

    // B11: the way out not working past kLeaveWithinMs: tried on and on,
    // never stopped, but 3 s apart, the game free between the tries.
    {
        DungeonPlanner p(kCoLong);
        const TripSnap r = at(kCoLongReward, 6, 108.0f, 689.0f);
        TripJob j = p.next(r, c, t);
        const std::uint64_t in = t;
        // Straight to the way out: 15 s in, the reward given up.
        p.done(j, TripResult::Retry, r, in);
        j = p.next(r, c, in + kRewardGiveUpMs);
        check(j.kind == TripJobKind::ExitRoom, "15 s in: the way out");
        p.done(j, TripResult::Retry, r, in + 20'000);
        j = p.next(r, c, in + 20'400);
        check(j.kind == TripJobKind::ExitRoom, "within 30 s: tried again 0.3 s on");
        p.done(j, TripResult::Retry, r, in + kLeaveWithinMs + 1000);
        const TripJob soon = p.next(r, c, in + kLeaveWithinMs + 1500);
        j = p.next(r, c, in + kLeaveWithinMs + 4100);
        check(soon.kind == TripJobKind::None && j.kind == TripJobKind::ExitRoom &&
                  !p.resting(in + kLeaveWithinMs + 1500),
              "past 30 s: tried on, 3 s apart -- the macro's buffs, the popups, the trash pass get the game between");
        bool on = true;
        std::uint64_t when = in + kLeaveWithinMs + 4100;
        for (int i = 0; i < 20; ++i) {
            p.done(j, TripResult::Retry, r, when);
            when += 3100;
            j = p.next(r, c, when);
            on = on && j.kind == TripJobKind::ExitRoom;
        }
        check(on && !p.resting(when), "a minute more: still trying, never given up, never a 30 s gap (out of the "
                                      "room is the point)");
    }
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
int count_in(const std::string& text, const std::string& part) {
    int n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++n;
    return n;
}
bool in_order(const std::string& text, std::initializer_list<const char*> parts) {
    std::size_t at = 0;
    for (const char* p : parts) {
        at = text.find(p, at);
        if (at == std::string::npos) return false;
        at += std::strlen(p);
    }
    return true;
}
std::string slice(const std::string& src, const std::string& from, const std::string& to) {
    const std::size_t a = src.find(from);
    if (a == std::string::npos) return {};
    const std::size_t b = src.find(to, a + from.size());
    return src.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

/// 2026.11.21, the audit's B6: the other features' timed jobs wait out the
/// boss fight and the reward room (TripQuietFlag), capped; and B10: the
/// check-in board's ESC with the character down, when the reviver sees no
/// death panel (checkin_down_esc).
void test_trip_quiet() {
    section("Expedition (2026.11.21, B6): the other features' timed jobs wait out the boss fight and the reward "
            "room");
    using Z = DungeonPlanner::Zone;
    check(DungeonPlanner::quiet_in(Z::Boss) == TripQuiet::Boss && DungeonPlanner::quiet_in(Z::Reward) == TripQuiet::Reward &&
              DungeonPlanner::quiet_in(Z::Lobby) == TripQuiet::None && DungeonPlanner::quiet_in(Z::Wait) == TripQuiet::None &&
              DungeonPlanner::quiet_in(Z::None) == TripQuiet::None,
          "the boss's room and the reward's only: never the lobby, the waiting room, or anywhere else (ordinary "
          "hunting unchanged)");
    check(DungeonPlanner::quiet_in(DungeonPlanner::zone_in(kCoLong, kCoLongBoss)) == TripQuiet::Boss &&
              DungeonPlanner::quiet_in(DungeonPlanner::zone_in(kCoLong, kCoLongReward)) == TripQuiet::Reward &&
              DungeonPlanner::quiet_in(DungeonPlanner::zone_in(kCoThu, kCoThuBoss)) == TripQuiet::Boss &&
              DungeonPlanner::quiet_in(DungeonPlanner::zone_in(kCoThu, kCoThuWait)) == TripQuiet::None &&
              DungeonPlanner::quiet_in(DungeonPlanner::zone_in(kCoLong, "t1_s1")) == TripQuiet::None,
          "by the maps: t1901_s2 / t1981_s2 the fight, t1901_s3 the reward; t1981_s1 and a town nothing");

    const std::uint64_t t = 9'000'000;
    {
        TripQuietFlag f;
        check(!f.on(t), "nothing set: nothing waits");
        // The worker sets it every turn through a 23-minute Cổ Long fight.
        f.set(TripQuiet::Boss, t);
        bool held = true;
        for (std::uint64_t at = t; at < t + 23 * 60'000; at += 5'000) {
            f.set(TripQuiet::Boss, at);
            held = held && f.at(at + 200) == TripQuiet::Boss;
        }
        check(held, "the boss fight, set each turn: on for all of a 23-minute fight");
        f.set(TripQuiet::Boss, t + TripQuietFlag::kBossMostMs - 1000);
        check(f.on(t + TripQuietFlag::kBossMostMs - 1) && !f.on(t + TripQuietFlag::kBossMostMs),
              "capped at 30 min from the room's entry, still set or not: the bag and the potions get their turn");
        // Into the reward room: its own cap, from its entry.
        const std::uint64_t r = t + 24 * 60'000;
        f.set(TripQuiet::Reward, r);
        f.set(TripQuiet::Reward, r + 20'000);
        check(f.at(r + 20'000) == TripQuiet::Reward, "the reward room: on, the boss's cap not carried over");
        f.set(TripQuiet::Reward, r + TripQuietFlag::kRewardMostMs - 500);
        check(f.on(r + TripQuietFlag::kRewardMostMs - 1) && !f.on(r + TripQuietFlag::kRewardMostMs) &&
                  TripQuietFlag::kRewardMostMs == kLeaveWithinMs + 15'000,
              "the reward room capped at its 30 s and 15 s more");
        // Back to the lobby: off.
        f.set(TripQuiet::None, r + 60'000);
        check(!f.on(r + 60'000), "out of the dungeon: off at once");
    }
    {
        TripQuietFlag f;
        f.set(TripQuiet::Boss, t);
        check(f.on(t + TripQuietFlag::kStaleMs - 1) && !f.on(t + TripQuietFlag::kStaleMs),
              "a worker stopped or stuck sets it no more: off after 30 s by itself");
        f.set(TripQuiet::Reward, t + 1000);
        f.clear();
        check(!f.on(t + 1000), "clear (a stop, the hunt stopped, the tabs unticked): off");
    }
    // 2026.11.21: in the boss's room a timed job waits no more than
    // kBossPutOffMostMs -- the owner's 30 min, the shop put off with the rest;
    // the reward room's wait as it was, nothing outside a dungeon.
    {
        const std::uint64_t most = TripQuietFlag::kBossPutOffMostMs;
        check(most == 30 * 60'000 && TripQuietFlag::holds_job(TripQuiet::Boss, 0, t) &&
                  TripQuietFlag::holds_job(TripQuiet::Boss, t, t + most - 1) &&
                  !TripQuietFlag::holds_job(TripQuiet::Boss, t, t + most),
              "the boss's room: a timed job waits 30 min at most (the owner's choice), then goes");
        check(TripQuietFlag::holds_job(TripQuiet::Reward, t, t + 60 * 60'000) &&
                  !TripQuietFlag::holds_job(TripQuiet::None, 0, t) && !TripQuietFlag::holds_job(TripQuiet::None, t, t),
              "the reward room: held for all of its own cap; outside a dungeon: never");
        check(DungeonPlanner::trip_outlives_stop(Z::Boss) && !DungeonPlanner::trip_outlives_stop(Z::Lobby) &&
                  !DungeonPlanner::trip_outlives_stop(Z::Wait) && !DungeonPlanner::trip_outlives_stop(Z::Reward) &&
                  !DungeonPlanner::trip_outlives_stop(Z::None) &&
                  DungeonPlanner::trip_outlives_stop(DungeonPlanner::zone_in(kCoLong, kCoLongBoss)) &&
                  DungeonPlanner::trip_outlives_stop(DungeonPlanner::zone_in(kCoThu, kCoThuBoss)),
              "Tự đánh quái stopped: the trip kept in the boss's room only (its Fight trip is sent once), ended "
              "anywhere else");
    }

    // The gates that read it: Tự chat's line, Nhiệm vụ nhanh's round.
    {
        ChatGateInput in;
        in.window = true;
        in.focused = true;
        in.hp_pct = 100.0f;
        check(chat_gate(in).send, "chat: everything clear -- the line goes");
        in.dungeon = trip_quiet_vi(TripQuiet::Boss);
        const ChatGate g = chat_gate(in);
        check(!g.send && g.wait_ms == 5000 && has(g.why, "đánh boss"),
              "chat: in the boss fight -- put off 5 s, looked at again, said why: " + g.why);
    }
    {
        QuestGateInput in;
        in.window = true;
        in.focused = true;
        in.hp_pct = 100.0f;
        in.hp_floor = 1;
        check(quest_gate(in).go, "quest: everything clear -- the round goes");
        in.dungeon = trip_quiet_vi(TripQuiet::Reward);
        const QuestGate g = quest_gate(in);
        check(!g.go && has(g.why, "phòng nhận thưởng"), "quest: in the reward room -- a timed round waits: " + g.why);
        in.asked = true;
        check(quest_gate(in).go, "quest: one asked for with Làm ngay 1 lần goes");
    }

    section("Expedition (2026.11.21, B10): the check-in board's ESC with the character down");
    {
        const std::uint64_t n = 7'000'000;
        check(checkin_down_esc(true, true, n - 1200, n), "the board, the reviver running, no panel on its last look "
                                                         "1.2 s ago: the ESC goes");
        check(!checkin_down_esc(false, true, n - 1200, n), "a notice or the book: never with the character down");
        check(!checkin_down_esc(true, false, n - 1200, n), "the reviver not running: nobody knows the panel is not "
                                                          "on top -- no ESC");
        check(!checkin_down_esc(true, true, 0, n), "the reviver's last turn saw a panel, waited, or had no frame: no ESC");
        check(!checkin_down_esc(true, true, n - kCheckinDownLookMs - 1, n), "a look older than 4 s: no ESC");
    }

#ifdef GOBOT_SOURCE_DIR
    section("Expedition (2026.11.21, B6/B10): the workers read it");
    const std::string root = GOBOT_SOURCE_DIR;
    const std::string gui = read_file(root + "/apps/gobot_gui/gui_app.cpp");
    const std::string trip = read_file(root + "/apps/gobot_gui/simple_expedition.cpp");
    const std::string quest = read_file(root + "/apps/gobot_gui/simple_quest.cpp");
    const std::string party = read_file(root + "/apps/gobot_gui/simple_party.cpp");
    const std::string popups = read_file(root + "/apps/gobot_gui/simple_popups.cpp");
    check(in_order(gui, { "const bool charm_due = timed && !held && settings.use_charms",
                          "if ((pass_due || charm_due || shop_due) && !held && !trash_left_held_) {",
                          "const TripQuiet trip = trip_quiet_.at(now_ms());", "continue;",
                          "if ((pass_due || charm_due || shop_due) && chat_first()) continue;",
                          "if (request == 4 || request == 5" }),
          "the trash worker: a timed pass, charm run or shop round waits before anything runs -- no clock moved");
    const std::string gate = slice(gui, "const TripQuiet trip = trip_quiet_.at(now_ms());",
                                   "if ((pass_due || charm_due || shop_due) && chat_first()) continue;");
    check(!gate.empty() && !has(gate, "next =") && !has(gate, "charm_check_ms =") && !has(gate, "shop_due_ms ="),
          "the trash worker's wait moves no clock: each job is still due, and goes as it is over");
    check(has(gui, "in.dungeon = asked ? std::string() : std::string(trip_quiet_vi(trip_quiet_.at(now_ms())));"),
          "Tự chat: a timed line waits; Gửi thử goes");
    check(has(quest, "in.dungeon = trip_quiet_vi(trip_quiet_.at(now_ms()));"), "Nhiệm vụ nhanh: its gates read it");
    check(in_order(party, { "bool GuiApp::party_may_invite(", "const TripQuiet trip = trip_quiet_.at(now_ms());",
                            "return false;", "reader_ready(" }),
          "the party: no invite in it");
    check(in_order(trip, { "void GuiApp::stop_expedition(bool wait) {", "trip_quiet_.clear();" }) &&
              in_order(trip, { "if (!hunter_ || !hunter_->running()) {", "trip_quiet_.clear();" }) &&
              in_order(trip, { "if (dungeon) j = dungeon->next(s, *dungeon_cfg, now);", "if (s.world) {",
                               "dungeon ? DungeonPlanner::quiet_in(dungeon->zone(s.stem)) : TripQuiet::None;",
                               "trip_quiet_.set(q, now);" }) &&
              in_order(trip, { "say_expedition(\"Đã dừng\");" }) && count_in(trip, "trip_quiet_.clear();") == 4,
          "the expedition worker: set each turn from the dungeon's room; cleared on a stop, the hunt stopped, the "
          "tabs unticked, the worker's end");
    check(in_order(trip, { "const auto do_exit_room = [&](const TripJob& j) -> TripResult {",
                           "if (tried == TripResult::Busy) ++busy;", "if (rounds > 0 && busy == rounds) {",
                           "return TripResult::Busy;" }),
          "the way out: rounds the hold was not had for are no tries -- Busy, asked again soon");
    check(!has(popups, "trip_quiet_"), "the popup closer: no wait -- a box over the game is still closed");
    check(in_order(popups, { "const auto hp_low = [this, &closer, &said_down_esc] {",
                             "checkin_down_esc(closer.last_about(PopupKind::CheckinBoard), revive_running_.load(),",
                             "revive_no_panel_ms_.load(), now_ms())" }) &&
              in_order(gui, { "const Reviver::Turn turn = reviver.turn();",
                              "revive_no_panel_ms_ = turn == Reviver::Turn::Alive ? now_ms() : 0;" }),
          "B10: the board's ESC with the character down only on the reviver's look that saw no panel");

    section("Expedition (2026.11.21, the review): the GUI's wiring that keeps the trip from blocking the rest");
    check(in_order(gui, { "const TripQuiet trip = trip_quiet_.at(now_ms());",
                          "if (trip != TripQuiet::None && trip_off_since == 0) trip_off_since = now_ms();",
                          "if (TripQuietFlag::holds_job(trip, trip_off_since, now_ms())) {", "continue;",
                          "trip_told = TripQuiet::None;", "trip_off_since = 0;" }),
          "the trash worker: the boss's room puts a timed job off kBossPutOffMostMs at most");
    const std::string stopped = slice(trip, "if (!hunter_ || !hunter_->running()) {", "said_hunter = false;");
    check(in_order(stopped, { "DungeonPlanner::trip_outlives_stop(cothu.zone(stem_here))",
                              "DungeonPlanner::trip_outlives_stop(colong.zone(stem_here))",
                              "if (!boss_room) hunter_->end_trip();" }) &&
              count_in(stopped, "end_trip()") == 1,
          "Tự đánh quái stopped: the trip ended, but in the boss's room");
    const std::string walk = slice(trip, "const auto do_walk = [&](const TripJob& j, bool fight) -> TripResult {",
                                   "const auto do_gate = [&]");
    check(in_order(walk, { "if (!hunter_ || !hunter_->running()) return TripResult::Busy;",
                           "if (other_hold || !input_.window_focused()) blocked += std::min<std::uint64_t>(now - last, 1000);",
                           "if (s.hp_known && s.hp == 0) return TripResult::Busy;",
                           "(j.max_ms ? 0 : std::min(blocked, now - start))", "now - start > kWalkMostMs" }),
          "do_walk: another hold's or another window's time not counted (capped 1 s a turn, never a walk with its own "
          "limit, never past kWalkMostMs); Busy with the hunter stopped or the character down");
    const std::string refusal = slice(trip, "const auto do_watch_refusal = [&]", "const auto do_start_dig = [&]");
    const std::string notice = slice(trip, "const auto do_close_notice = [&]", "return TripResult::Ok;");
    check(has(refusal, "find_notice_ok(f, &ok)") && !has(refusal, "find_ok(f, &ok)") &&
              has(notice, "find_notice_ok(f, &ok)") && !has(notice, " find_ok("),
          "the entry's watch and the notice's close click a one-button notice only (B7)");
    check(in_order(trip, { "const bool keys_job = j.kind != TripJobKind::Walk && j.kind != TripJobKind::Fight;",
                           "if (keys_job && hunter_) hunter_->expect_map_change(kExpectMapMs);",
                           "const TripResult r = run_job(j);",
                           "if (keys_job && hunter_) hunter_->expect_map_change(kExpectMapMs);" }),
          "a job of keys bracketed by the expedition's map-change window (B3)");
    const std::string zones = slice(trip, "if (dungeon && hunter_) {", "std::lock_guard<std::mutex> lock(worker_mutex_);");
    check(in_order(zones, { "if (z != DungeonPlanner::Zone::Lobby || st == DungeonPlanner::Step::Watch ||",
                            "st == DungeonPlanner::Step::WaitEntry || st == DungeonPlanner::Step::Member)",
                            "hunter_->expect_map_change(kExpectMapMs);" }),
          "the window renewed in the dungeon's rooms, and in the lobby for the leader's watch and a waiting member");
    const std::string lobby = slice(zones, "if (j.kind == TripJobKind::None && z == DungeonPlanner::Zone::Lobby &&",
                                    "hunter_->end_trip();");
    check(has(lobby, "Step::Ready") && has(lobby, "Step::Member") && !has(lobby, "fatal") &&
              !has(lobby, "Countdown") && !has(lobby, "RetryWait"),
          "the lobby's trip ended for Ready and a member (no planner stops any more) -- never the countdown or the "
          "retry's wait (B20)");
#else
    skip("no source folder\n");
#endif
}

/// 2026.11.24, the owner: "Kiểm tra các phần khác xem có phần nào không làm
/// được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác" -- and "thử
/// lại luôn ... chứ không phải là dừng 90s". No tab of Đào khoáng and Phó bản
/// stops for good: failed purchases tried again 30 s on, rounds of failures a
/// 30 s gap, the hunter fighting meanwhile; and nothing bought again while the
/// last purchase is not yet seen in the bag (tab_gained). The planners' cases
/// are in their own tests; here the pure part and the worker's wiring.
void test_no_stop() {
    section("Expedition (2026.11.24): no tab stops for good -- tab_gained, the 30 s, the worker's gap");
    check(kTripRetryMs == 30'000 && kTripRetryMs == bag::kShopTroubleAgainMs,
          "the gap and a failed purchase's wait: 30 s, the owner's for the shop's trouble");
    {
        TripPackets::Tab before;
        before.known = true;
        before.slots.assign(72, TripSlot{});
        before.slots[3] = TripSlot{ kIncenseItem, 2 };
        TripPackets::Tab after = before;
        check(!tab_gained(before, after), "the same tab: nothing gained");
        after.slots[3].count = 3;
        check(tab_gained(before, after), "more of an item in its slot: gained");
        after = before;
        after.slots[3].count = 1;
        check(!tab_gained(before, after), "less of it (one used): nothing gained");
        after = before;
        after.slots[3] = TripSlot{};
        check(!tab_gained(before, after), "a slot emptied: nothing gained");
        after = before;
        after.slots[10] = TripSlot{ 8999999, 1 };
        check(tab_gained(before, after), "an item under an id GoBot does not know, in an empty slot: gained -- VTC's "
                                         "Lư Hương may not be GoC's");
        after = before;
        after.slots[3] = TripSlot{ kIncenseItem2, 1 };
        check(tab_gained(before, after), "another item in a slot: gained");
        TripPackets::Tab unread = after;
        unread.known = false;
        check(!tab_gained(before, unread), "a tab not read: nothing gained");
        TripPackets::Tab never;
        TripPackets::Tab empty;
        empty.known = true;
        empty.slots.assign(72, TripSlot{});
        check(tab_gained(never, before) && !tab_gained(never, empty),
              "nothing read before: whatever is held now is new; an empty tab is not");
    }

#ifdef GOBOT_SOURCE_DIR
    section("Expedition (2026.11.24): the worker and the tabs -- no stop, no red line, the gap's trip end");
    const std::string root = GOBOT_SOURCE_DIR;
    const std::string trip = read_file(root + "/apps/gobot_gui/simple_expedition.cpp");
    const std::string planner = read_file(root + "/src/expedition.cpp");
    const std::string header = read_file(root + "/include/gobot/expedition.hpp");
    check(!trip.empty() && !planner.empty() && !header.empty(), "the worker, the planners and their header found");
    check(!has(trip, "fatal()") && !has(trip, "TripJobKind::Fatal") && !has(trip, "ui.stopped") &&
              !has(trip, "coloured(kBad, stopped"),
          "the worker: no planner's stop looked at, no red stopped line on either tab");
    check(!has(planner, "fatal_") && !has(planner, "TripResult::Fatal") && !has(planner, "TripJobKind::Fatal") &&
              !has(header, "bool fatal()") && !has(planner, "kMaxTicketBuys") && !has(planner, "bought_"),
          "the planners: no fatal_, no Fatal job or result, no cap of three purchases");
    check(in_order(trip, { "if (hunter_ && j.kind == TripJobKind::None &&", "(mine_job && mining.resting(now))",
                           "dungeon->resting(now) && dungeon->zone(s.stem) != DungeonPlanner::Zone::Boss",
                           "hunter_->end_trip();", "if (j.kind == TripJobKind::None) continue;" }),
          "a planner in its gap: the trip ended before the turn's wait -- mining, the lobby, the waiting room -- so that "
          "the hunter fights; never in the boss's room");
    check(in_order(trip, { "if (mine_job) mining.done(j, r, after, over);",
                           "else if (dungeon) dungeon->done(j, r, after, over);",
                           "if (mine_job ? mining.resting(over) : (dungeon && dungeon->resting(over)))",
                           "say_expedition(mine_job ? mining.status() : dungeon->status());" }),
          "a gap a job began: its line on the card's log, where a stop's red line was");
    check(!has(trip, "ImGui::Checkbox(\"Tự thử lại khi không vào được\"") &&
              in_order(trip, { "label(\"Thử lại sau (giây)\");",
                               "changed |= ImGui::SliderInt(\"##retry\", &c.entry_retry_seconds, 5, 300);",
                               "hint(\"Không vào được phó bản thì GoBot luôn thử lại sau số giây này.\");" }) &&
              has(planner, "entry_retry = ini.get_bool(s, \"entry_retry_enabled\", entry_retry);") &&
              has(planner, "ini.set_bool(s, \"entry_retry_enabled\", entry_retry);") && !has(planner, "= c.entry_retry;"),
          "Tự thử lại khi không vào được off the card, Thử lại sau always there; its key still read and written (old "
          "files load), never acted on");
    check(has(trip, "không có thì GoBot mua 1 ở Shop của NPC; mua chưa được thì ") &&
              has(trip, "thử lại sau 30 giây; chưa thấy gói tin túi Khác xác nhận lần mua trước thì không mua thêm.") &&
              !has(trip, "(tối đa 3 lần)"),
          "the Cổ Long card: tried again after 30 s, none bought before the last is seen -- no \"tối đa 3 lần\"");
#else
    skip("no source folder\n");
#endif
}

/// A frame of one colour, opaque.
Frame filled(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < f.pixels.size(); i += 4) {
        f.pixels[i] = b;
        f.pixels[i + 1] = g;
        f.pixels[i + 2] = r;
        f.pixels[i + 3] = 255;
    }
    return f;
}

/// `pic` drawn into `f` with its top-left at x, y: its opaque pixels.
void paste(Frame& f, const Frame& pic, int x, int y) {
    for (int py = 0; py < pic.height; ++py)
        for (int px = 0; px < pic.width; ++px) {
            const int fx = x + px, fy = y + py;
            if (fx < 0 || fy < 0 || fx >= f.width || fy >= f.height) continue;
            const std::size_t s = (static_cast<std::size_t>(py) * pic.width + px) * 4;
            if (pic.pixels[s + 3] < 128) continue;
            const std::size_t d = (static_cast<std::size_t>(fy) * f.width + fx) * 4;
            for (int c = 0; c < 3; ++c) f.pixels[d + c] = pic.pixels[s + c];
            f.pixels[d + 3] = 255;
        }
}

/// 2026.11.24, the owner: "khi làm đội trưởng để đi Cổ Long thì không mua
/// được Lư Hương. Cụ thể là khi ấn vào Lư Hương thì không ấn mua mà lại tắt
/// luôn shop mua". An NPC's shop bought from as the player buys: the list's
/// names, the icon double-clicked, the quantity box read and answered, the
/// game's no; and the worker's source: never Mua, never ESC three times.
void test_npc_shop() {
    section("Expedition (2026.11.24): an NPC's shop -- Lư Hương Long Mỹ bought as the player buys");
    check(shop_list_text(kIncenseName) == "LưHươngLongMỹ" && shop_list_text("Cuốc Hắc Quang") == "CuốcHắcQuang" &&
              shop_list_text(kShopRowText) == kShopRowText,
          "the list's words with no spaces, as GoC's picture of Thợ Mỏ's shop has CuốcHắcQuang");

    // What the box needs typed for one.
    {
        const QuantityKeys one = quantity_keys(1, 1);
        check(one.ok && one.erase == 0 && one.digits.empty(), "the 1 the game puts in the box: nothing typed");
        const QuantityKeys empty = quantity_keys(0, 1);
        check(empty.ok && empty.erase == 0 && empty.digits == "1", "an empty box: 1 typed");
        const QuantityKeys five = quantity_keys(5, 1);
        check(five.ok && five.erase == 2 && five.digits == "1",
              "5 in it: erased, a Backspace more than its digits, then 1 -- never 51");
        const QuantityKeys hundred = quantity_keys(1, 100);
        check(hundred.ok && hundred.erase == 0 && hundred.digits == "00", "1 towards 100: the rest, as the potion "
                                                                          "shop types it");
        const QuantityKeys twelve = quantity_keys(13, 12);
        check(twelve.ok && twelve.erase == 3 && twelve.digits == "12", "13 towards 12: erased and typed whole");
        check(!quantity_keys(CountReader::kUnreadable, 1).ok && !quantity_keys(-3, 1).ok && !quantity_keys(1, 0).ok,
              "a box that does not read, or is gone, or nothing wanted: nothing typed (Hủy Bỏ)");
    }

    // What a look after the double-click is taken for.
    {
        BuyLookIn in;
        check(buy_look(in) == BuyLook::Nothing, "nothing up: looked at again");
        in.dialogs = 1;
        check(buy_look(in) == BuyLook::Nothing, "a dialog whose box is half drawn: looked at again");
        in.box = true;
        in.question = true;
        check(buy_look(in) == BuyLook::Question, "one dialog, its box drawn and reading in the game's question frame: "
                                                 "the quantity question");
        in.question = false;
        check(buy_look(in) == BuyLook::Other, "a box GoBot cannot tell for the shop's: never typed into -- Hủy Bỏ");
        in.box = false;
        in.bare = true;
        check(buy_look(in) == BuyLook::Other, "the light bar where a box would be, but not in the game's question "
                                              "frame (an invitation, a trade): never confirmed -- Hủy Bỏ");
        in.question = true;
        check(buy_look(in) == BuyLook::Confirm, "the light bar where a box would be, in the game's question frame: a "
                                                "question with no box -- Xác Nhận (a piece of equipment)");
        in.known = true;
        check(buy_look(in) == BuyLook::Other, "the revive question, the bin's, a registration: never confirmed");
        in = BuyLookIn{};
        in.dialogs = 2;
        in.bare = true;
        check(buy_look(in) == BuyLook::Other, "two dialogs: none of them the purchase's");
        in = BuyLookIn{};
        in.message = true;
        check(buy_look(in) == BuyLook::Message, "a lone message: the game's no -- closed, the shop left");
        in.full = true;
        check(buy_look(in) == BuyLook::Full, "the full-bag message");
        in.more_in_bag = true;
        in.dialogs = 1;
        check(buy_look(in) == BuyLook::Bought, "more of the item in the bag's frames: bought, whatever is up");
    }

    // The icon's place from the name's, by numbers: the shop's buttons at
    // 600,600 put the left column's cells at 414, the right one's at 566.
    {
        const ScreenRect buttons{ 600, 600, 119, 21 };
        const int left = buttons.x - bag::kShopButtonsDx;
        check(left == 414 && bag::kShopCellPitch == 152, "the columns: 186 left of the buttons, 152 apart (the "
                                                         "potions' row)");
        const auto name_ending = [](int last_x, int y, int w) { return ScreenRect{ last_x + 2 - w, y, w, 14 }; };
        const bag::ShopCell a = bag::shop_cell(name_ending(left + bag::kShopNameLastX, 343, 70), buttons);
        check(a.ok && a.column == 0 && a.x == left + bag::kShopMpX && a.y == 350 + bag::kShopIconBelowName &&
                  a.by_name_x == a.x,
              "a name ending where the left column's names end: its icon's middle, 17 across, 8 under the name");
        const bag::ShopCell r = bag::shop_cell(name_ending(left + bag::kShopCellPitch + bag::kShopNameLastX, 301, 30),
                                               buttons);
        check(r.ok && r.column == 1 && r.x == left + bag::kShopCellPitch + bag::kShopMpX,
              "a short name in the right column: that column's icon, whatever the name's length");
        const bag::ShopCell wide = bag::shop_cell(name_ending(left + bag::kShopNameLastX + 12, 343, 90), buttons);
        check(wide.ok && wide.x == a.x, "MenuText's letters 12 wider than the game's: the same icon");
        const bag::ShopCell piece = bag::shop_cell(name_ending(left + bag::kShopNameLastX - 40, 343, 40), buttons);
        check(!piece.ok && piece.column == 0 && std::string(piece.why).find("end") != std::string::npos,
              "a name ending 40 short of where names end -- a piece of another's: not clicked");
        const bag::ShopCell gap = bag::shop_cell(ScreenRect{ left + 140, 343, 30, 14 }, buttons);
        check(!gap.ok && gap.column == -1, "between the columns: neither, not clicked");
        const bag::ShopCell below = bag::shop_cell(name_ending(left + bag::kShopNameLastX, 610, 70), buttons);
        check(!below.ok, "beside the buttons, not over the list: not clicked");
        const ScreenRect list = bag::shop_list(buttons);
        check(list.x <= left + bag::kShopNameFieldX && list.right() >= left + bag::kShopCellPitch + bag::kShopNameFieldEndX &&
                  list.bottom() < buttons.y && list.y > buttons.y + bag::kShopWindowFromButtons.y,
              "the list's names searched: both name fields, under the window's top and above its buttons");
    }

    // On the pictures: GoC's own cells of three NPCs' shops, put in a shop at
    // their places -- the names found as the game draws them, the icon hit.
    const std::string ini = exe_relative("configs/templates.ini");
    Frame mua, pickaxe, rod, bait;
    if (!TemplateStore::load_one(bag::kShopButtons, ini, mua) ||
        !TemplateStore::load_one("grabbed/pickaxe_npc_shop", ini, pickaxe) ||
        !TemplateStore::load_one("grabbed/rod_in_shop", ini, rod) ||
        !TemplateStore::load_one("grabbed/baiter_in_shop", ini, bait)) {
        skip("the shop's pictures (grabbed/ngs_buy_button, pickaxe_npc_shop, rod_in_shop, baiter_in_shop) are not "
             "installed\n");
    } else {
        // An NPC's shop is the potion shop's window: its Mua | Thoát as GoC
        // cut them from Thợ Mỏ's shop are the buttons the shop is found by.
        Frame goc_buttons;
        if (TemplateStore::load_one("grabbed/buy_pickaxe", ini, goc_buttons)) {
            const TemplateMatcher::Match m =
                TemplateMatcher::find(goc_buttons, ScreenRect{ 0, 0, goc_buttons.width, goc_buttons.height }, mua, -1.0f);
            char what[200];
            std::snprintf(what, sizeof what,
                          "Thợ Mỏ's Mua | Thoát (GoC's grabbed/buy_pickaxe) are the potion shop's: ngs_buy_button "
                          "found in them at %.2f (do_buy finds the shop at 0.80)",
                          static_cast<double>(m.score));
            check(m.found && m.score >= 0.80f, what);
        } else {
            skip("GoC's grabbed/buy_pickaxe is not installed\n");
        }
        Frame scene = filled(1024, 768, 24, 24, 32);
        const ScreenRect b{ 600, 620, mua.width, mua.height };
        paste(scene, mua, b.x, b.y);
        const int left = b.x - bag::kShopButtonsDx, top = b.y - bag::kShopButtonsDy;
        // The pickaxe's picture starts at its cell's corner; the bait's 2
        // across and 1 down, the rod's 2 across and 3 down. And this client's
        // own potions row (kShopRow, cut from the live shop: both cells' top
        // 21 rows, names and all), at its corner, in the fifth row.
        paste(scene, pickaxe, left, top + 42);
        paste(scene, bait, left + bag::kShopCellPitch - 2, top - 1);
        paste(scene, rod, left - 2, top + 3 * 42 - 3);
        Frame potions;
        const bool own = TemplateStore::load_one(bag::kShopRow, ini, potions);
        if (own) paste(scene, potions, left, top + 5 * 42);
        else skip("grabbed/ngs_hpmp is not installed: this client's own list not tried\n");
        struct Item {
            const char* name;
            int column, cell_x, cell_y;
        };
        std::vector<Item> items{ Item{ "Cuốc Hắc Quang", 0, left, top + 42 },
                                 Item{ "Cá Nhỏ", 1, left + bag::kShopCellPitch, top },
                                 Item{ "Cần Câu Tham Trúc", 0, left, top + 3 * 42 } };
        if (own) {
            items.push_back(Item{ "Rượu lá tre", 0, left, top + 5 * 42 });
            items.push_back(Item{ "Thịt bò khô", 1, left + bag::kShopCellPitch, top + 5 * 42 });
        }
        for (const Item& it : items) {
            const MenuText::Hit bare = MenuText::find(scene, bag::shop_list(b), shop_list_text(it.name));
            const MenuText::Hit spaced = MenuText::find(scene, bag::shop_list(b), it.name);
            const bag::ShopCell cb = bare.found ? bag::shop_cell(bare.at, b) : bag::ShopCell{};
            const bag::ShopCell cs = spaced.found ? bag::shop_cell(spaced.at, b) : bag::ShopCell{};
            const bag::ShopCell& c = cb.ok ? cb : cs;
            const bool on_icon = c.x > it.cell_x && c.x < it.cell_x + 35 && c.y > it.cell_y && c.y < it.cell_y + 36;
            char what[400];
            std::snprintf(what, sizeof what,
                          "%s in the list: found (no spaces %.2f at %d,%d %dx%d; as written %.2f at %d,%d %dx%d), its "
                          "column %d, its icon box double-clicked at %d,%d (the box %d..%d x %d..%d)",
                          it.name, static_cast<double>(bare.score), bare.at.x, bare.at.y, bare.at.w, bare.at.h,
                          static_cast<double>(spaced.score), spaced.at.x, spaced.at.y, spaced.at.w, spaced.at.h,
                          c.column, c.x, c.y, it.cell_x + 1, it.cell_x + 34, it.cell_y + 1, it.cell_y + 35);
            check(c.ok && c.column == it.column && on_icon, what);
        }
        // Names that are not in it: never an item's place to click.
        for (const char* other : { "Cuốc Gỗ", "Lư Hương Long Mỹ", "Cuốc Kim Cang" }) {
            bool clicked = false;
            float best = 0.0f;
            for (const std::string& text : { shop_list_text(other), std::string(other) }) {
                const MenuText::Hit h = MenuText::find(scene, bag::shop_list(b), text);
                best = std::max(best, h.score);
                clicked = clicked || (h.found && bag::shop_cell(h.at, b).ok);
            }
            char what[200];
            std::snprintf(what, sizeof what, "%s, not in the list: no item's place clicked (best %.2f)", other,
                          static_cast<double>(best));
            check(!clicked, what);
        }
    }

    // The quantity box: there on the sell question (the buy question is drawn
    // as it is), not on the bin's question nor the revive question; the
    // question's buttons still there; and the 1 the game puts in it read.
    Frame pair, sell, ask, revive;
    if (!TemplateStore::load_one("grabbed/confirm_button", ini, pair) ||
        !TemplateStore::load_one(bag::kSellQuestion, ini, sell) ||
        !TemplateStore::load_one("grabbed/del_item_confirm_1", ini, ask) ||
        !TemplateStore::load_one("grabbed/hoisinh_confirm", ini, revive)) {
        skip("the dialogs' pictures (grabbed/confirm_button, sell_confirm, del_item_confirm_1, hoisinh_confirm) are "
             "not installed\n");
    } else {
        TemplateStore st;
        st.put("grabbed/confirm_button", pair);
        st.put("grabbed/del_item_confirm_1", ask);
        struct Box {
            const Frame* pic;
            const char* name;
            bool box;
        };
        CountReader digits;
        const bool digits_ok = digits.load(GlyphFont::default_dir());
        for (const Box& each : { Box{ &sell, "the sell question", true }, Box{ &ask, "the bin's question", false },
                                 Box{ &revive, "the revive question", false } }) {
            Frame scene = filled(640, 480, 24, 24, 32);
            paste(scene, *each.pic, 220, 200);
            const std::vector<bag::Dialog> ds = bag::find_dialogs(scene, st);
            const float field = ds.size() == 1 ? bag::quantity_field(scene, ds.front().pair) : -1.0f;
            char what[200];
            std::snprintf(what, sizeof what, "%s: one dialog, its box's place %.2f the box's dark field -- %s",
                          each.name, static_cast<double>(field), each.box ? "a box" : "no box");
            check(ds.size() == 1 && (each.box ? field >= bag::kFieldBox : (field >= 0.0f && field <= bag::kFieldNone)),
                  what);
            if (ds.size() != 1) continue;
            check(bag::pair_still(scene, st, ds.front().pair) &&
                      !bag::pair_still(filled(640, 480, 24, 24, 32), st, ds.front().pair),
                  std::string(each.name) + ": its buttons still there by pair_still, and not on a frame without it");
            if (each.box) {
                if (digits_ok)
                    check(digits.read_field(scene, bag::quantity_box(ds.front().pair)) == 1,
                          "the sell question's box read through quantity_box: the 1 the game puts there");
                else
                    skip("the game's digits are not installed: the box's 1 not read\n");
            }
        }
        // A question of the game's with no box and words of its own -- the
        // bin's question's frame, its words wiped and others drawn -- is taken
        // for one in the game's question frame (Confirm), the bin's own never.
        if (digits_ok) {
            st.put(bag::kSellQuestion, sell);
            Frame other = ask;
            std::uint8_t r0 = 0, g0 = 0, b0 = 0;
            other.get(20, 12, r0, g0, b0);
            for (int y = 12; y < 40; ++y)
                for (int x = 20; x < 178; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * other.width + x) * 4;
                    const bool stroke = (y == 18 || y == 19 || y == 30 || y == 31) && (x / 7) % 3 != 0;
                    other.pixels[i] = stroke ? 255 : b0;
                    other.pixels[i + 1] = stroke ? 255 : g0;
                    other.pixels[i + 2] = stroke ? 255 : r0;
                }
            Frame scene = filled(640, 480, 24, 24, 32);
            paste(scene, other, 220, 200);
            const std::vector<bag::Dialog> ds = bag::find_dialogs(scene, st);
            const bool one = ds.size() == 1;
            check(one && !ds.front().ours() && bag::quantity_field(scene, ds.front().pair) <= bag::kFieldNone &&
                      bag::shop_question(scene, st, ds.front(), digits, CleanSettings::kBuyFrameDefault, false),
                  "a question with no box and words of its own: in the game's question frame, no box -- Xác Nhận "
                  "(BuyLook::Confirm)");
            Frame bin_scene = filled(640, 480, 24, 24, 32);
            paste(bin_scene, ask, 220, 200);
            const std::vector<bag::Dialog> bin = bag::find_dialogs(bin_scene, st);
            check(bin.size() == 1 && bin.front().ours(), "the bin's question itself: the bin's -- known, never "
                                                          "confirmed by a purchase");
        }
    }

#ifdef GOBOT_SOURCE_DIR
    section("Expedition (2026.11.24): the worker buys as the player does");
    const std::string trip = read_file(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/simple_expedition.cpp");
    const std::string shop = slice(trip, "// ---- an NPC's shop: Lư Hương Long Mỹ, a pickaxe (2026.11.24) ----",
                                   "const auto do_loot = [&]");
    const std::string once = slice(shop, "const auto buy_once = [&]", "const auto close_shop = [&]");
    const std::string closing = slice(shop, "const auto close_shop = [&]", "const auto close_bag_left = [&]");
    const std::string bagleft = slice(shop, "const auto close_bag_left = [&]", "const auto do_buy = [&]");
    const std::string buy = slice(shop, "const auto do_buy = [&]", "const auto do_loot = [&]");
    check(!shop.empty() && !once.empty() && !closing.empty() && !bagleft.empty() && !buy.empty(),
          "the shop's part of the worker found");
    check(!has(shop, "kShopMuaX") && !has(shop, "mua_x"), "Mua is never clicked (it took no key on the live client)");
    check(in_order(once, { "off_list(b);", "sleep_ms(kShopSettleMs);", "MenuText::find(f, bag::shop_list(b), text)",
                           "bag::shop_cell(h.at, b)", "click(cell.x, cell.y, 2)", "off_list(b);" }),
          "the pointer off the list before the name is looked for; the name in the list alone; its icon double-clicked "
          "and the pointer off the list again at once");
    check(count_in(once, "click(cell.x, cell.y") == 1 && !has(once, "click(hit.at") && !has(once, "kShopThoatX"),
          "one double-click on the icon box -- never a click on the name, never near Thoát");
    check(in_order(once, { "while (held_ok() && now_ms() - clicked < static_cast<std::uint64_t>(kReactMs)) {",
                           "bag::quantity_field(g, d.pair)",
                           "bag::shop_question(g, *store, d, digits, bag_settings.buy_frame_score, in.box)",
                           "const BuyLook l = buy_look(in);" }),
          "what comes up waited for kReactMs (the owner's 30 s), each look judged by buy_look -- a question with no "
          "box only in the game's own question frame");
    check(in_order(once, { "const int have = read_box(q.pair, b, box_look);", "quantity_keys(have, 1)",
                           "if (!sent || now_has != 1) {", "click(q.confirm_x(), q.y())" }),
          "the box read, only what is needed for one typed and read back as 1, before Xác Nhận");
    check(in_order(once, { "const std::uint64_t confirmed = now_ms();", "if (got_more()) {", "return ShopTry::Bought;" }),
          "bought when the bag's frames have more of it");
    // The 2026.11.24 review: an item under an id GoBot does not know (VTC's
    // Lư Hương, a pickaxe's variant) counted as bought by the tab having
    // gained anything since before the click -- never the "not in the bag"
    // path that paid for a second one; a 30 s with nothing come up ends the
    // visit (the other jobs have their turn), not two more 30 s tries.
    check(in_order(once, { "const TripPackets::Tab before_tab = tab_of(j);", "tab_gained(before_tab, now)",
                           "click(cell.x, cell.y, 2)", "in.more_in_bag = got_more();" }) &&
              in_order(once, { "if (tab_of(j).seq == seq) {", "if (got_more()) {", "keep(g, \"shop_not_in_bag\");" }),
          "a purchase under an unknown id: the tab read before the double-click, gained since = bought (also just "
          "before the 'not in the bag' path)");
    check(in_order(once, { "case BuyLook::Nothing:", "keep(g, \"shop_no_question\");", "return ShopTry::Over;",
                           "case BuyLook::Question:" }),
          "30 s with nothing come up after the double-click ends the visit (Over), not two more tries of 30 s");
    check(count_in(closing, "press(VK_ESCAPE") == 1 && has(closing, "bag::kShopThoatX") &&
              in_order(closing, { "press(VK_ESCAPE, 50)", "shop_buttons(f, &b)", "click(b.x + bag::kShopThoatX" }),
          "the shop shut the proven way: one ESC, then Thoát only on a look that shows it still up");
    check(in_order(closing, { "shop_buttons(f, nullptr).valid() && !checkin_board_title(f, *store)",
                              "if (none >= 2) return true;", "press(VK_ESCAPE, 50)" }),
          "no shop and no check-in board on two looks before the ESC: nothing pressed (the game's menu never "
          "brought up)");
    check(count_in(bagleft, "press(VK_ESCAPE") == 1 &&
              in_order(bagleft, { "for (int n = 0; n < 2", "looked = grab(f);", "if (!looked) return false;",
                                  "if (!bag_up(f)) return true;", "press(VK_ESCAPE, 50)",
                                  "now_ms() - from < static_cast<std::uint64_t>(kReactMs)" }) &&
              has(bagleft, "bag::find(g, *store, bag_settings.layout).found") && !has(bagleft, "hãy tắt giúp"),
          "the bag left beside the shop: ESC only on a look that shows the bag, a failed look not taken for "
          "closed, each ESC watched for kReactMs, twice at most -- and no asking the player");
    check(!has(buy, "for (int i = 0; i < 3 && held_ok(); ++i) {") && count_in(buy, "press(VK_ESCAPE") == 1 &&
              in_order(buy, { "for (int t = 0; t < kShopTries", "buy_once(j, b, why)", "close_shop(b);",
                              "close_bag_left();" }) &&
              !has(buy, "if (close_shop(b)) close_bag_left();"),
          "no ESC three times: tried again in the same shop, then shut once -- the bag looked at whatever "
          "close_shop said");
    check(in_order(buy, { "keep(f, \"shop_not_seen\");", "bag::find_dialogs(g, *store).empty()",
                          "MenuText::find(g, area, kShopRowText).found", "press(VK_ESCAPE, 50);" }),
          "no shop seen: ESC only when a fresh look shows something to shut (a dialog, a message, the NPC's rows)");
    check(in_order(trip, { "party_covered = hud.covered;", "dungeon->step() == DungeonPlanner::Step::Ready",
                           "now - bag_fix_ms >= static_cast<std::uint64_t>(kTripRetryMs)",
                           "bag::find(f, *store, bag_settings.layout).found", "press(VK_ESCAPE, 50);",
                           "if (j.kind == TripJobKind::None) continue;" }),
          "the bag over the party panels in the lobby: one ESC on a look that shows it, once per 30 s, no limit");
    check(count_in(shop, "keep(") >= 12 && count_in(shop, "say_expedition(") >= 10,
          "a picture kept and a line on the card at each step that does not go");
#else
    skip("no source folder\n");
#endif
}

/// 2026.11.24, the owner: "Khi ra tới bản đồ TổCổLong thì cần mua lại HP cho
/// pet. Cái này ở trong tab Linh Thú của người giữ sân và là item "Thịtbò" =>
/// cần có lựa chọn để mua và mua bao nhiêu slot tương tự như mua HP MP". An
/// arrival into TổCổLong owes one round of the pet's food, once; the lobby's
/// steps wait for it, 90 s at most; and the window's wiring of it.
void test_pet_restock() {
    section("Expedition (2026.11.24): the pet's HP food on reaching TổCổLong -- the arrival, once; the lobby's wait "
            "and its cap");
    const std::uint64_t t = 7'000'000;
    check(std::string(kCoLongLobby) == "t60_s19" && PetRestock::kWaitMostMs == 90'000 && PetRestock::kAgainMs == 30'000,
          "TổCổLong is t60_s19, Cổ Long's lobby; the wait 90 s at most, a round in trouble again 30 s on");
    {
        PetRestock p;
        check(!p.look("", 0, true, t) && !p.look(kCoLongLobby, 0, true, t) && p.owed(t) == 0 && p.due(t) == 0,
              "not in the world, or no map entry yet: nothing owed");
        check(!p.look("t52_s2", 3, true, t) && p.owed(t) == 0 && !p.waits(3, t), "an entry anywhere else: nothing owed");
        const std::uint64_t in = t + 1000;
        check(p.look(kCoLongLobby, 4, true, in) && p.owed(in) == 4 && p.due(in) == 4 && p.waits(4, in) &&
                  p.arrived_ms() == in,
              "an entry into t60_s19 with the box ticked and the bot running: one round owed for it, due at once, the "
              "lobby waiting");
        check(!p.look(kCoLongLobby, 4, true, in + 500) && !p.look(kCoLongLobby, 4, false, in + 600) && p.owed(in + 600) == 4,
              "the same entry looked at again -- by the tick, by the expedition worker: still the one round");
        p.put_off(4, in + 31'000);
        check(p.due(in + 2000) == 0 && p.owed(in + 2000) == 4 && p.waits(4, in + 2000) && p.due(in + 31'000) == 4,
              "put off 30 s: not due till then, still owed, the lobby still waiting");
        p.put_off(9, in + 80'000);
        check(p.due(in + 31'000) == 4, "a put-off for another entry changes nothing");
        {
            // The 2026.11.24 review: a first full try in vain -- the lobby no
            // longer waits, the round still owed and tried again.
            PetRestock q;
            q.look(kCoLongLobby, 6, true, in);
            q.tried(7);
            const bool other = q.waits(6, in + 1000);
            q.put_off(6, in + 31'000);
            q.tried(6);
            check(other && !q.waits(6, in + 2000) && q.owed(in + 2000) == 6 && q.due(in + 31'000) == 6,
                  "a round tried once in vain: the lobby goes on at once, the round still owed and tried 30 s on");
            q.look("t52_s2", 8, true, in + 40'000);
            q.look(kCoLongLobby, 9, true, in + 50'000);
            check(q.waits(9, in + 51'000), "the next arrival: waited for again");
        }
        check(!p.waits(5, in + 2000), "the lobby of another entry does not wait for this one");
        const std::uint64_t cap = in + PetRestock::kWaitMostMs;
        check(p.owed(cap - 1) == 4 && p.owed(cap) == 0 && p.due(cap) == 0 && !p.waits(4, cap),
              "90 s after the arrival: no longer owed, the lobby no longer waits");
        check(!p.expire(cap - 1) && p.expire(cap) && !p.expire(cap + 1000) && p.arrived_ms() == 0,
              "given up at 90 s, said once: until the next arrival");
        check(!p.look(kCoLongLobby, 4, true, cap + 2000) && p.owed(cap + 2000) == 0,
              "and never owed again for that entry, however long it lasts");
    }
    {
        PetRestock p;
        check(p.look(kCoLongLobby, 5, true, t) && p.owed(t) == 5, "an arrival");
        p.over(6);
        check(p.owed(t) == 5, "over for another entry: changes nothing");
        p.over(5);
        check(p.owed(t) == 0 && !p.waits(5, t) && p.due(t) == 0, "bought, or the game's no: over, the lobby goes on");
        check(!p.look(kCoLongLobby, 6, false, t) && p.owed(t) == 0 && !p.look(kCoLongLobby, 6, true, t + 100) &&
                  p.owed(t + 100) == 0,
              "an entry with the box off or the bot stopped: nothing -- not even once it is ticked in the same stay");
        check(p.look(kCoLongLobby, 7, true, t + 200) && p.owed(t + 200) == 7, "the next arrival owes again");
        check(!p.look(kCoLongWait, 8, true, t + 300) && p.owed(t + 300) == 0 && !p.waits(7, t + 300),
              "into the waiting room before it was bought: over -- at the next arrival again");
        check(!p.look(kCoThuLobby, 9, true, t + 400) && p.owed(t + 400) == 0, "Cổ Thụ's lobby is not TổCổLong");
    }

    // The lobby's steps wait for it, 90 s at most.
    CoThuConfig c;
    c.enabled = true;
    c.role = 0;
    c.minimum_members = 1;
    c.wait_seconds = 0;
    TripSnap s = at(kCoLongLobby, 4, 847.0f, 802.0f);
    s.other.known = true;
    s.other.slots.assign(72, TripSlot{});
    s.other.slots[7] = TripSlot{ kIncenseItem, 1 };
    s.pet_restock = true;
    {
        DungeonPlanner p(kCoLong);
        TripJob j = p.next(s, c, t);
        check(j.kind == TripJobKind::None && p.status().find("Chờ mua HP pet") != std::string::npos &&
                  p.step() == DungeonPlanner::Step::Ready,
              "the leader, its Lư Hương in Khác and its party there: \"Chờ mua HP pet\" -- nothing walked");
        j = p.next(s, c, t + 89'000);
        check(j.kind == TripJobKind::None && p.status().find("Chờ mua HP pet") != std::string::npos,
              "89 s on, the round still owed: still waiting");
        j = p.next(s, c, t + 90'000);
        const TripJob walk = p.next(s, c, t + 90'000);
        check(j.kind == TripJobKind::None && walk.kind == TripJobKind::Walk && walk.x == kCoLongNpcX &&
                  !p.resting(t + 90'000),
              "90 s: on without it -- the countdown, then to the NPC; never a stop");
    }
    {
        DungeonPlanner p(kCoLong);
        TripSnap done = s;
        p.next(s, c, t);
        done.pet_restock = false;
        p.next(done, c, t + 20'000);
        const TripJob walk = p.next(done, c, t + 20'000);
        check(walk.kind == TripJobKind::Walk && walk.x == kCoLongNpcX, "bought after 20 s: on at once");
        // A new stay in the lobby -- out to the waiting room and back -- waits
        // afresh.
        p.done(walk, TripResult::Ok, done, t + 21'000);
        const TripSnap waiting = at(kCoLongWait, 5, 300.0f, 689.0f);
        const TripJob door = p.next(waiting, c, t + 30'000);
        p.done(door, TripResult::Ok, waiting, t + 31'000);
        TripSnap back = s;
        back.epoch = 6;
        const TripJob j = p.next(back, c, t + 200'000);
        check(j.kind == TripJobKind::None && p.status().find("Chờ mua HP pet") != std::string::npos,
              "back in the lobby later: the wait afresh, its own 90 s");
    }
    {
        // Nothing owed: no wait at all.
        DungeonPlanner p(kCoLong);
        TripSnap none = s;
        none.pet_restock = false;
        p.next(none, c, t);
        const TripJob j = p.next(none, c, t);
        check(j.kind == TripJobKind::Walk && p.status().find("HP pet") == std::string::npos,
              "no round owed: straight on, as ever");
    }
    {
        // A member: its line says so while it waits for its leader.
        CoThuConfig m = c;
        m.role = 1;
        DungeonPlanner p(kCoLong);
        p.next(s, m, t);
        check(p.status().find("Chờ mua HP pet") != std::string::npos && !p.resting(t), "a member: \"Chờ mua HP pet\"");
        TripSnap done = s;
        done.pet_restock = false;
        p.next(done, m, t + 5000);
        check(p.status().find("chờ đội trưởng") != std::string::npos, "bought: waiting for the leader as ever");
    }
    check(DungeonPlanner::waits_for_pet(DungeonPlanner::Step::Ready) &&
              DungeonPlanner::waits_for_pet(DungeonPlanner::Step::Countdown) &&
              DungeonPlanner::waits_for_pet(DungeonPlanner::Step::Member) &&
              DungeonPlanner::waits_for_pet(DungeonPlanner::Step::RetryWait) &&
              !DungeonPlanner::waits_for_pet(DungeonPlanner::Step::BuyTicket) &&
              !DungeonPlanner::waits_for_pet(DungeonPlanner::Step::TalkNpc) &&
              !DungeonPlanner::waits_for_pet(DungeonPlanner::Step::Watch) &&
              !DungeonPlanner::waits_for_pet(DungeonPlanner::Step::WaitEntry),
          "only the lobby's steps with nothing of the NPC's begun wait: never a walk, a talk, a row or the entry's watch "
          "half way");

#ifdef GOBOT_SOURCE_DIR
    section("Expedition (2026.11.24): the pet's round wired into the window, the trash worker and the dungeon");
    const std::string gui = read_file(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp");
    const std::string trip = read_file(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/simple_expedition.cpp");
    const std::string card = read_file(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/simple_shop.cpp");
    const std::string planner = read_file(std::string(GOBOT_SOURCE_DIR) + "/src/expedition.cpp");
    const std::string tick = slice(gui, "void GuiApp::tick() {", "\n}\n");
    const std::string arrival = slice(gui, "void GuiApp::pet_arrival_tick() {", "\n}\n");
    const std::string wake = slice(gui, "void GuiApp::request_pet() {", "\n}\n");
    const std::string pet = slice(gui, "const auto run_pet = [&](std::uint32_t entry) {", "const auto run_shop = [&]");
    const std::string loop = slice(gui, "while (!trash_stop_) {\n        std::this_thread::sleep_for(", "// The charms', the pass's");
    check(!tick.empty() && !arrival.empty() && !wake.empty() && !pet.empty() && !loop.empty(),
          "gui_app.cpp: the tick, pet_arrival_tick, request_pet, run_pet and the worker's loop found");
    check(in_order(tick, { "watch_left_held();", "pet_arrival_tick();" }),
          "the arrival looked for every tick, after the bin's doubt");
    check(in_order(arrival, { "if (pet_read_ms_ == 0 || now - pet_read_ms_ >= 2000) {",
                              "if (s.load(exe_relative(\"configs/trash.ini\"))) pet_on_ = s.buy_pet && s.buy_pet_slots > 0;",
                              "const bool armed = pet_on_ && simple_running() && !simple_cold_start_;",
                              "pet_armed_ = armed;", "const PacketWorld::Place place = packet_world_.place();",
                              "if (pet_restock_.look(stem, place.epoch, armed, now))",
                              "const std::uint32_t owed = pet_restock_.owed(now);", "if (!simple_running()) {",
                              "pet_restock_.over(owed);", "if (!trash_running_ && now - pet_wake_ms_ >= 5000) {",
                              "request_pet();" }),
          "pet_arrival_tick: the box and its slots from trash.ini every 2 s; armed only with the bot running and no "
          "cold start; each entry looked at; the bot stopped drops what is owed; no worker -- one for it, 5 s apart");
    check(in_order(wake, { "vitals_for_job();", "if (trash_running_) return;", "join_pumping(trash_thread_);",
                           "trash_oneshot_ = true;", "trash_running_ = true;",
                           "trash_thread_ = std::thread([this] { trash_loop(); });" }) &&
              !has(wake, "trash_request_"),
          "request_pet: a worker for it alone, as \"Mua ngay\" starts one -- whether a timed bag job is ticked or "
          "not -- with no request of its own to take a button's place");
    check(in_order(trip, { "s.epoch = place.epoch;", "pet_restock_.look(s.stem, s.epoch, pet_armed_.load(), now);",
                           "s.pet_restock = pet_restock_.waits(s.epoch, now);" }),
          "the expedition worker takes an arrival it sees first, and hands the lobby's wait to its planner");
    check(in_order(planner, { "if (zone_ == Zone::Lobby && s.pet_restock && !pet_cut_ && waits_for_pet(step_)) {",
                              "if (now < pet_since_ + PetRestock::kWaitMostMs) {", "return wait(b);",
                              "pet_cut_ = true;", "if (now < backoff_until_) return wait(status_);" }) &&
              count_in(planner, "pet_cut_ = false;") == 1 && count_in(planner, "pet_since_ = 0;") == 1,
          "the planner: the lobby's steps wait, 90 s at most, then on -- the cap reset by each new stay");
    check(in_order(loop, { "if ((pass_due || charm_due || shop_due) && chat_first()) continue;",
                           "if (pet_restock_.expire(now_ms()))", "const std::uint32_t pet_owed = pet_restock_.owed(now_ms());",
                           "if (here.epoch != pet_owed || stem_here != expedition::kCoLongLobby) {",
                           "pet_restock_.over(pet_owed);",
                           "pet_due = request == 0 && !held && !trash_left_held_ && pet_restock_.due(now_ms()) == pet_owed;",
                           "if (pet_due && chat_first()) continue;", "if (pet_due && !pass_due && !charm_due) {",
                           "run_pet(pet_owed);", "continue;", "run_shop(request == 4, request == 5, false);",
                           "if (request == 0 && trash_oneshot_ && pet_owed != 0 && pet_restock_.owed(now_ms()) != 0) "
                           "continue;",
                           "if (request == 0) {" }) &&
              count_in(gui, "run_pet(") == 1,
          "the worker's turn: a button first, then a Tự chat line, a timed pass and the charms due, then the pet's "
          "round -- before a timed potion round -- for the entry the character is still in; a worker here for it "
          "stays while it is owed");
    check(in_order(pet, { "const int left = settings.buy_pet_slots - pet_bought;", "if (!settings.buy_pet || left <= 0) {",
                          "pet_restock_.over(entry);", "if (!input_.window_valid()) return later(5'000,",
                          "cleaner.set_mode(BagCleaner::Mode::ShopBuy);",
                          "const int plan = std::min(left, BagCleaner::kShopMaxBuys);",
                          "cleaner.set_shop_plan(0, 0, plan);", "take_hold(\"trash cleaning\")", "r = cleaner.run();",
                          "pet_bought += r.shop_bought_pet + r.shop_unsure_pet;",
                          "if (r.pet_ended || pet_bought >= settings.buy_pet_slots) {", "pet_restock_.over(entry);",
                          "later(BagCleaner::kBoardWaitMs, \"the check-in board is up\");",
                          "later(bag::kHpStoppedAgainMs, \"stopped for HP\");",
                          "later(PetRestock::kAgainMs," }) &&
              !has(pet, "set_shop_plan(settings.buy_hp") && !has(pet, "shop_clock") && !has(pet, "owed.hp"),
          "run_pet: the pet's food alone, what is left of its slots for the arrival; bought or the game's no: over; "
          "the board a minute, HP 15 s, trouble 30 s -- never the potions' plan or clock");
    check(has(card, "ImGui::Checkbox(\"Mua HP pet (Thịt bò) khi ra tới TổCổLong\", &simple_clean_.buy_pet)") &&
              has(card, "put(\"buy_pet\", simple_clean_.buy_pet ? \"true\" : \"false\");") &&
              has(card, "simple_clean_.buy_pet_slots = std::clamp(simple_clean_.buy_pet_slots, 0, "
                        "CleanSettings::kBuySlotsPerRunMost);") &&
              has(card, "put(\"buy_pet_slots\", std::to_string(simple_clean_.buy_pet_slots));") &&
              has(card, "std::to_string(PetRestock::kWaitMostMs / 1000)"),
          "the Tự mua bình card: the box and the slots, each written to trash.ini as it changes, the tooltip's "
          "numbers from the constants");
    // The notes: on top of DOC-TRUOC's section 5, README beside the potion
    // shop, with the numbers the code has.
    const std::string doc = read_file(std::string(GOBOT_SOURCE_DIR) + "/DOC-TRUOC.txt");
    const std::string readme = read_file(std::string(GOBOT_SOURCE_DIR) + "/README.md");
    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    const std::size_t e24 = doc.find("- Mới trong bản 2026.11.24 (mua HP pet khi ra tới TổCổLong;");
    check(sec5 != std::string::npos && e24 != std::string::npos && doc.find("- Mới trong bản", sec5) == e24 &&
              doc.find("- Mới trong bản", e24 + 1) == doc.find("- Mới trong bản 2026.11.17c (") &&
              has(doc, "100). Lưu trong configs\\trash.ini: buy_pet, buy_pet_slots.") &&
              has(doc, "giữa ngăn cách nút " + std::to_string(33) + " điểm ảnh sang phải và " + std::to_string(353) +
                           " điểm ảnh lên trên.") &&
              has(doc, "pet_no_item, pet_item_place), rồi thử lại sau " + std::to_string(PetRestock::kAgainMs / 1000) +
                           " giây nếu nhân vật còn ở") &&
              has(doc, "dòng \"Chờ mua HP pet (Thịt bò) xong rồi mới đi tiếp\", tối đa " +
                           std::to_string(PetRestock::kWaitMostMs / 1000) + " giây kể"),
          "DOC-TRUOC.txt: the 2026.11.24 entry first in section 5, over 17c's -- the keys, the tab's place from the "
          "buttons, the 30 s, the lobby's line and its 90 s");
    check(has(readme, "**Mua HP pet (Thịt bò) khi ra tới TổCổLong** ticked (`buy_pet` in") &&
              has(readme, "(the tab's middle 33 px right of the buttons and 353 above them; five tabs of") &&
              has(readme, "HP pet* -- 90 s at the most from the arrival, then go on without it; nothing is") &&
              has(readme, "since 2026.11.24 the pet's HP food on reaching TổCổLong (`buy_pet`, off as shipped and in a "
                          "file without it, and `buy_pet_slots`, 0 to 999, 1 as shipped)"),
          "README.md: the pet's food beside the potion shop, and trash.ini's keys in the table");
    {
        const ScreenRect b{ 497, 541, 119, 21 };
        const ScreenRect r = bag::shop_tab_rect(b, bag::kShopTabPet);
        check(r.cx() - b.x == 33 && r.cy() - b.y == -353 && bag::kShopTabW == 64 &&
                  CleanSettings::kBuySlotsPerRunMost == 999 && CleanSettings::kBuyPetSlotsDefault == 1,
              "the numbers the notes give are the code's");
    }
#else
    skip("no source folder\n");
#endif
}

void test_prj_map() {
    section("Expedition: a map drawn on another's floors (.prj)");
    std::string text = std::string("\x01\x02junk\\data\\back\\t52_1.bg\0", 25) + "\\data\\Tile\\t8_1.til" +
                       std::string("\0\0", 2) + "\\data\\Map\\T52_S1.map" + std::string("\0", 1);
    const Bytes prj(text.begin(), text.end());
    check(GameMapBook::prj_map_stem(prj) == "t52_s1", "t1981_s1.prj names t52_s1.map: read lowercase");
    const std::string none = "\\data\\back\\x.bg";
    check(GameMapBook::prj_map_stem(Bytes(none.begin(), none.end())).empty(), "no map named: none");
}

} // namespace

void test_expedition(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_packets();
    test_settings();
    test_route_and_spot();
    test_mining_planner();
    test_cothu_planner();
    test_colong_planner();
    test_dungeon_blocks();
    test_trip_quiet();
    test_no_stop();
    test_npc_shop();
    test_pet_restock();
    test_prj_map();
}
