// The bag from the packets, checked against the hotbar and the shop's own
// counts before anything goes by it (the packets plan, item E): see
// gobot/bag_ledger.hpp. No clock, no frame read and no line written here:
// GoBot's window feeds it (apps/gobot_gui/packets_bag.cpp) and says what it
// finds.

#include "gobot/bag_ledger.hpp"

#include <algorithm>
#include <utility>

namespace gobot {
namespace {

constexpr std::size_t kEventsMost = 64;     // told by GoBot's thread four times a second

} // namespace

// ---------------------------------------------------------------- the numbers

int BagLedger::total_of(const BagFrame& bag, std::uint32_t item) {
    if (item == 0) return 0;
    int n = 0;
    for (const BagSlotFrame& s : bag)
        if (s.item == item) n += s.count;
    return n;
}

int BagLedger::slots_of(const BagFrame& bag, std::uint32_t item) {
    if (item == 0) return 0;
    int n = 0;
    for (const BagSlotFrame& s : bag)
        if (s.item == item) ++n;
    return n;
}

int BagLedger::used_slots(const BagFrame& bag) {
    int n = 0;
    for (const BagSlotFrame& s : bag)
        if (s.item != 0) ++n;
    return n;
}

bool BagLedger::one_fell(const BagFrame& before, const BagFrame& after, std::size_t& slot, std::uint32_t& item) {
    int changed = 0;
    std::size_t at = 0;
    for (std::size_t j = 0; j < before.size(); ++j) {
        if (before[j].item == after[j].item && before[j].count == after[j].count) continue;
        if (++changed > 1) return false;
        at = j;
    }
    if (changed != 1) return false;
    const BagSlotFrame& b = before[at];
    const BagSlotFrame& a = after[at];
    if (b.item == 0) return false;
    const bool one_fewer = a.item == b.item && static_cast<int>(a.count) + 1 == static_cast<int>(b.count);
    const bool last_gone = a.item == 0 && b.count == 1;
    if (!one_fewer && !last_gone) return false;
    slot = at;
    item = b.item;
    return true;
}

bool BagLedger::same_bag(const BagFrame& a, const BagFrame& b) {
    for (std::size_t j = 0; j < a.size(); ++j)
        if (a[j].item != b[j].item || a[j].count != b[j].count) return false;
    return true;
}

std::uint32_t BagLedger::item_of_id(const std::string& catalog_id) {
    const std::size_t slash = catalog_id.find_last_of("/\\");
    const std::string tail = slash == std::string::npos ? catalog_id : catalog_id.substr(slash + 1);
    if (tail.empty() || tail.size() > 10) return 0;
    std::uint64_t n = 0;
    for (const char c : tail) {
        if (c < '0' || c > '9') return 0;
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return n > 0xFFFFFFFFull ? 0 : static_cast<std::uint32_t>(n);
}

// ---------------------------------------------------------------- fed

void BagLedger::set_potion_keys(const std::vector<std::uint32_t>& vks) {
    std::lock_guard<std::mutex> lock(mutex_);
    potion_keys_ = vks;
}

void BagLedger::set_shop_items(std::uint32_t hp_item, std::uint32_t mp_item) {
    std::lock_guard<std::mutex> lock(mutex_);
    hp_item_ = hp_item;
    mp_item_ = mp_item;
}

bool BagLedger::is_potion_key(std::uint32_t vk) const {
    return vk != 0 && std::find(potion_keys_.begin(), potion_keys_.end(), vk) != potion_keys_.end();
}

void BagLedger::take_bag(std::uint64_t at, std::uint32_t epoch, std::uint64_t start, const BagFrame& slots) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!seen_.empty() && at < seen_.back().at) return;
    for (const Seen& f : seen_)
        if (f.at == at && same_bag(f.slots, slots)) return;
    Seen f;
    f.at = at;
    f.epoch = epoch;
    f.start = start;
    f.slots = slots;
    seen_.push_back(f);
    while (seen_.size() > kFramesKept) seen_.pop_front();
    ++frames_;
}

bool BagLedger::has_bag(std::uint64_t at, const BagFrame& slots) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Seen& f : seen_)
        if (f.at == at && same_bag(f.slots, slots)) return true;
    return false;
}

