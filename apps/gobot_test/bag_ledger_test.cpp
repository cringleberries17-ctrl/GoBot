// gobot_test -- the bag from the packets, checked against the hotbar and the
// shop's own counts before anything goes by it (the packets plan, item E):
// BagLedger on bags built by hand and a clock of the test's own, and where
// GoBot's window wires it in, read off its source.
//
// Nothing here goes near the game, the network, the desktop or the registry.
// 8820131 and 8810111 are the shop's two potions (CleanSettings::kBuyHpId and
// kBuyMpId); 8810121 in slot 6 is the P0-2 recording's; every other number is
// made up.

#include "gobot/bag_ledger.hpp"
#include "gobot/self_frames.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

constexpr std::uint64_t kT0 = 5'000'000;        // where the test's clock starts
constexpr std::uint64_t kMinute = 60'000;
constexpr std::uint32_t kHp = 8820131;          // Thịt bò khô
constexpr std::uint32_t kMp = 8810111;          // Rượu lá tre
constexpr std::uint32_t kCharm = 8960012;
constexpr std::uint32_t kG = 'G';               // the HP potion's key
constexpr std::uint32_t kH = 'H';               // the MP potion's key

BagSlotFrame slot_of(std::uint32_t item, std::uint16_t count) {
    BagSlotFrame s;
    s.item = item;
    s.count = count;
    return s;
}

void less_one(BagFrame& bag, std::size_t slot) { bag[slot].count = static_cast<std::uint16_t>(bag[slot].count - 1); }

/// The capture running, in `epoch`, PacketSelf's last batch of the start the
/// capture is at.
BagLedger::Link live(std::uint32_t epoch = 1, std::uint64_t start = 1) {
    BagLedger::Link l;
    l.capturing = true;
    l.epoch = epoch;
    l.self_start = start;
    l.capture_start = start;
    return l;
}

bool has_event(const std::vector<BagLedger::Event>& events, BagLedger::Event::Kind kind) {
    for (const BagLedger::Event& e : events)
        if (e.kind == kind) return true;
    return false;
}

BagLedger::KeySummary key_of(const BagLedger::Summary& s, std::uint32_t vk) {
    for (const BagLedger::KeySummary& k : s.keys)
        if (k.vk == vk) return k;
    return BagLedger::KeySummary{};
}

/// A ledger with a bag: Thịt bò khô in slots 6 and 20 (2 slots, 171), Rượu
/// lá tre in 9, 10 and 11 (3 slots, 220), a charm in 30; G and H the macro's
/// potion keys.
struct Fixture {
    BagLedger       L;
    BagFrame        bag{};
    std::uint64_t   t = kT0;
    BagLedger::Link link = live();

    Fixture() {
        bag[6] = slot_of(kHp, 71);
        bag[20] = slot_of(kHp, 100);
        bag[9] = slot_of(kMp, 50);
        bag[10] = slot_of(kMp, 80);
        bag[11] = slot_of(kMp, 90);
        bag[30] = slot_of(kCharm, 5);
        L.set_potion_keys({ kG, kH });
        L.set_shop_items(kHp, kMp);
        L.take_bag(t, link.epoch, link.self_start, bag);
    }
    /// The bag as it is now, in a frame `ms` on.
    void frame(std::uint64_t ms) {
        t += ms;
        L.take_bag(t, link.epoch, link.self_start, bag);
    }
    /// `vk` pressed, and 200 ms on a frame with `slot` one less: one potion.
    void drink(std::uint32_t vk, std::size_t slot) {
        t += 3000;
        L.press(vk, t);
        less_one(bag, slot);
        L.take_bag(t + 200, link.epoch, link.self_start, bag);
        t += 300;
        L.settle(t, link);
    }
    /// The hotbar read `count` on `vk`'s slot, well clear of any bag frame,
    /// and judged once the frames of that moment are in.
    void look(std::uint32_t vk, int count) {
        t += 2500;
        L.hotbar(vk, count, t);
        t += BagLedger::kHotbarSettleMs + BagLedger::kLateMs;
        L.settle(t, link);
    }
    /// G learned to drink from slot 6, and checked twice against the hotbar.
    void trust_g() {
        for (int i = 0; i < 3; ++i) drink(kG, 6);
        look(kG, bag[6].count);
        look(kG, bag[6].count);
    }
    /// Two shop runs whose own counts are the bag's.
    void trust_shop() {
        L.shop_done(2, 3, false, t, link);
        t += 10 * kMinute;
        L.shop_done(2, 3, false, t, link);
    }
};

// ------------------------------------------------------------ the numbers

