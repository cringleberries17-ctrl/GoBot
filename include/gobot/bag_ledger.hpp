#pragma once

#include "gobot/self_frames.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gobot {

/// The bag as the game's server lists it (0x0068, gobot/self_frames.hpp),
/// checked against what the screen reads of the same things before anything
/// goes by it -- the packets plan, item E.
///
/// Nothing of 0x0068 is proven on today's server, so nothing is assumed:
///
/// - Which item a potion key drinks is learned. A press of the key answered
///   within kPressWindowMs by a bag frame in which exactly one slot fell by
///   exactly one is a vote for that slot and its item. A key is learned at
///   kLearnVotes votes for one item that are kLearnPercent of its last
///   kVotesKept; the slot it drinks from is the newest vote's, so a stack
///   used up and the next one taken changes nothing but the slot.
/// - A learned key is checked against the hotbar: the count the screen reads
///   on its hotbar slot against the count of the bag slot it drinks from, at a
///   moment no bag frame came near. It is trusted for "Low" -- judged on the
///   item's total in the bag -- after kHotbarAgreements agreements with none
///   of its last kChecksKept checks disagreeing. kDisagreeInARow
///   disagreements running distrust it: what it learned is forgotten, and it
///   is learned again from the start.
/// - The shop job's own counts of the two potions' slots are checked against
///   the bag's. The shop is trusted after kShopAgreements runs whose counts
///   agree, and loses that (a void) on any hotbar or shop disagreement, on a
///   press no bag frame answered within kPressWindowMs of a key learned to
///   drink one of the shop's two potions, and on a new capture start with no
///   bag frame since. Another key's unanswered press is only counted: a drink
///   of it changes nothing the shop counts, and the game passes over a press
///   inside its potion's cooldown (the potion check's second press).
/// - A due look of the shop is skipped (shop_ask) only when the shop is
///   trusted, the bag is the game's now and has enough of both, a real look
///   came within kRealRunMs, the last due look was not skipped -- every second
///   one is real -- and a bag frame came after the macro's last potion press.
///
/// "Out" is never the bag's: an empty hotbar slot is the hotbar's to say.
///
/// Pure: no clock (every time is given, on one steady clock), no frame read
/// and no line written. One lock inside, so that GoBot's thread and the trash
/// job's may both ask.
class BagLedger {
public:
    static constexpr std::uint64_t kPressWindowMs = 1500;   ///< a potion press and the bag frame it brings
    static constexpr std::uint64_t kLateMs = 500;           ///< a frame still on its way when that is over
    static constexpr int           kLearnVotes = 3;
    static constexpr int           kLearnPercent = 80;
    static constexpr std::size_t   kVotesKept = 10;
    static constexpr int           kHotbarAgreements = 2;
    static constexpr std::size_t   kChecksKept = 10;
    static constexpr int           kDisagreeInARow = 2;
    static constexpr int           kShopAgreements = 2;
    /// A hotbar look is judged this long after it, once the bag frames of
    /// that moment are all in.
    static constexpr std::uint64_t kHotbarSettleMs = 1000;
    /// No bag frame may have come this long before a hotbar look, nor before
    /// it was judged: the two might have seen two moments of a potion drunk.
    static constexpr std::uint64_t kHotbarQuietMs = 2000;
    /// The longest without a real look at the shop, skips or not.
    static constexpr std::uint64_t kRealRunMs = 2ull * 60 * 60 * 1000;
    static constexpr std::size_t   kFramesKept = 16;
    static constexpr std::size_t   kPendingMost = 64;     ///< presses and hotbar looks waiting to be judged

    /// How the capture stands as something is asked: whether the newest bag
    /// frame kept is the game's bag now.
    struct Link {
        bool          capturing = false;    ///< the capture runs
        std::uint32_t epoch = 0;            ///< PacketSelf::epoch() now
        std::uint64_t self_start = 0;       ///< PacketSelf::start_count(): the start of its last batch
        std::uint64_t capture_start = 0;    ///< PacketCapture::start_count() now
    };