void BagLedger::press(std::uint32_t vk, std::uint64_t at) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Given again from PacketSelf's ring at every catch-up: from the newest
    // one on, and that one only once per key.
    if (at < press_cursor_) return;
    if (at == press_cursor_) {
        if (std::find(cursor_keys_.begin(), cursor_keys_.end(), vk) != cursor_keys_.end()) return;
    } else {
        press_cursor_ = at;
        cursor_keys_.clear();
    }
    cursor_keys_.push_back(vk);
    if (!is_potion_key(vk)) return;
    ++keys_[vk].presses;
    last_potion_ms_ = std::max(last_potion_ms_, at);
    presses_.push_back(Press{ vk, at });
    while (presses_.size() > kPendingMost) presses_.pop_front();
}

void BagLedger::hotbar(std::uint32_t vk, int count, std::uint64_t at) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vk == 0 || count < 0) return;
    looks_.push_back(Look{ vk, count, at });
    while (looks_.size() > kPendingMost) looks_.pop_front();
}

// ---------------------------------------------------------------- judged

void BagLedger::settle(std::uint64_t now, const Link& link) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool cur = current_locked(link);

    // Each press, oldest first, answered by the first bag frame after it
    // within kPressWindowMs that no earlier press took.
    for (auto it = presses_.begin(); it != presses_.end();) {
        const Press p = *it;
        Seen* answer = nullptr;
        for (Seen& f : seen_) {
            if (f.at <= p.at) continue;
            if (f.at > p.at + kPressWindowMs) break;
            if (f.claimed) continue;
            answer = &f;
            break;
        }
        Key& k = keys_[p.vk];
        if (answer != nullptr) {
            answer->claimed = true;
            ++k.answered;
            vote(p.vk, k, *answer);
            it = presses_.erase(it);
            continue;
        }
        if (now < p.at + kPressWindowMs + kLateMs) {
            ++it;       // its frame may still come
            continue;
        }
        // Nothing answered it. A press of a key learned to drink one of the
        // shop's two potions, with the bag the game's: the bag may not be
        // what the shop would find. Any other key's is only counted: nothing
        // the shop counts changes with it -- and the game passes over a press
        // inside its potion's cooldown, as the potion check's second press.
        ++k.unanswered;
        if (k.learned && cur && (k.item == hp_item_ || k.item == mp_item_))
            void_shop("a press of a learned key of the shop's potions that no 0x0068 answered within 1.5 s");
        it = presses_.erase(it);
    }

    // The hotbar's looks, once the bag frames of their moment are all in.
    for (auto it = looks_.begin(); it != looks_.end();) {
        if (now < it->at + kHotbarSettleMs + kLateMs) {
            ++it;
            continue;
        }
        const Look h = *it;
        it = looks_.erase(it);
        if (cur) judge_look(h, link);
    }
}

void BagLedger::vote(std::uint32_t vk, Key& k, const Seen& answer) {
    // The bag just before the answer: the frame kept before it, in the same
    // epoch -- nothing missed between the two.
    const Seen* before = nullptr;
    for (const Seen& f : seen_) {
        if (&f == &answer) break;
        before = &f;
    }
    std::size_t slot = 0;
    std::uint32_t item = 0;
    if (before == nullptr || before->epoch != answer.epoch || !one_fell(before->slots, answer.slots, slot, item)) {
        ++k.refused;
        return;
    }
    k.votes.push_back(Vote{ slot, item });
    while (k.votes.size() > kVotesKept) k.votes.pop_front();
    relearn(vk, k);
}

void BagLedger::relearn(std::uint32_t vk, Key& k) {
    std::map<std::uint32_t, int> by_item;
    for (const Vote& v : k.votes) ++by_item[v.item];
    std::uint32_t best = 0;
    int n = 0;
    for (const auto& entry : by_item)
        if (entry.second > n) {
            best = entry.first;
            n = entry.second;
        }
    const int all = static_cast<int>(k.votes.size());
    const bool learned = n >= kLearnVotes && n * 100 >= kLearnPercent * all;
    // The slot it drinks from: the newest vote's for that item -- a stack
    // used up, the next one is.
    std::size_t slot = 0;
    for (auto v = k.votes.rbegin(); v != k.votes.rend(); ++v)
        if (v->item == best) {
            slot = v->slot;
            break;
        }
    Event e;
    e.vk = vk;
    e.a = n;
    e.b = all;
    if (learned && (!k.learned || k.item != best)) {
        // Learned now, or learned to drink something else: checked afresh.
        k.checks.clear();
        k.agreements = 0;
        k.disagree_run = 0;
        k.low_said = false;
        e.kind = Event::Kind::Learned;
        e.item = best;
        e.slot = slot;
        if (events_.size() < kEventsMost) events_.push_back(e);
    } else if (!learned && k.learned) {
        k.checks.clear();
        k.agreements = 0;
        k.disagree_run = 0;
        k.low_said = false;
        e.kind = Event::Kind::Unlearned;
        e.item = k.item;
        e.slot = k.slot;
        if (events_.size() < kEventsMost) events_.push_back(e);
    }
    k.learned = learned;
    k.item = learned ? best : 0;
    k.slot = learned ? slot : 0;
    k.votes_for = n;
}

