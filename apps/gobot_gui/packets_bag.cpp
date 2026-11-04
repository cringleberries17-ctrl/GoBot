// The bag from the packets (the packets plan, item E): the potion keys'
// presses matched to the bag frames that answer them, the hotbar's counts and
// the shop's own counts set beside the bag's (gobot/bag_ledger.hpp), and what
// goes by the bag once those agree -- "Low" on a potion's total in the bag
// (potions_tick) and a due look of the shop skipped (run_shop). The trash's
// lines only say what the bag shows. The charms' can also stop a right-click
// on a slot the bag names another item in, or another count in for a stack of
// one (bag_charm_slot) -- never cause one.
//
// "Out" stays the hotbar's, buying stays the screen's, and with [Packets]
// bag=off nothing goes by the bag at all: the checks and their lines go on.
// Nothing here presses, clicks or looks at the screen. PacketSelf and the
// capture are read as copies, each under its own lock.

#include "gui_app.hpp"
#include "gobot/bag.hpp"
#include "gobot/bag_ledger.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace gobot {
namespace {

/// How often GoBot's thread catches the ledger up with PacketSelf.
constexpr std::uint64_t kSyncEveryMs = 250;

std::string key_text(std::uint32_t vk) { return "key " + InputManager::key_name(vk); }

/// What the ledger saw happen, as the log says it.
std::string event_text(const BagLedger::Event& e) {
    using Kind = BagLedger::Event::Kind;
    const std::string key = key_text(e.vk);
    const unsigned item = static_cast<unsigned>(e.item);
    char buf[320];
    switch (e.kind) {
        case Kind::Learned:
            std::snprintf(buf, sizeof buf, "Bag (packets): %s drinks %u, from slot %zu now -- learned, %d of its last %d "
                          "presses", key.c_str(), item, e.slot, e.a, e.b);
            break;
        case Kind::Unlearned:
            std::snprintf(buf, sizeof buf, "Bag (packets): %s no longer learned -- %d of its last %d presses for one "
                          "item; \"Low\" is the hotbar's", key.c_str(), e.a, e.b);
            break;
        case Kind::LowTrusted:
            std::snprintf(buf, sizeof buf, "Bag (packets): %s's \"Low\" goes by the bag's total of %u now -- %d hotbar "
                          "checks agree with slot %zu", key.c_str(), item, e.a, e.slot);
            break;
        case Kind::HotbarDisagrees:
            std::snprintf(buf, sizeof buf, "Bag (packets): %s's hotbar slot read %d, the bag %d (slot %zu, %u) -- "
                          "disagree", key.c_str(), e.a, e.b, e.slot, item);
            break;
        case Kind::Distrusted:
            std::snprintf(buf, sizeof buf, "Bag (packets): %s distrusted -- %d disagreements running; learned again "
                          "from its presses, and \"Low\" is the hotbar's meanwhile", key.c_str(), e.a);
            break;
        case Kind::ShopTrusted:
            std::snprintf(buf, sizeof buf, "Bag (packets): the shop trusted -- %d runs' own counts agree with the bag; "
                          "a due look may be skipped", e.a);
            break;
        case Kind::ShopVoided:
            std::snprintf(buf, sizeof buf, "Bag (packets): the shop's trust voided (%d agreeing runs lost) -- %s", e.a,
                          e.why);
            break;
        default:
            buf[0] = '\0';
            break;
    }
    return buf;
}

/// "8820131 4 slots / 380".
std::string held_text(std::uint32_t item, int slots, int total) {
    return std::to_string(item) + " " + std::to_string(slots) + (slots == 1 ? " slot / " : " slots / ") +
           std::to_string(total);
}

} // namespace

/// What the bag's part keeps. The ledger has its own lock; sync_mutex lets
/// one catch-up with PacketSelf run at a time, from GoBot's thread or the
/// trash job's.
struct GuiApp::BagPart {
    BagLedger     ledger;
    std::mutex    sync_mutex;
    std::uint64_t next_sync_ms = 0;     ///< GoBot's thread only
    std::string   said;                 ///< the last minute line; GoBot's thread only
    std::string   trash_said;           ///< the last trash line; GoBot's thread only