    /// Something worth a line in the log, said as it happens.
    struct Event {
        enum class Kind {
            Learned,            ///< `vk` drinks `item` from `slot`: `a` votes of `b`
            Unlearned,          ///< `vk`'s last presses no longer agree on one item
            LowTrusted,         ///< `vk`'s "Low" goes by the bag now: `a` hotbar agreements
            HotbarDisagrees,    ///< the hotbar read `a` on `vk`'s slot, the bag's `slot` held `b`
            Distrusted,         ///< `vk` disagreed kDisagreeInARow times running: learned again
            ShopTrusted,        ///< `a` shop runs agree: a due look may be skipped
            ShopVoided,         ///< the shop's trust lost: `why`
        };
        Kind          kind = Kind::Learned;
        std::uint32_t vk = 0;
        std::uint32_t item = 0;
        std::size_t   slot = 0;
        int           a = 0;
        int           b = 0;
        const char*   why = "";       ///< a string literal
    };

    /// What shop_ask answers.
    struct ShopAnswer {
        bool        skip = false;
        int         hp_slots = -1;   ///< the bag's slots of the HP potion; -1 when the bag is not the game's now
        int         mp_slots = -1;
        const char* why = "";        ///< why not skipped; a string literal
    };

    /// What a shop run's own counts came to beside the bag's (shop_done).
    struct ShopCheck {
        bool        real = false;        ///< a real run that counted the bag
        bool        compared = false;
        bool        agreed = false;
        int         hp_packets = -1;     ///< the bag's slots of each potion as the run began
        int         mp_packets = -1;
        int         agreements = 0;      ///< runs agreeing since the last void
        bool        trusted = false;
        const char* why = "";            ///< why not compared; a string literal
    };

    /// A charm's stack in the bag before it was right-clicked and now.
    struct CharmSeen {
        bool        judged = false;
        int         before = -1;         ///< its total in the bag as of the claim
        int         after = -1;          ///< and in the newest frame
        int         frames_since = 0;    ///< bag frames since the claim
        const char* why = "";            ///< why not judged; a string literal
    };

    /// An item of a list, as the bag holds it.
    struct Held {
        std::uint32_t item = 0;
        int           slots = 0;
        int           total = 0;
    };

    /// One potion key, for the log.
    struct KeySummary {
        std::uint32_t vk = 0;
        bool          learned = false;
        std::uint32_t item = 0;
        std::size_t   slot = 0;
        int           votes_for = 0;    ///< of the last kVotesKept, for its item
        int           votes = 0;        ///< the last kVotesKept, all items
        int           presses = 0;
        int           answered = 0;     ///< presses a bag frame came within kPressWindowMs of
        int           unanswered = 0;
        int           refused = 0;      ///< answered, but not by exactly one slot one less
        int           hotbar_agree = 0;
        int           hotbar_disagree = 0;
        bool          low_trusted = false;
        int           distrusts = 0;
        /// The newest bag frame's total of `item`, every stack of it, once
        /// learned; -1 otherwise (2026.11.04d: what key H drank on
        /// 2026-11-04 was never counted in the log).
        int           total = -1;
    };

    /// Everything, for the minute line.
    struct Summary {
        bool          known = false;     ///< a bag frame has come
        bool          current = false;   ///< and it is the game's bag now
        std::uint64_t newest_at = 0;
        int           used_slots = 0;
        std::uint32_t hp_item = 0, mp_item = 0;
        int           hp_slots = 0, hp_total = 0, mp_slots = 0, mp_total = 0;
        std::uint64_t frames = 0;        ///< bag frames taken in all
        std::vector<KeySummary> keys;    ///< every potion key, by key code
        int           hotbar_agree = 0, hotbar_disagree = 0;
        int           shop_agree = 0, shop_disagree = 0;    ///< in all
        int           shop_agreements = 0;                  ///< since the last void
        bool          shop_trusted = false;
        int           voids = 0;
        const char*   last_void = "";
        int           skips = 0, reals = 0;
        bool          last_due_skipped = false;
        std::uint64_t last_real_ms = 0;
    };