void BagLedger::judge_look(const Look& h, const Link& link) {
    const auto found = keys_.find(h.vk);
    if (found == keys_.end() || !found->second.learned) return;
    Key& k = found->second;
    // A bag frame near the look: the hotbar and the bag may each have seen
    // another moment of a potion drunk. Not judged.
    for (const Seen& f : seen_)
        if (f.at + kHotbarQuietMs >= h.at && f.at <= h.at + kHotbarSettleMs) return;
    const Seen* bag = as_of(h.at);
    if (bag == nullptr || bag->epoch != link.epoch) return;
    const BagSlotFrame& s = bag->slots[k.slot];
    bool agree = false;
    int packets = 0;
    if (h.count == 0) {
        // The hotbar's slot empty: none of the item left anywhere.
        packets = total_of(bag->slots, k.item);
        agree = packets == 0;
    } else if (s.item != k.item) {
        return;     // the slot it drank from holds something else now: nothing to set beside it
    } else {
        packets = s.count;
        agree = packets == h.count;
    }
    k.checks.push_back(agree);
    while (k.checks.size() > kChecksKept) k.checks.pop_front();
    Event e;
    e.vk = h.vk;
    e.item = k.item;
    e.slot = k.slot;
    if (agree) {
        ++k.agreements;
        ++k.hotbar_agree;
        k.disagree_run = 0;
        if (!k.low_said && low_trusted(k)) {
            k.low_said = true;
            e.kind = Event::Kind::LowTrusted;
            e.a = k.agreements;
            if (events_.size() < kEventsMost) events_.push_back(e);
        }
        return;
    }
    ++k.hotbar_disagree;
    ++k.disagree_run;
    k.low_said = false;
    e.kind = Event::Kind::HotbarDisagrees;
    e.a = h.count;
    e.b = packets;
    if (events_.size() < kEventsMost) events_.push_back(e);
    void_shop("the hotbar and the bag disagreed");
    if (k.disagree_run >= kDisagreeInARow) {
        // Distrusted: what it learned is forgotten, and learned again.
        ++k.distrusts;
        e.kind = Event::Kind::Distrusted;
        e.a = k.disagree_run;
        e.b = 0;
        if (events_.size() < kEventsMost) events_.push_back(e);
        k.votes.clear();
        k.learned = false;
        k.item = 0;
        k.slot = 0;
        k.votes_for = 0;
        k.checks.clear();
        k.agreements = 0;
        k.disagree_run = 0;
    }
}

void BagLedger::void_shop(const char* why) {
    ++voids_;
    last_void_ = why;
    if (shop_agree_ > 0 && events_.size() < kEventsMost) {
        Event e;
        e.kind = Event::Kind::ShopVoided;
        e.a = shop_agree_;
        e.why = why;
        events_.push_back(e);
    }
    shop_agree_ = 0;
}

std::vector<BagLedger::Event> BagLedger::take_events() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

// ---------------------------------------------------------------- asked

bool BagLedger::current_locked(const Link& link) const {
    return link.capturing && link.self_start == link.capture_start && !seen_.empty() &&
           seen_.back().epoch == link.epoch;
}

bool BagLedger::low_trusted(const Key& k) const {
    return k.learned && k.agreements >= kHotbarAgreements &&
           std::find(k.checks.begin(), k.checks.end(), false) == k.checks.end();
}

const BagLedger::Seen* BagLedger::as_of(std::uint64_t at) const {
    const Seen* out = nullptr;
    for (const Seen& f : seen_) {
        if (f.at > at) break;
        out = &f;
    }
    return out;
}

std::uint64_t BagLedger::newest_at() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_.empty() ? 0 : seen_.back().at;
}