    /// bag_part_, made the first time anything asks for it -- GoBot's thread
    /// or the trash job's, whichever comes first -- under a lock of its own.
    static BagPart& of(const GuiApp& app);
    /// How the capture stands now, for the ledger.
    static BagLedger::Link link_of(const GuiApp& app);
    /// The ledger caught up with PacketSelf -- its bag frames and the macro's
    /// presses -- and what is due judged by `now`.
    void sync(const GuiApp& app, std::uint64_t now);
};

void GuiApp::BagPartFree::operator()(BagPart* p) const { delete p; }

GuiApp::BagPart& GuiApp::BagPart::of(const GuiApp& app) {
    static std::mutex made;
    std::lock_guard<std::mutex> lock(made);
    std::unique_ptr<BagPart, BagPartFree>& part = const_cast<GuiApp&>(app).bag_part_;
    if (!part) {
        part.reset(new BagPart);
        // The shop's potions by the numbers their catalog ids end in: what
        // the bag lists them by, compared and never assumed.
        part->ledger.set_shop_items(BagLedger::item_of_id(CleanSettings::kBuyHpId),
                                    BagLedger::item_of_id(CleanSettings::kBuyMpId));
    }
    return *part;
}

BagLedger::Link GuiApp::BagPart::link_of(const GuiApp& app) {
    BagLedger::Link l;
    // packet_positions=off: nothing from the packets, whoever captures. And
    // only GoBot's own capture of the game window's process, as for HP: one
    // of the Protocol tab's or Autopilot's takes every client's bag.
    l.capturing = app.capture_.running() && !app.packets_off_.load() && app.capture_on_window();
    l.epoch = app.packet_self_.epoch();
    l.self_start = app.packet_self_.start_count();
    l.capture_start = app.capture_.start_count();
    return l;
}

void GuiApp::BagPart::sync(const GuiApp& app, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(sync_mutex);
    const PacketSelf::Bag latest = app.packet_self_.bag();
    if (latest.known && !ledger.has_bag(latest.at_ms, latest.slots)) {
        // Every frame up to the newest, those that came between two
        // catch-ups too, in the epoch the newest came in.
        const std::uint64_t start = app.packet_self_.start_count();
        for (const PacketSelf::BagSeen& f : app.packet_self_.bag_frames())
            if (f.at_ms <= latest.at_ms) ledger.take_bag(f.at_ms, latest.epoch, start, f.slots);
    }
    for (const PacketSelf::Press& p : app.packet_self_.presses_since(ledger.press_cursor()))
        ledger.press(p.vk, p.at_ms);
    ledger.settle(now, link_of(app));
}

void GuiApp::bag_check_tick(std::uint64_t now) {
    BagPart& part = BagPart::of(*this);
    if (now < part.next_sync_ms) return;
    part.next_sync_ms = now + kSyncEveryMs;
    // The keys the macro drinks with -- HP's, MP's and the pet's potions,
    // each once: only their presses are matched to the bag.
    std::vector<std::uint32_t> keys;
    for (const MacroKey& k : macro_config_.keys) {
        const MacroKeyKind kind = macro_key_kind(k);
        if (!k.enabled || k.keycode == 0 ||
            (kind != MacroKeyKind::HpPotion && kind != MacroKeyKind::MpPotion && kind != MacroKeyKind::PetPotion))
            continue;
        if (std::find(keys.begin(), keys.end(), k.keycode) == keys.end()) keys.push_back(k.keycode);
    }
    part.ledger.set_potion_keys(keys);
    part.sync(*this, now);
    // What changed, said here -- the trash job's catch-ups too.
    for (const BagLedger::Event& e : part.ledger.take_events()) {
        const std::string line = event_text(e);
        if (!line.empty()) GB_INFO("%s", line.c_str());
    }
}

void GuiApp::bag_note_hotbar(std::uint32_t vk, int count, std::uint64_t at) {
    BagPart::of(*this).ledger.hotbar(vk, count, at);
}

bool GuiApp::bag_low_count(std::uint32_t vk, int& total, std::uint32_t& item) const {
    total = -1;
    item = 0;
    if (packets_settings_.bag == PacketUse::Off) return false;
    return BagPart::of(*this).ledger.low_total(vk, BagPart::link_of(*this), total, item);
}