void test_bag_numbers() {
    section("Bag (packets): totals, slots, one slot one less, item numbers");
    BagFrame b{};
    b[6] = slot_of(8810121, 71);        // the P0-2 recording's slot 6
    b[9] = slot_of(8810121, 29);
    b[2] = slot_of(kHp, 100);
    check(BagLedger::total_of(b, 8810121) == 100 && BagLedger::slots_of(b, 8810121) == 2 &&
              BagLedger::used_slots(b) == 3 && BagLedger::total_of(b, kHp) == 100 && BagLedger::slots_of(b, kHp) == 1,
          "the item's total over all its slots, its slots, and the slots used");
    check(BagLedger::total_of(b, 0) == 0 && BagLedger::slots_of(b, 0) == 0 && BagLedger::total_of(b, 1) == 0,
          "an empty slot is no item's, and an item not there has none");

    std::size_t slot = 99;
    std::uint32_t item = 0;
    BagFrame a = b;
    a[6].count = 70;
    check(BagLedger::one_fell(b, a, slot, item) && slot == 6 && item == 8810121, "slot 6 71 -> 70: one less");
    a[6].count = 69;
    check(!BagLedger::one_fell(b, a, slot, item), "71 -> 69: two less, refused");
    a = b;
    a[6].count = 70;
    a[9].count = 28;
    check(!BagLedger::one_fell(b, a, slot, item), "two slots one less each: refused");
    a = b;
    a[6].count = 70;
    a[40] = slot_of(4, 1);
    check(!BagLedger::one_fell(b, a, slot, item), "one less and something else besides: refused");
    a = b;
    a[6].count = 72;
    check(!BagLedger::one_fell(b, a, slot, item) && !BagLedger::one_fell(b, b, slot, item),
          "one more, or nothing changed: no");
    BagFrame last = b;
    last[9] = slot_of(8810121, 1);
    BagFrame gone = last;
    gone[9] = slot_of(0, 0);
    check(BagLedger::one_fell(last, gone, slot, item) && slot == 9 && item == 8810121, "the last one gone: one less");
    a = b;
    a[6].item = 7;
    a[6].count = 70;
    check(!BagLedger::one_fell(b, a, slot, item), "another item in the slot: no");

    check(BagLedger::item_of_id("spend/hp/8820131") == kHp && BagLedger::item_of_id("spend/mana/8810111") == kMp &&
              BagLedger::item_of_id("spend\\buff\\123") == 123 && BagLedger::item_of_id("8960012") == kCharm,
          "the item number a catalog id ends in");
    check(BagLedger::item_of_id("charm_3.png") == 0 && BagLedger::item_of_id("spend/buff/") == 0 &&
              BagLedger::item_of_id("") == 0 && BagLedger::item_of_id("4294967296") == 0 &&
              BagLedger::item_of_id("12345678901") == 0,
          "and none for a picture's file name, an empty tail, or a number past 32 bits");
}

// ------------------------------------------------------------ learning a key

void test_bag_learning() {
    section("Bag (packets): a potion key learned from its presses, a 2-slot fall refused");
    Fixture f;
    f.drink(kG, 6);
    f.drink(kG, 6);
    BagLedger::KeySummary g = key_of(f.L.summary(f.link), kG);
    check(!g.learned && g.votes_for == 2 && g.presses == 2 && g.answered == 2,
          "two presses each answered by slot 6 one less: 2 votes, not learned yet");
    f.drink(kG, 6);
    g = key_of(f.L.summary(f.link), kG);
    std::vector<BagLedger::Event> events = f.L.take_events();
    check(g.learned && g.slot == 6 && g.item == kHp && g.votes_for == 3 && g.votes == 3 &&
              has_event(events, BagLedger::Event::Kind::Learned),
          "the third: G drinks 8820131 from slot 6, learned 3/3, and said");

    // Answered by two slots one less each, and by one slot two less: no vote.
    f.t += 3000;
    f.L.press(kG, f.t);
    less_one(f.bag, 6);
    less_one(f.bag, 9);
    f.L.take_bag(f.t + 200, f.link.epoch, f.link.self_start, f.bag);
    f.t += 3000;
    f.L.press(kG, f.t);
    less_one(f.bag, 6);
    less_one(f.bag, 6);
    f.L.take_bag(f.t + 200, f.link.epoch, f.link.self_start, f.bag);
    f.t += 300;
    f.L.settle(f.t, f.link);
    g = key_of(f.L.summary(f.link), kG);
    check(g.refused == 2 && g.votes == 3 && g.answered == 5 && g.learned,
          "a 2-slot fall and a fall of 2 are refused: answered, no vote");

    // A key that is not a potion key, and a press given twice.
    f.L.press('1', f.t + 10);
    f.L.press(kG, f.t + 20);
    f.L.press(kG, f.t + 20);
    f.L.press('1', f.t + 10);
    g = key_of(f.L.summary(f.link), kG);
    check(g.presses == 6 && f.L.press_cursor() == f.t + 20,
          "a skill key's press is not counted, and a press given again is counted once");
    f.t += 3000;
    f.L.settle(f.t, f.link);        // that press of G goes unanswered

    // The key's last presses split between two items: kLearnPercent.
    f.drink(kG, 9);
    g = key_of(f.L.summary(f.link), kG);
    events = f.L.take_events();
    check(!g.learned && g.votes_for == 3 && g.votes == 4 && has_event(events, BagLedger::Event::Kind::Unlearned),
          "a vote for another item: 3 of 4 for one is 75%, under 80% -- no longer learned, and said");
}