std::uint64_t BagLedger::press_cursor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return press_cursor_;
}

bool BagLedger::current(const Link& link) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_locked(link);
}

bool BagLedger::low_total(std::uint32_t vk, const Link& link, int& total, std::uint32_t& item) const {
    std::lock_guard<std::mutex> lock(mutex_);
    total = -1;
    item = 0;
    const auto found = keys_.find(vk);
    if (found == keys_.end() || !low_trusted(found->second) || !current_locked(link)) return false;
    item = found->second.item;
    total = total_of(seen_.back().slots, item);
    return true;
}

BagLedger::ShopAnswer BagLedger::shop_ask(std::uint64_t now, const Link& link, int hp_below, int mp_below) {
    std::lock_guard<std::mutex> lock(mutex_);
    ShopAnswer a;
    // The capture started again, and no bag frame has come since: once per
    // start, the shop's trust goes.
    if (!seen_.empty() && seen_.back().start != link.capture_start && voided_start_ != link.capture_start) {
        voided_start_ = link.capture_start;
        void_shop("a new capture start with no 0x0068 since");
    }
    if (seen_.empty()) {
        a.why = "no bag frame yet";
        return a;
    }
    if (!current_locked(link)) {
        a.why = "the bag is not the game's now: something was missed since its last frame";
        return a;
    }
    const Seen& bag = seen_.back();
    a.hp_slots = slots_of(bag.slots, hp_item_);
    a.mp_slots = slots_of(bag.slots, mp_item_);
    if (hp_item_ == 0 || mp_item_ == 0) {
        a.why = "the potions' item numbers are not known";
        return a;
    }
    if (shop_agree_ < kShopAgreements) {
        a.why = "the shop's own counts have not agreed with the bag's twice since the last void";
        return a;
    }
    if (last_real_ms_ == 0 || now < last_real_ms_ || now - last_real_ms_ >= kRealRunMs) {
        a.why = "no real look within 2 h";
        return a;
    }
    if (last_due_skipped_) {
        a.why = "the last due look was skipped: every second one is real";
        return a;
    }
    if (last_potion_ms_ != 0 && bag.at <= last_potion_ms_) {
        a.why = "no 0x0068 since the macro's last potion press";
        return a;
    }
    if ((hp_below > 0 && a.hp_slots < hp_below) || (mp_below > 0 && a.mp_slots < mp_below)) {
        a.why = "the bag has fewer slots than the shop buys under";
        return a;
    }
    a.skip = true;
    return a;
}

BagLedger::ShopCheck BagLedger::shop_done(int hp_screen, int mp_screen, bool skipped, std::uint64_t at,
                                          const Link& link) {
    std::lock_guard<std::mutex> lock(mutex_);
    ShopCheck c;
    if (skipped) {
        ++skips_;
        last_due_skipped_ = true;
        c.why = "skipped";
    } else if (hp_screen < 0 || mp_screen < 0) {
        c.why = "the run counted no slots";
    } else {
        // A real look at the bag: the schedule starts from it, whatever the
        // bag here says.
        c.real = true;
        ++reals_;
        last_real_ms_ = at;
        last_due_skipped_ = false;
        const Seen* bag = as_of(at);
        if (hp_item_ == 0 || mp_item_ == 0) {
            c.why = "the potions' item numbers are not known";
        } else if (seen_.empty()) {
            c.why = "no bag frame yet";
        } else if (!current_locked(link)) {
            c.why = "the bag is not the game's now";
        } else if (bag == nullptr || bag->epoch != link.epoch) {
            c.why = "no bag frame of the game's from before the run";
        } else {
            c.compared = true;
            c.hp_packets = slots_of(bag->slots, hp_item_);
            c.mp_packets = slots_of(bag->slots, mp_item_);
            c.agreed = c.hp_packets == hp_screen && c.mp_packets == mp_screen;
            if (c.agreed) {
                ++shop_agree_all_;
                shop_disagree_run_ = 0;
                if (++shop_agree_ == kShopAgreements && events_.size() < kEventsMost) {
                    Event e;
                    e.kind = Event::Kind::ShopTrusted;
                    e.a = shop_agree_;
                    events_.push_back(e);
                }
            } else {
                ++shop_disagree_all_;
                ++shop_disagree_run_;
                if (shop_disagree_run_ >= kDisagreeInARow) {
                    void_shop("the shop's own counts and the bag's disagreed twice running");
                    shop_disagree_run_ = 0;
                } else {
                    void_shop("the shop's own counts and the bag's disagreed");
                }
            }
        }
    }
    c.agreements = shop_agree_;
    c.trusted = shop_agree_ >= kShopAgreements;
    return c;
}