bool GuiApp::bag_says_enough(int hp_below, int mp_below, int& hp_slots, int& mp_slots) const {
    BagPart& part = BagPart::of(*this);
    const std::uint64_t now = now_ms();
    part.sync(*this, now);
    const BagLedger::ShopAnswer a = part.ledger.shop_ask(now, BagPart::link_of(*this), hp_below, mp_below);
    hp_slots = a.hp_slots;
    mp_slots = a.mp_slots;
    if (!a.skip) {
        GB_INFO("Shop: a real look -- the packets' bag does not skip it: %s", a.why);
        return false;
    }
    if (packets_settings_.bag == PacketUse::Off) {
        GB_INFO("Shop: a real look -- the packets' bag would skip it, but [Packets] bag=off");
        return false;
    }
    return true;
}

void GuiApp::bag_note_shop(int hp_slots_screen, int mp_slots_screen, bool skipped, std::uint64_t at) {
    BagPart& part = BagPart::of(*this);
    part.sync(*this, now_ms());
    const BagLedger::ShopCheck c =
        part.ledger.shop_done(hp_slots_screen, mp_slots_screen, skipped, at, BagPart::link_of(*this));
    if (skipped) return;            // run_shop has said so
    const std::string hp_item = std::to_string(BagLedger::item_of_id(CleanSettings::kBuyHpId));
    const std::string mp_item = std::to_string(BagLedger::item_of_id(CleanSettings::kBuyMpId));
    const int need = BagLedger::kShopAgreements;
    if (!c.real) {
        GB_INFO("Bag check: the shop run counted no slots -- nothing compared");
        return;
    }
    if (!c.compared) {
        GB_INFO("Bag check: the shop counted %d slots of Thịt bò khô (%s) and %d of Rượu lá tre (%s) -- not compared: %s",
                hp_slots_screen, hp_item.c_str(), mp_slots_screen, mp_item.c_str(), c.why);
        return;
    }
    if (c.agreed)
        GB_INFO("Bag check: the shop counted %d slots of Thịt bò khô (%s) and %d of Rượu lá tre (%s), the packets' bag "
                "%d and %d -- agree (%d of %d)%s",
                hp_slots_screen, hp_item.c_str(), mp_slots_screen, mp_item.c_str(), c.hp_packets, c.mp_packets,
                std::min(c.agreements, need), need, c.trusted ? ": trusted, a due look may be skipped" : "");
    else
        GB_INFO("Bag check: the shop counted %d slots of Thịt bò khô (%s) and %d of Rượu lá tre (%s), the packets' bag "
                "%d and %d -- disagree (0 of %d): no due look is skipped until %d runs agree again",
                hp_slots_screen, hp_item.c_str(), mp_slots_screen, mp_item.c_str(), c.hp_packets, c.mp_packets, need,
                need);
}

std::string GuiApp::bag_charm_text(const std::string& clock_id, std::uint64_t claimed_at) const {
    // A picture cut from the bag has no item number to look for.
    const std::uint32_t item = BagLedger::item_of_id(clock_id);
    if (item == 0) return std::string();
    BagPart& part = BagPart::of(*this);
    part.sync(*this, now_ms());
    const BagLedger::CharmSeen s = part.ledger.charm(item, claimed_at, BagPart::link_of(*this));
    const std::string id = std::to_string(item);
    if (!s.judged) return "(packets: " + std::string(s.why) + ")";
    if (s.after == s.before - 1)
        return "(packets: " + id + " " + std::to_string(s.before) + " -> " + std::to_string(s.after) + ", one less)";
    if (s.after == s.before)
        return "(packets: " + id + " " + std::to_string(s.before) +
               (s.frames_since == 0 ? ", no 0x0068 since)" : ", unchanged)");
    return "(packets: " + id + " " + std::to_string(s.before) + " -> " + std::to_string(s.after) + ")";
}