/// The macro's presses are stamped as their keys go down (MacroEngine::
/// set_on_press): the game drinks then, and its 0x0068 can come before the key
/// is up again, 70 ms and more later.
void test_bag_press_times() {
    section("Bag (packets): presses stamped as their keys go down -- a quick answer, two potions close together");
    {
        // The bag frame 50 ms after the key went down, before it came up.
        Fixture f;
        for (int i = 0; i < 3; ++i) {
            f.t += 3000;
            f.L.press(kG, f.t);
            less_one(f.bag, 6);
            f.L.take_bag(f.t + 50, f.link.epoch, f.link.self_start, f.bag);
            f.t += BagLedger::kPressWindowMs + BagLedger::kLateMs;
            f.L.settle(f.t, f.link);
        }
        const BagLedger::Summary s = f.L.summary(f.link);
        const BagLedger::KeySummary g = key_of(s, kG);
        check(g.learned && g.slot == 6 && g.item == kHp && g.answered == 3 && g.unanswered == 0 && s.voids == 0,
              "a 0x0068 50 ms after the key went down: each press answered, G learned, nothing voided");
    }
    {
        // HP and MP due together: G down, its frame 60 ms on; H down 150 ms
        // after G, its frame 60 ms after that.
        Fixture f;
        for (int i = 0; i < 3; ++i) {
            f.t += 3000;
            const std::uint64_t g_at = f.t, h_at = f.t + 150;
            f.L.press(kG, g_at);
            less_one(f.bag, 6);
            f.L.take_bag(g_at + 60, f.link.epoch, f.link.self_start, f.bag);
            f.L.press(kH, h_at);
            less_one(f.bag, 9);
            f.L.take_bag(h_at + 60, f.link.epoch, f.link.self_start, f.bag);
            f.t = h_at + BagLedger::kPressWindowMs + BagLedger::kLateMs;
            f.L.settle(f.t, f.link);
        }
        const BagLedger::Summary s = f.L.summary(f.link);
        const BagLedger::KeySummary g = key_of(s, kG), h = key_of(s, kH);
        check(g.learned && g.slot == 6 && g.item == kHp && g.votes_for == g.votes && h.learned && h.slot == 9 &&
                  h.item == kMp && h.votes_for == h.votes,
              "an HP press and an MP press 150 ms apart, each answered by its own frame: G learns Thịt bò khô from "
              "slot 6, H Rượu lá tre from slot 9, and neither votes for the other's");
    }
}

// ------------------------------------------------------------ the hotbar

void test_bag_hotbar() {
    section("Bag (packets): a learned key checked against its hotbar slot; \"Low\" by the bag's total");
    Fixture f;
    for (int i = 0; i < 3; ++i) f.drink(kG, 6);
    int total = -1;
    std::uint32_t item = 0;
    check(!f.L.low_total(kG, f.link, total, item) && total == -1, "learned, not checked yet: \"Low\" is the hotbar's");

    // Judged only once the frames of its moment are in.
    f.t += 2500;
    f.L.hotbar(kG, f.bag[6].count, f.t);
    f.L.settle(f.t + BagLedger::kHotbarSettleMs + BagLedger::kLateMs - 1, f.link);
    check(f.L.summary(f.link).hotbar_agree == 0, "a look is not judged before its frames are all in");
    f.t += BagLedger::kHotbarSettleMs + BagLedger::kLateMs;
    f.L.settle(f.t, f.link);
    check(f.L.summary(f.link).hotbar_agree == 1 && !f.L.low_total(kG, f.link, total, item),
          "then it agrees: 68 on the hotbar, 68 in slot 6 -- one agreement is not enough");
    f.look(kG, f.bag[6].count);
    const std::vector<BagLedger::Event> events = f.L.take_events();
    check(f.L.low_total(kG, f.link, total, item) && total == 168 && item == kHp &&
              has_event(events, BagLedger::Event::Kind::LowTrusted),
          "two agreements: \"Low\" goes by the item's total in the bag, 68 + 100 = 168, and that is said");
    check(!f.L.low_total(kH, f.link, total, item), "H, never learned: the hotbar's");

    // A bag frame near the look: not judged, either way.
    f.t += 3000;
    less_one(f.bag, 10);
    f.frame(0);
    f.L.hotbar(kG, 1, f.t + 500);
    f.t += 500 + BagLedger::kHotbarSettleMs + BagLedger::kLateMs;
    f.L.settle(f.t, f.link);
    BagLedger::Summary s = f.L.summary(f.link);
    check(s.hotbar_agree == 2 && s.hotbar_disagree == 0, "a look 500 ms after a bag frame is not judged");

    // The slot empty on the hotbar: agrees only with none left anywhere.
    f.look(kG, 0);
    s = f.L.summary(f.link);
    check(s.hotbar_disagree == 1 && !f.L.low_total(kG, f.link, total, item),
          "the hotbar's slot empty with 168 in the bag: a disagreement, and \"Low\" is the hotbar's again");
}