BagLedger::CharmSeen BagLedger::charm(std::uint32_t item, std::uint64_t claimed_at, const Link& link) const {
    std::lock_guard<std::mutex> lock(mutex_);
    CharmSeen s;
    if (item == 0) {
        s.why = "not an item number";
        return s;
    }
    if (seen_.empty()) {
        s.why = "no bag frame yet";
        return s;
    }
    if (!current_locked(link)) {
        s.why = "the bag is not the game's now";
        return s;
    }
    const Seen* before = as_of(claimed_at);
    if (before == nullptr || before->epoch != link.epoch) {
        s.why = "no bag frame of the game's from before the charm";
        return s;
    }
    for (const Seen& f : seen_)
        if (f.at > claimed_at) ++s.frames_since;
    s.judged = true;
    s.before = total_of(before->slots, item);
    s.after = total_of(seen_.back().slots, item);
    return s;
}

std::vector<BagLedger::Held> BagLedger::present(const std::vector<std::uint32_t>& items, const Link& link) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Held> out;
    if (!current_locked(link)) return out;
    const BagFrame& bag = seen_.back().slots;
    for (const std::uint32_t item : items) {
        if (item == 0 || std::any_of(out.begin(), out.end(), [item](const Held& h) { return h.item == item; }))
            continue;
        Held h;
        h.item = item;
        h.slots = slots_of(bag, item);
        h.total = total_of(bag, item);
        if (h.slots > 0) out.push_back(h);
    }
    return out;
}

bool BagLedger::slot_at(std::size_t slot, const Link& link, BagSlotFrame& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out = BagSlotFrame{};
    if (slot >= kBagSlots || !current_locked(link)) return false;
    out = seen_.back().slots[slot];
    return true;
}

BagLedger::Summary BagLedger::summary(const Link& link) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Summary s;
    s.known = !seen_.empty();
    s.current = current_locked(link);
    s.hp_item = hp_item_;
    s.mp_item = mp_item_;
    if (s.known) {
        const Seen& bag = seen_.back();
        s.newest_at = bag.at;
        s.used_slots = used_slots(bag.slots);
        s.hp_slots = slots_of(bag.slots, hp_item_);
        s.hp_total = total_of(bag.slots, hp_item_);
        s.mp_slots = slots_of(bag.slots, mp_item_);
        s.mp_total = total_of(bag.slots, mp_item_);
    }
    s.frames = frames_;
    std::vector<std::uint32_t> vks = potion_keys_;
    std::sort(vks.begin(), vks.end());
    vks.erase(std::unique(vks.begin(), vks.end()), vks.end());
    for (const std::uint32_t vk : vks) {
        KeySummary ks;
        ks.vk = vk;
        const auto found = keys_.find(vk);
        if (found != keys_.end()) {
            const Key& k = found->second;
            ks.learned = k.learned;
            ks.item = k.item;
            ks.slot = k.slot;
            ks.votes_for = k.votes_for;
            ks.votes = static_cast<int>(k.votes.size());
            ks.presses = k.presses;
            ks.answered = k.answered;
            ks.unanswered = k.unanswered;
            ks.refused = k.refused;
            ks.hotbar_agree = k.hotbar_agree;
            ks.hotbar_disagree = k.hotbar_disagree;
            ks.low_trusted = low_trusted(k);
            ks.distrusts = k.distrusts;
        }
        s.keys.push_back(ks);
    }
    for (const auto& entry : keys_) {
        s.hotbar_agree += entry.second.hotbar_agree;
        s.hotbar_disagree += entry.second.hotbar_disagree;
    }
    s.shop_agree = shop_agree_all_;
    s.shop_disagree = shop_disagree_all_;
    s.shop_agreements = shop_agree_;
    s.shop_trusted = shop_agree_ >= kShopAgreements;
    s.voids = voids_;
    s.last_void = last_void_;
    s.skips = skips_;
    s.reals = reals_;
    s.last_due_skipped = last_due_skipped_;
    s.last_real_ms = last_real_ms_;
    return s;
}

} // namespace gobot