    BagLedger() = default;
    BagLedger(const BagLedger&) = delete;
    BagLedger& operator=(const BagLedger&) = delete;

    /// The macro's potion keys: only their presses count.
    void set_potion_keys(const std::vector<std::uint32_t>& vks);
    /// The bag's item numbers of the HP and MP potions the shop buys; 0 for
    /// one not known, which no skip is made without.
    void set_shop_items(std::uint32_t hp_item, std::uint32_t mp_item);

    /// A bag frame, as it came at `at`, in PacketSelf's `epoch`, while its
    /// last batch was of capture start `start`. One kept already, or older
    /// than the newest kept, is passed over: frames may be given again.
    void take_bag(std::uint64_t at, std::uint32_t epoch, std::uint64_t start, const BagFrame& slots);
    /// Whether that frame is kept already.
    bool has_bag(std::uint64_t at, const BagFrame& slots) const;
    /// A key the macro pressed at `at`. Any key may be given, and given again:
    /// only a potion key's press counts, once.
    void press(std::uint32_t vk, std::uint64_t at);
    /// What the hotbar's slot of `vk` read in a look at `at`: its count, 0
    /// for an empty slot.
    void hotbar(std::uint32_t vk, int count, std::uint64_t at);
    /// Judge the presses and the hotbar looks whose time has come by `now`.
    void settle(std::uint64_t now, const Link& link);
    /// What happened since the last call.
    std::vector<Event> take_events();

    /// The newest bag frame's time; 0 for none.
    std::uint64_t newest_at() const;
    /// The newest press given, of any key, for PacketSelf::presses_since; 0
    /// for none.
    std::uint64_t press_cursor() const;
    /// The newest bag frame is the game's bag now: the capture runs, its
    /// start is the one PacketSelf last saw, and nothing was missed since.
    bool current(const Link& link) const;

    /// "Low" by the bag for `vk`: true, with the total in the bag of the item
    /// it drinks, only for a key trusted for it and a bag that is current.
    bool low_total(std::uint32_t vk, const Link& link, int& total, std::uint32_t& item) const;
    /// The same for a key only learned, trusted or not yet: for words only --
    /// a slot's "Low" says the bag's total beside the stack in use, not
    /// checked yet -- never for what goes by the bag (2026.11.04d).
    bool learned_total(std::uint32_t vk, const Link& link, int& total, std::uint32_t& item) const;

    /// A due look of the shop: whether it may be skipped, the potions' slots
    /// in the bag, and why not. A new capture start with no bag frame since
    /// voids the shop's trust here.
    ///
    /// Unused since 2026.11.09, with shop_done: the shop counts nothing now,
    /// and GoBot never skips one of its looks (GuiApp::bag_shop_text only
    /// logs). Kept, with its tests, for a later use of the bag's counts.
    ShopAnswer shop_ask(std::uint64_t now, const Link& link, int hp_below, int mp_below);

    /// A shop look done: `skipped`, or a real run whose own counts of the
    /// two potions' slots were `hp_screen` and `mp_screen` (-1: it counted
    /// nothing) as it began at `at`.
    ShopCheck shop_done(int hp_screen, int mp_screen, bool skipped, std::uint64_t at, const Link& link);

    /// `item`'s stack as of `claimed_at` and now.
    CharmSeen charm(std::uint32_t item, std::uint64_t claimed_at, const Link& link) const;

    /// Which of `items` the bag holds, when it is current; nothing otherwise.
    std::vector<Held> present(const std::vector<std::uint32_t>& items, const Link& link) const;

    /// Slot `slot` of the newest bag frame, when the bag is current (see
    /// current); false otherwise, and for a slot the bag does not have. Only
    /// ever read: for a charm's slot, checked before it is right-clicked.
    bool slot_at(std::size_t slot, const Link& link, BagSlotFrame& out) const;

    Summary summary(const Link& link) const;