BagCleaner::PacketSays GuiApp::bag_charm_slot(const std::string& colour_id, int slot, int count,
                                              std::string& said) const {
    using Says = BagCleaner::PacketSays;
    said.clear();
    // [Packets] bag=off: the screen alone decides, as for "Low" and the shop.
    if (packets_settings_.bag == PacketUse::Off) {
        said = "[Packets] bag=off: the screen decides";
        return Says::Nothing;
    }
    // A picture cut from the bag with no colour told has no item number.
    const std::uint32_t wanted = BagLedger::item_of_id(colour_id);
    if (wanted == 0) {
        said = "no item number to look for";
        return Says::Nothing;
    }
    BagPart& part = BagPart::of(*this);
    part.sync(*this, now_ms());
    BagSlotFrame s;
    if (slot < 0 || !part.ledger.slot_at(static_cast<std::size_t>(slot), BagPart::link_of(*this), s)) {
        said = "the bag is not the game's now, or no frame yet";
        return Says::Nothing;
    }
    const std::string held = s.item == 0 || s.count == 0 ? std::string("empty")
                                                         : std::to_string(s.item) + " x" + std::to_string(s.count);
    // Another count than the screen's, or none: for a count printed, most
    // likely not the slot the screen shows -- a page scrolled, a bag sorted --
    // and the cleaner says nothing of it either way; for a stack of one, told
    // on the screen by its bare icon, it stops the right-click.
    if (count != static_cast<int>(s.count)) {
        said = held;
        return Says::CountDiffers;
    }
    said = held;
    return s.item == wanted ? Says::Agrees : Says::Other;
}

void GuiApp::note_bag_minute(std::uint64_t now) {
    (void)now;
    BagPart& part = BagPart::of(*this);
    const BagLedger::Link link = BagPart::link_of(*this);
    const BagLedger::Summary s = part.ledger.summary(link);

    // Built from counts alone: a minute with nothing new says nothing.
    std::ostringstream line;
    line << "Bag (packets): ";
    if (!s.known) {
        line << "no bag frame yet (0x0068 comes only when the bag changes)";
    } else {
        line << s.used_slots << " of " << kBagSlots << " slots used";
        if (!s.current) line << " (not the game's bag now: something was missed since its last frame)";
        line << "; " << held_text(s.hp_item, s.hp_slots, s.hp_total) << ", "
             << held_text(s.mp_item, s.mp_slots, s.mp_total);
    }
    std::string low;
    for (const BagLedger::KeySummary& k : s.keys) {
        line << "; " << key_text(k.vk);
        if (k.learned)
            line << " -> slot " << k.slot << " (" << k.item << ") learned " << k.votes_for << "/" << k.votes;
        else
            line << " not learned (" << k.votes_for << " of " << BagLedger::kLearnVotes << " votes)";
        line << ", presses " << k.presses << " (answered " << k.answered << ", not " << k.unanswered
             << ", other changes " << k.refused << ")";
        if (k.distrusts > 0) line << ", distrusted " << k.distrusts;
        if (k.low_trusted) low += (low.empty() ? "" : ", ") + InputManager::key_name(k.vk);
    }
    line << "; checks: hotbar " << s.hotbar_agree << "/" << s.hotbar_disagree << ", shop " << s.shop_agree << "/"
         << s.shop_disagree << "; voids " << s.voids;
    if (s.voids > 0) line << " (last: " << s.last_void << ")";
    line << "; \"Low\" by the bag: " << (low.empty() ? std::string("none") : low) << "; shop: "
         << (s.shop_trusted ? "trusted" : "not trusted") << " ("
         << std::min(s.shop_agreements, BagLedger::kShopAgreements) << " of " << BagLedger::kShopAgreements
         << "), skipped " << s.skips << ", real " << s.reals;
    if (packets_settings_.bag == PacketUse::Off) line << "; [Packets] bag=off: the screen decides";
    if (line.str() != part.said) {
        part.said = line.str();
        GB_INFO("%s", part.said.c_str());
    }

    // The trash list's catalog ids the bag holds, while the trash job runs:
    // for the log only -- the trash job goes by the screen.
    if (!trash_running_.load() || !s.current) return;
    CleanSettings listed;
    if (!listed.load(exe_relative("configs/trash.ini"))) return;
    std::vector<std::uint32_t> items;
    for (const std::vector<std::string>* ids : { &listed.delete_ids, &listed.drop_ids, &listed.use_ids })
        for (const std::string& id : *ids) items.push_back(BagLedger::item_of_id(id));
    std::ostringstream trash;
    trash << "Trash (packets): listed ids present in the packets' bag: ";
    const std::vector<BagLedger::Held> held = part.ledger.present(items, link);
    if (held.empty()) trash << "none";
    for (std::size_t i = 0; i < held.size(); ++i)
        trash << (i ? ", " : "") << held[i].item << " x" << held[i].total << " (" << held[i].slots
              << (held[i].slots == 1 ? " slot)" : " slots)");
    if (trash.str() != part.trash_said) {
        part.trash_said = trash.str();
        GB_INFO("%s", part.trash_said.c_str());
    }
}

} // namespace gobot