void test_bag_distrust() {
    section("Bag (packets): two disagreements running distrust a key; any disagreement voids the shop");
    Fixture f;
    f.trust_g();
    f.trust_shop();
    check(f.L.summary(f.link).shop_trusted, "trusted: G for \"Low\", and the shop");
    f.L.take_events();
    f.look(kG, 30);
    int total = -1;
    std::uint32_t item = 0;
    BagLedger::Summary s = f.L.summary(f.link);
    std::vector<BagLedger::Event> events = f.L.take_events();
    check(!f.L.low_total(kG, f.link, total, item) && key_of(s, kG).learned && s.shop_agreements == 0 &&
              !s.shop_trusted && s.voids == 1 && has(s.last_void, "hotbar") &&
              has_event(events, BagLedger::Event::Kind::HotbarDisagrees) &&
              has_event(events, BagLedger::Event::Kind::ShopVoided),
          "one disagreement: \"Low\" back to the hotbar, still learned, and the shop's trust voided");
    f.look(kG, 31);
    s = f.L.summary(f.link);
    events = f.L.take_events();
    check(!key_of(s, kG).learned && key_of(s, kG).distrusts == 1 && key_of(s, kG).votes == 0 &&
              has_event(events, BagLedger::Event::Kind::Distrusted),
          "the second running: G distrusted, what it learned forgotten");
    for (int i = 0; i < 3; ++i) f.drink(kG, 20);
    s = f.L.summary(f.link);
    check(key_of(s, kG).learned && key_of(s, kG).slot == 20 && !f.L.low_total(kG, f.link, total, item),
          "learned again from three more presses -- from slot 20 now -- and checked afresh before \"Low\" is the bag's");
}

// ------------------------------------------------------------ the shop

void test_bag_shop_checks() {
    section("Bag (packets): the shop's own counts against the bag's, and every void");
    Fixture f;
    BagLedger::ShopCheck c = f.L.shop_done(2, 3, false, f.t, f.link);
    check(c.real && c.compared && c.agreed && c.hp_packets == 2 && c.mp_packets == 3 && c.agreements == 1 && !c.trusted,
          "a run that counted 2 and 3 slots, as the bag has them: one agreement");
    f.t += 10 * kMinute;
    c = f.L.shop_done(2, 3, false, f.t, f.link);
    check(c.agreements == 2 && c.trusted && has_event(f.L.take_events(), BagLedger::Event::Kind::ShopTrusted),
          "a second: trusted, and said");
    f.t += 10 * kMinute;
    c = f.L.shop_done(2, 4, false, f.t, f.link);
    check(c.compared && !c.agreed && c.agreements == 0 && !c.trusted, "a run that counted 4 where the bag has 3: void");
    c = f.L.shop_done(-1, -1, false, f.t, f.link);
    check(!c.real && !c.compared, "a run that counted nothing: nothing compared, and no real look");

    // A press of a learned key that no bag frame answered.
    f.trust_shop();
    f.t += 3000;
    f.L.press(kH, f.t);             // H: never learned
    f.t += BagLedger::kPressWindowMs + BagLedger::kLateMs;
    f.L.settle(f.t, f.link);
    check(f.L.summary(f.link).shop_agreements == 2 && key_of(f.L.summary(f.link), kH).unanswered == 1,
          "an unlearned key's press with no 0x0068: counted, no void");
    for (int i = 0; i < 3; ++i) f.drink(kG, 6);
    check(f.L.summary(f.link).shop_agreements == 2, "presses answered: no void");
    f.t += 3000;
    const std::uint64_t pressed = f.t;
    f.L.press(kG, pressed);
    f.L.settle(pressed + BagLedger::kPressWindowMs + BagLedger::kLateMs - 1, f.link);
    check(f.L.summary(f.link).shop_agreements == 2, "a learned key's press: waited for until its frame cannot come");
    f.t = pressed + BagLedger::kPressWindowMs + BagLedger::kLateMs;
    f.L.settle(f.t, f.link);
    BagLedger::Summary s = f.L.summary(f.link);
    check(s.shop_agreements == 0 && has(s.last_void, "no 0x0068") && key_of(s, kG).unanswered == 1,
          "then, with no 0x0068 within 1.5 s: void");

    // A learned key whose potion the shop does not buy -- the owner's H
    // drinks 8810121 -- pressed with no bag frame answering it, as the
    // potion check's second press inside the game's cooldown: counted only.
    {
        Fixture h;
        h.bag[25] = slot_of(8810121, 50);
        h.frame(1000);
        for (int i = 0; i < 3; ++i) h.drink(kH, 25);
        h.trust_shop();
        const int voids_before = h.L.summary(h.link).voids;
        h.t += 3000;
        h.L.press(kH, h.t);
        h.t += BagLedger::kPressWindowMs + BagLedger::kLateMs;
        h.L.settle(h.t, h.link);
        const BagLedger::Summary hs = h.L.summary(h.link);
        check(key_of(hs, kH).learned && key_of(hs, kH).item == 8810121 && key_of(hs, kH).unanswered == 1 &&
                  hs.shop_agreements == 2 && hs.voids == voids_before,
              "a learned key of a potion the shop does not buy (8810121), its press unanswered: counted, no void");
    }

    // A new capture start, and no bag frame since.
    f.trust_shop();
    const int voids = f.L.summary(f.link).voids;
    const BagLedger::Link restarted = live(2, 2);
    BagLedger::ShopAnswer a = f.L.shop_ask(f.t, restarted, 2, 3);
    check(!a.skip && a.hp_slots == -1 && f.L.summary(restarted).shop_agreements == 0 &&
              f.L.summary(restarted).voids == voids + 1 && has(f.L.summary(restarted).last_void, "capture start"),
          "a new capture start with no 0x0068 since: void, and nothing said of the bag");
    f.L.shop_ask(f.t, restarted, 2, 3);
    check(f.L.summary(restarted).voids == voids + 1, "once per start");
    f.link = restarted;
    f.frame(1000);
    a = f.L.shop_ask(f.t, f.link, 2, 3);
    check(!a.skip && a.hp_slots == 2 && a.mp_slots == 3 && has(a.why, "agreed"),
          "a bag frame of the new start: the bag is the game's again, but the shop has to agree twice more");
}