    // ---- the bag's numbers ----

    /// How many of `item` the bag holds, in all of its slots.
    static int total_of(const BagFrame& bag, std::uint32_t item);
    /// How many slots hold `item`.
    static int slots_of(const BagFrame& bag, std::uint32_t item);
    /// How many slots hold anything.
    static int used_slots(const BagFrame& bag);
    /// Exactly one slot changed from `before` to `after`, and it fell by
    /// exactly one: the same item one fewer, or the last one gone. Its slot and
    /// item, then.
    static bool one_fell(const BagFrame& before, const BagFrame& after, std::size_t& slot, std::uint32_t& item);
    static bool same_bag(const BagFrame& a, const BagFrame& b);
    /// The item number a catalog id ends in -- 8820131 for "spend/hp/8820131"
    /// -- or 0 for an id that does not end in one, a picture's file name.
    static std::uint32_t item_of_id(const std::string& catalog_id);

private:
    struct Seen {
        std::uint64_t at = 0;
        std::uint32_t epoch = 0;
        std::uint64_t start = 0;
        BagFrame      slots{};
        bool          claimed = false;   ///< a press's answer already
    };
    struct Vote {
        std::size_t   slot = 0;
        std::uint32_t item = 0;
    };
    struct Key {
        std::deque<Vote> votes;          ///< the last kVotesKept
        bool          learned = false;
        std::uint32_t item = 0;
        std::size_t   slot = 0;
        int           votes_for = 0;
        std::deque<bool> checks;         ///< the last kChecksKept hotbar checks, true for agreeing
        int           agreements = 0;    ///< since it was learned
        int           disagree_run = 0;
        bool          low_said = false;  ///< LowTrusted told
        int           presses = 0, answered = 0, unanswered = 0, refused = 0;
        int           hotbar_agree = 0, hotbar_disagree = 0;
        int           distrusts = 0;
    };
    struct Press {
        std::uint32_t vk = 0;
        std::uint64_t at = 0;
    };
    struct Look {
        std::uint32_t vk = 0;
        int           count = 0;
        std::uint64_t at = 0;
    };

    bool is_potion_key(std::uint32_t vk) const;                  ///< under mutex_
    bool current_locked(const Link& link) const;                 ///< under mutex_
    bool low_trusted(const Key& k) const;                        ///< under mutex_
    const Seen* as_of(std::uint64_t at) const;                   ///< under mutex_: the newest frame at or before `at`
    void vote(std::uint32_t vk, Key& k, const Seen& answer);     ///< under mutex_
    void relearn(std::uint32_t vk, Key& k);                      ///< under mutex_
    void judge_look(const Look& h, const Link& link);            ///< under mutex_
    void void_shop(const char* why);                             ///< under mutex_

    mutable std::mutex                 mutex_;
    std::vector<std::uint32_t>         potion_keys_;
    std::uint32_t                      hp_item_ = 0, mp_item_ = 0;
    std::deque<Seen>                   seen_;
    std::uint64_t                      frames_ = 0;
    std::deque<Press>                  presses_;
    std::deque<Look>                   looks_;
    std::uint64_t                      press_cursor_ = 0;        ///< the newest press given, any key
    std::vector<std::uint32_t>         cursor_keys_;             ///< the keys given at press_cursor_
    std::uint64_t                      last_potion_ms_ = 0;      ///< the newest potion press given
    std::map<std::uint32_t, Key>       keys_;
    std::vector<Event>                 events_;
    int                                shop_agree_ = 0;          ///< since the last void
    int                                shop_agree_all_ = 0, shop_disagree_all_ = 0, shop_disagree_run_ = 0;
    int                                voids_ = 0;
    const char*                        last_void_ = "";
    std::uint64_t                      voided_start_ = 0;        ///< the capture start already voided for
    int                                skips_ = 0, reals_ = 0;
    bool                               last_due_skipped_ = false;
    std::uint64_t                      last_real_ms_ = 0;
};

} // namespace gobot