void test_bag_stale() {
    section("Bag (packets): a stale epoch, an old start, no capture -- nothing goes by the bag");
    Fixture f;
    f.trust_g();
    f.trust_shop();
    int total = -1;
    std::uint32_t item = 0;
    check(f.L.current(f.link) && f.L.low_total(kG, f.link, total, item), "current: \"Low\" by the bag");

    const BagLedger::Link gap = live(2, 1);             // something missed since the last bag frame
    BagLedger::Link not_yet = live(1, 1);
    not_yet.capture_start = 2;                          // the capture started again; PacketSelf has not seen it
    BagLedger::Link off = live(1, 1);
    off.capturing = false;
    for (const BagLedger::Link* l : std::vector<const BagLedger::Link*>{ &gap, &not_yet, &off })
        check(!f.L.current(*l) && !f.L.low_total(kG, *l, total, item) && total == -1,
              "a gap, a start not yet seen, no capture: not current, \"Low\" the hotbar's");
    const BagLedger::ShopAnswer a = f.L.shop_ask(f.t + kMinute, gap, 2, 3);
    check(!a.skip && a.hp_slots == -1 && has(a.why, "not the game's now") && f.L.summary(gap).shop_agreements == 2,
          "the shop not skipped on a stale bag -- and a gap alone voids nothing");
    // No capture, as GuiApp says with packet_positions=off whoever captures:
    // the ledger learned, trusted and with the bag's newest frame, and still
    // nothing goes by it.
    const BagLedger::ShopAnswer o = f.L.shop_ask(f.t + kMinute, off, 2, 3);
    check(!o.skip && !f.L.low_total(kG, off, total, item) && f.L.summary(off).shop_trusted,
          "no capture (packet_positions=off): no skip and no \"Low\" by the bag, though it is trusted");
    f.t += 2500;
    f.L.hotbar(kG, 1, f.t);
    f.t += 2000;
    f.L.settle(f.t, gap);
    check(f.L.summary(gap).hotbar_disagree == 0, "a hotbar look is not judged against a stale bag");
    check(!f.L.charm(kCharm, f.t - 100, gap).judged, "nor a charm");
    check(f.L.present({ kHp }, gap).empty(), "and the trash's line names nothing");

    // The next frame of the new epoch: the game's bag again.
    f.link = gap;
    f.frame(1000);
    check(f.L.current(gap) && f.L.low_total(kG, gap, total, item) && total == 168, "a frame since: current again");
}

void test_bag_schedule() {
    section("Bag (packets): a due look skipped only when trusted, every second due look and every 2 h real");
    Fixture f;
    BagLedger::ShopAnswer a = f.L.shop_ask(f.t, f.link, 2, 3);
    check(!a.skip && a.hp_slots == 2 && a.mp_slots == 3, "never with the shop not trusted");
    f.trust_shop();
    const std::uint64_t real = f.t;
    a = f.L.shop_ask(real + 60 * kMinute, f.link, 2, 3);
    check(a.skip && a.hp_slots == 2 && a.mp_slots == 3, "trusted, current, enough, a real look 60 min ago: skipped");
    f.L.shop_done(-1, -1, true, real + 60 * kMinute, f.link);
    a = f.L.shop_ask(real + 119 * kMinute, f.link, 2, 3);
    check(!a.skip && has(a.why, "skipped"), "the next due look after a skip is real");
    const std::uint64_t real2 = real + 119 * kMinute;
    f.L.shop_done(2, 3, false, real2, f.link);
    a = f.L.shop_ask(real2 + 60 * kMinute, f.link, 2, 3);
    check(a.skip, "and the one after that may be skipped again");
    a = f.L.shop_ask(real2 + BagLedger::kRealRunMs - 1, f.link, 2, 3);
    check(a.skip, "up to 2 h after the last real look");
    a = f.L.shop_ask(real2 + BagLedger::kRealRunMs, f.link, 2, 3);
    check(!a.skip && has(a.why, "2 h"), "and never with no real look in 2 h");

    // A potion pressed since the newest bag frame.
    f.t = real2 + 61 * kMinute;
    f.L.press(kH, f.t);
    a = f.L.shop_ask(f.t + 100, f.link, 2, 3);
    check(!a.skip && has(a.why, "potion press"), "a potion pressed and no 0x0068 since: not skipped");
    less_one(f.bag, 9);
    f.frame(200);
    a = f.L.shop_ask(f.t + 100, f.link, 2, 3);
    check(a.skip, "its 0x0068 come: skipped");

    // Fewer slots than the shop buys under.
    a = f.L.shop_ask(f.t + 100, f.link, 3, 3);
    check(!a.skip && a.hp_slots == 2 && has(a.why, "fewer slots"), "2 slots of Thịt bò khô, bought under 3: real");
    a = f.L.shop_ask(f.t + 100, f.link, 0, 3);
    check(a.skip, "a potion the shop leaves alone (under 0) does not stop a skip");
    a = f.L.shop_ask(f.t + 100, f.link, 2, 4);
    check(!a.skip, "nor, under 4 with 3 slots of Rượu lá tre, is it skipped");
}

// ------------------------------------------------------------ charms, trash

void test_bag_charm_and_trash() {
    section("Bag (packets): a charm's stack before and after, and the trash list's ids in the bag");
    Fixture f;
    const std::uint64_t claimed = f.t + 100;
    BagLedger::CharmSeen s = f.L.charm(kCharm, claimed, f.link);
    check(s.judged && s.before == 5 && s.after == 5 && s.frames_since == 0, "no frame since the claim: 5, unchanged");
    less_one(f.bag, 30);
    f.frame(500);
    s = f.L.charm(kCharm, claimed, f.link);
    check(s.judged && s.before == 5 && s.after == 4 && s.frames_since == 1, "a frame since: 5 -> 4, one less");
    check(!f.L.charm(kCharm, kT0 - 1, f.link).judged, "no frame from before the claim: not judged");
    check(!f.L.charm(0, claimed, f.link).judged, "no item number: not judged");

    const std::vector<BagLedger::Held> held = f.L.present({ kHp, 1103063, 0, kHp, kCharm }, f.link);
    check(held.size() == 2 && held[0].item == kHp && held[0].slots == 2 && held[0].total == 171 &&
              held[1].item == kCharm && held[1].total == 4,
          "the listed ids the bag holds, each once, with its slots and total; one it does not hold is left out");

    const BagLedger::Summary sum = f.L.summary(f.link);
    check(sum.known && sum.current && sum.used_slots == 6 && sum.hp_slots == 2 && sum.hp_total == 171 &&
              sum.mp_slots == 3 && sum.mp_total == 220 && sum.keys.size() == 2 && sum.keys[0].vk == kG &&
              sum.keys[1].vk == kH && sum.frames == 2,
          "the minute line's numbers: slots used, each potion's slots and total, every potion key");
}

void test_bag_slot_at() {
    section("Bag (packets): one slot of the bag, for a charm's -- only while the bag is the game's now");
    {
        BagLedger L;
        BagSlotFrame s = slot_of(1, 1);
        check(!L.slot_at(1, live(), s) && s.item == 0 && s.count == 0, "no bag frame yet: no slot, and nothing in it");
    }
    Fixture f;
    BagSlotFrame s;
    check(f.L.slot_at(30, f.link, s) && s.item == kCharm && s.count == 5, "slot 30: the charm, 5 of it");
    check(f.L.slot_at(6, f.link, s) && s.item == kHp && s.count == 71 && f.L.slot_at(0, f.link, s) && s.item == 0,
          "slot 6: Thịt bò khô, 71; slot 0: empty");
    check(!f.L.slot_at(kBagSlots, f.link, s) && !f.L.slot_at(1000, f.link, s) && s.item == 0,
          "past the bag's 72 slots: none");
    less_one(f.bag, 30);
    f.frame(500);
    check(f.L.slot_at(30, f.link, s) && s.item == kCharm && s.count == 4, "the newest frame's: one less, 4");
    BagLedger::Link off = f.link;
    off.capturing = false;
    BagLedger::Link missed = f.link;
    missed.capture_start = f.link.capture_start + 1;        // a start PacketSelf has not seen yet
    BagLedger::Link gap = f.link;
    gap.epoch = f.link.epoch + 1;                            // something missed since its last frame
    for (const BagLedger::Link* l : std::vector<const BagLedger::Link*>{ &off, &missed, &gap })
        check(!f.L.slot_at(30, *l, s) && s.item == 0 && s.count == 0,
              "the capture not running, a start missed, a gap: no slot -- the bag may not be the game's now");
}

// ------------------------------------------------------------ the wiring

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string body(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}

int count_in(const std::string& text, const std::string& part) {
    int n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}
#endif

void test_bag_wiring() {
    section("Bag (packets) wired in: the shop's skip, \"Low\" and \"Out\", the charms (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    if (gui.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const std::string potions = source("/apps/gobot_gui/simple_potions.cpp");
    const std::string part = source("/apps/gobot_gui/packets_bag.cpp");

    // The shop: bag_says_enough( once, under !forced && !dry, after the HP
    // check and before anything of the run is built.
    const std::size_t from = gui.find("const auto run_shop = [&](bool forced, bool dry) {");
    const std::size_t to = gui.find("while (!trash_stop_) {", from == npos ? 0 : from);
    const std::string shop = from == npos || to == npos ? std::string() : gui.substr(from, to - from);
    const std::size_t hp_check = shop.find("return not_now(\"Máu đang thấp, mua bình sau.\", 15'000);");
    const std::size_t guard = shop.find("if (!forced && !dry) {");
    const std::size_t asked = shop.find("bag_says_enough(");
    const std::size_t built = shop.find("TrashList list;");
    check(!shop.empty() && count_in(gui, "bag_says_enough(") == 1 && hp_check != npos && guard != npos &&
              asked != npos && built != npos && hp_check < guard && guard < asked && asked - guard < 100 &&
              asked < built,
          "gui_app.cpp: run_shop asks bag_says_enough( only under !forced && !dry, after its HP check, before "
          "anything is built");
    const std::size_t skipped = shop.find("bag_note_shop(-1, -1, true, now_ms());");
    const std::size_t ran = shop.find("cleaner.run()");
    const std::size_t noted = shop.find("bag_note_shop(r.shop_hp_before, r.shop_mp_before, false, held_ms);");
    check(skipped != npos && asked < skipped && skipped < built && ran != npos && noted != npos && ran < noted &&
              has(shop.substr(asked, skipped - asked), "shop_due_ms = minutes(settings.buy_every_minutes);"),
          "a skip sets the next due look and is told to the bag; a real run's own counts are set beside the bag as "
          "it was when the hold was taken");

    // Potions: "Out" always the hotbar's, "Low" by the bag only for a key it
    // has been checked for, with the same alert; no input at all.
    const std::string tick = body(potions, "void GuiApp::potions_tick() {");
    const std::size_t out_case = tick.find("case Kind::Out:");
    const std::size_t out_alert = tick.find("alert(\"potion_out\", alert_text::potion_out(hp, key));");
    const std::size_t next_case = tick.find("case Kind::Wild:");
    check(out_case != npos && out_alert != npos && next_case != npos && out_case < out_alert &&
              out_alert < next_case && count_in(potions, "\"potion_out\"") == 1 &&
              !has(tick.substr(out_case, next_case - out_case), "bag"),
          "simple_potions.cpp: potions_tick raises potion_out from the hotbar's Kind::Out, the bag nowhere near it");
    const std::size_t low_case = tick.find("case Kind::Low:");
    const std::size_t low_by_bag = tick.find("if (by_bag) {", low_case == npos ? 0 : low_case);
    check(has(tick, "bag_check_tick(now_ms());") && has(tick, "const bool by_bag = bag_low_count(vk, bag_total, bag_item);") &&
              low_case != npos && low_by_bag != npos && low_by_bag < out_case &&
              has(tick, "bag_note_hotbar(vk, now.count, looked);") &&
              has(tick, "alert(\"potion_low\", alert_text::potion_low_bag(hp, key, bag_total));") &&
              count_in(potions, "\"potion_low\"") == 2,
          "and \"Low\" goes by the bag for a key it was checked for -- the slot's own Low not said -- with the same "
          "alert");
    check(!potions.empty() && !part.empty() && !has(potions, "input_.") && !has(potions, "take_hold") &&
              !has(part, "input_.") && !has(part, "take_hold") && !has(part, "key_press") &&
              !has(part, "click_at") && !has(part, "click_on_game") && !has(part, "game_mouse_mutex_"),
          "simple_potions.cpp and packets_bag.cpp call no InputManager function, take no hold and click nothing");
    check(has(body(part, "bool GuiApp::bag_low_count("), "packets_settings_.bag == PacketUse::Off") &&
              has(body(part, "bool GuiApp::bag_says_enough("), "packets_settings_.bag == PacketUse::Off"),
          "packets_bag.cpp: [Packets] bag=off keeps both uses -- \"Low\" and the shop's skip -- on the screen");
    {
        // A charm's slot: Nothing with bag=off, and Other -- an answer that
        // stops a right-click -- only after the count is the screen's; before
        // that, another count is CountDiffers, which stops one only for a
        // stack of one (charm_sweep).
        const std::string slot = body(part, "BagCleaner::PacketSays GuiApp::bag_charm_slot(");
        const std::size_t off = slot.find("packets_settings_.bag == PacketUse::Off");
        const std::size_t counted = slot.find("if (count != static_cast<int>(s.count)) {");
        const std::size_t other = slot.find("Says::Other");
        const std::size_t differs = slot.find("return Says::CountDiffers;");
        const std::size_t held = differs == npos ? npos : slot.rfind("said = held;", differs);
        check(off != npos && counted != npos && other != npos && off < counted && counted < other &&
                  count_in(slot, "Says::Agrees") == 1 && count_in(slot, "Says::Other") == 1 &&
                  has(slot, "part.ledger.slot_at("),
              "packets_bag.cpp: bag_charm_slot says nothing with bag=off, and names another item only for a slot "
              "whose count is the screen's (B9)");
        check(differs != npos && held != npos && counted < held && differs - held < 40 && differs < other &&
                  count_in(slot, "Says::CountDiffers") == 1 && count_in(slot, "return Says::Nothing;") == 3,
              "and a slot the list has with another count, or empty, is CountDiffers with what it holds -- said "
              "before Agrees or Other, never Nothing");
        {
            // CountDiffers stops a right-click for a stack of one alone; for a
            // count printed it is not compared, as before.
            const std::string cpp = source("/src/bag.cpp");
            const std::string sweep = body(cpp, "void BagCleaner::charm_sweep(");
            check(has(sweep, "const bool one_differs = count == 1 && says == PacketSays::CountDiffers;") &&
                      has(sweep, "if (says == PacketSays::Other || one_differs) {") &&
                      has(sweep, ": not the same slot, not compared\""),
                  "bag.cpp: charm_sweep leaves a slot alone on Other, and on CountDiffers only where the screen "
                  "reads a stack of one; another count over a number printed is logged, not compared");
        }
        const std::size_t wired = gui.find("cleaner.set_charm_packets(");
        const std::size_t run = gui.find("r = cleaner.run();", wired == npos ? 0 : wired);
        check(wired != npos && run != npos && count_in(gui, "bag_charm_slot(") == 1,
              "gui_app.cpp: the charm job hands the cleaner the packets' slot before it runs, and nothing else asks it");
    }
    check(has(body(part, "BagLedger::Link GuiApp::BagPart::link_of(const GuiApp& app) {"),
              "l.capturing = app.capture_.running() && !app.packets_off_.load() && app.capture_on_window();"),
          "and so does packet_positions=off, whoever captures: the bag is then no capture's -- nor another "
          "client's, from a capture not GoBot's own of the game window");

    // The charms: the bag's words beside the clock, which goes by the screen.
    const std::size_t clock_set = gui.find("unsaved = !charm_clock.set_next(t.id, at) || unsaved;");
    const std::size_t said = gui.find("const std::string by_bag = bag_charm_text(t.id, held_ms);");
    const std::size_t loop_end = gui.find("// Claimed and never right-clicked", said == npos ? 0 : said);
    check(clock_set != npos && said != npos && loop_end != npos && clock_set < said &&
              !has(gui.substr(said, loop_end - said), "set_next(") && has(gui, "(theo gói tin: đã dùng)"),
          "gui_app.cpp: a charm's line says what the bag shows after its clock is set, and changes nothing of it");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_bag_ledger(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_bag_numbers();
    test_bag_learning();
    test_bag_press_times();
    test_bag_hotbar();
    test_bag_distrust();
    test_bag_shop_checks();
    test_bag_stale();
    test_bag_schedule();
    test_bag_charm_and_trash();
    test_bag_slot_at();
    test_bag_wiring();
}
