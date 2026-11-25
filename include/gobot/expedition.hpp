#pragma once

#include "gobot/common.hpp"
#include "gobot/config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gobot {

// ============================================================================
// Đào khoáng and phó bản Cổ Thụ (2026.11.19)
//
// The owner, 2026-11-19: "làm phần đào khoáng và Phó bản Cổ Thụ / Cổ Long
// (Ngọc Thụ Thần)" -- both at once, from GoC 0.6.5.0's own code, with no
// live capture ("Không, làm theo GoC"), as Thành viên and as Đội trưởng.
//
// Everything here is what GoC does, read statically from go_client.exe and
// GhostCore.dll (TickMining 0x1400FE170, TickCoThu 0x14011DE40): the maps,
// the places, the item ids, the order of the steps. GoC walks with its own
// server's move service; GoBot walks with the hunter's walk back (Hunter::
// set_trip). Nothing here has been seen working on the owner's client: every
// step says what it does and why it gave up, in the log and on the card.
//
// This header is the part with no screen and no keys -- the packets it reads,
// the settings, and the two planners -- so the tests drive it as they like.
// The worker that presses and clicks is the window's (simple_expedition.cpp).
// ============================================================================

// ---- the game's numbers, as GoC has them ----

namespace expedition {

/// One mine of Mỏ Quặng Thủy Quật (GoC's table at 0x140258ED0): its map, its
/// name, its gate on the hub t20_s70 and the gate inside it back out -- no dig
/// spot is taken within kGateClearPx of that one, across.
struct Mine {
    const char* stem;
    const char* name;
    float       entry_x, entry_y;   ///< the gate into it, on t20_s70
    float       exit_x, exit_y;     ///< the gate out, in the mine
};
extern const std::array<Mine, 4> kMines;
const Mine* mine(const std::string& stem);

/// The pickaxes (GoC's table at 0x140258BE0): id and name.
struct Pickaxe {
    std::uint32_t id;
    const char*   name;
};
extern const std::array<Pickaxe, 14> kPickaxes;
bool        is_pickaxe(std::uint32_t id);
const char* pickaxe_name(std::uint32_t id);     ///< "" for none

/// The maps of the mining zone: the hub, the four mines, Thanh Âm Quan
/// (t1_s1) and t1_s4. Anywhere else, mining waits.
constexpr const char* kHub = "t20_s70";
constexpr const char* kTown = "t1_s1";
constexpr const char* kTown4 = "t1_s4";
bool in_mining_zone(const std::string& stem);

/// Thợ Mỏ, who sells the pickaxes, on the hub.
constexpr float kMinerX = 492.0f, kMinerY = 659.0f;
/// The Teleporters, and the row each one's menu is chosen by.
constexpr float kTownTeleX = 975.0f, kTownTeleY = 1525.0f;
constexpr float kHubTeleX = 55.0f, kHubTeleY = 655.0f;
inline const char* const kToHubRow = "Mỏ Quặng Thủy Quật-3K Vàng";
inline const char* const kToTownRow = "Thanh Âm Quan-1K Vàng";
inline const char* const kShopRowText = "Shop";
/// The gates between t1_s1 and t1_s4.
constexpr float kTown4GateX = 4650.0f, kTown4GateY = 1420.0f;   ///< on t1_s4, to t1_s1
constexpr float kTownGateX = 670.0f, kTownGateY = 1710.0f;      ///< on t1_s1, to t1_s4

/// A dig spot: this far across from the mine's exit gate, from another
/// player (both ways), and the steps it is looked for in.
constexpr int kGateClearPx = 192;
constexpr int kPlayerClearPx = 64;
constexpr int kSpotStepPx = 64;
constexpr int kSpotSteps = 32;
constexpr int kSpotTolX = 16, kSpotTolY = 64;
/// Arriving anywhere else: within this either side, and above and below.
constexpr int kArriveTolX = 48, kArriveTolY = 16;

/// Cổ Thụ's maps (GoC 0x1400BA760) and places.
constexpr const char* kCoThuLobby = "t25_s60";      ///< Trung Tâm Rừng Đại Thụ
constexpr const char* kCoThuWait = "t1981_s1";      ///< Phòng chờ, where a death sends you back
constexpr const char* kCoThuBoss = "t1981_s2";
constexpr const char* kCoThuReward = "t1981_s3";
constexpr float kCoThuNpcX = 1278.0f, kCoThuNpcY = 2864.0f;
constexpr float kCoThuDoorX = 1200.0f, kCoThuDoorY = 2298.0f;
constexpr float kCoThuUpperX = 1760.0f, kCoThuUpperY = 368.0f;   ///< Tầng trên
constexpr float kCoThuLowerX = 1871.0f, kCoThuLowerY = 806.0f;   ///< Tầng dưới
constexpr float kCoThuRewardX = 665.0f, kCoThuRewardY = 2280.0f;
inline const char* const kCoThuRow = "Ngọc Thụ Thần - Level trên 160";
/// The boss's monster type, and how long it is gone before it is dead.
constexpr std::uint32_t kCoThuBossType = 1016004;
constexpr std::uint64_t kBossGoneMs = 1500;
/// Đèn Cấp Cao, the leader's entry lamp, in bag Khác (0x0069).
constexpr std::uint32_t kLampItem = 8991013;

/// Cổ Long (Ngọc Long Tổ), GoC's TickCoLong (0x1400EB270, planner
/// 0x1400E2860): the same four rooms as Cổ Thụ on other maps (0x1400E2630).
/// The leader's ticket, Lư Hương Long Mỹ, is bought from the lobby's NPC --
/// the one that takes you in, its menu's "Shop" row. GoC knows no boss here:
/// the fight lasts until the game moves you to the reward's room.
constexpr const char* kCoLongLobby = "t60_s19";
constexpr const char* kCoLongWait = "t1901_s1";     ///< "map chuẩn bị", where a death sends you back
constexpr const char* kCoLongBoss = "t1901_s2";
constexpr const char* kCoLongReward = "t1901_s3";   ///< "map kết quả"
constexpr float kCoLongNpcX = 847.0f, kCoLongNpcY = 802.0f;     ///< GoC buys at 848, goes in at 847
constexpr float kCoLongDoorX = 108.0f, kCoLongDoorY = 689.0f;
constexpr float kCoLongLeftX = 100.0f, kCoLongLeftY = 632.0f;
constexpr float kCoLongRightX = 920.0f, kCoLongRightY = 633.0f;
constexpr float kCoLongRewardX = 860.0f, kCoLongRewardY = 802.0f;
inline const char* const kCoLongRow = "Ngọc Long Tổ - Level trên 160";
/// Lư Hương Long Mỹ: GoC takes either id, in bag Khác, and buys the first.
constexpr std::uint32_t kIncenseItem = 8990188;
constexpr std::uint32_t kIncenseItem2 = 8991014;
inline const char* const kIncenseName = "Lư Hương Long Mỹ";

/// One of the two dungeons, as the planner walks it.
struct Dungeon {
    const char* tag;            ///< "CoThu": the ini's section and the log's
    const char* name;           ///< "Cổ Thụ"
    const char* lobby_name;     ///< where to stand to start
    const char* lobby;
    const char* wait;
    const char* boss;
    const char* reward;
    float npc_x, npc_y;
    float door_x, door_y;
    float spot_x[2], spot_y[2];
    const char* spot_name[2];
    float reward_x, reward_y;
    const char* row;            ///< the NPC's menu row that takes you in
    const char* row_name;       ///< "Ngọc Thụ Thần"
    std::uint32_t ticket, ticket2;  ///< the leader's entry item(s), bag Khác
    const char* ticket_name;
    bool buys_ticket;           ///< from the NPC's Shop when there is none
    std::uint32_t boss_type;    ///< 0: no boss known -- the fight ends with the room
    int ok_looks;               ///< the entry's Xác Nhận, looks for it
    /// The reward NPC's way-out row, clicked by its text (Cổ Long's
    /// "[ Thoát khỏi Cõi Vàng ]"); none: the talk key and Xác Nhận alone.
    const char* leave_row = nullptr;
    /// The reward NPC talked to by a click on it where the character comes
    /// in, no walk (the owner, 2026-11-19, Cổ Long: "chỉ cần click vào NPC").
    bool click_reward_npc = false;
};
/// Out of the reward room within this of coming in (the owner, 2026-11-19:
/// "cần rời trong 30 giây"); the reward itself is given up at kRewardGiveUpMs.
constexpr std::uint64_t kLeaveWithinMs = 30000;
constexpr std::uint64_t kRewardGiveUpMs = 15000;
/// No tab of these stops for good (2026.11.24, the owner: "Kiểm tra các phần
/// khác xem có phần nào không làm được thì dừng không, cần thay cơ chế này
/// bằng 1 cơ chế khác"; then "thử lại luôn ... chứ không phải là dừng 90s").
/// Until then three failed purchases of Lư Hương, three of a pickaxe, a job
/// failing three times, no dig spot, five restarts of a dungeon's room or a
/// refusal with Tự thử lại unticked stopped Đào khoáng or Phó bản until it was
/// unticked. Now a purchase that did not go is tried again this long after --
/// the owner's 30 s for the shop's trouble (bag::kShopTroubleAgainMs) -- and a
/// round of failures is followed by a gap of the same, the hunter fighting
/// meanwhile, then the steps again: with no limit.
constexpr std::uint64_t kTripRetryMs = 30000;
/// The gap after a round of failures that are not the shop's (2026.11.24
/// review, the owner's correction "thử lại luôn ... chứ không phải là dừng
/// 90s": only the shop's and the party's trouble wait 30 s): the map not
/// changing, no dig spot, a spot not reached, a mining job failing 3 times,
/// a dungeon room's restarts. A few seconds -- the trip ended meanwhile so
/// the hunter has its turn -- then the steps again.
constexpr std::uint64_t kRoundGapMs = 5000;
extern const Dungeon kCoThu;
extern const Dungeon kCoLong;

} // namespace expedition

// ---- the packets: the bag's other tabs, and digging ----

/// One slot of a bag frame: an item id (0 for none) and how many.
struct TripSlot {
    std::uint32_t item = 0;
    std::uint16_t count = 0;
};

/// What the game's frames say of the bag's tabs and of digging (2026.11.19).
///
/// The bag: 0x0065 is what the character wears (GoC reads 17 ids at 12 +
/// 4j), 0x0066 Trang bị 1, 0x0067 Trang bị 2, 0x0068 Tiêu Dùng (PacketSelf
/// reads it too), 0x0069 Khác, 0x006A Nguyên liệu -- 72 slots each, ids at
/// 12 + 4j and counts at 300 + 2j as 0x0068 has them (GhostCore's
/// ParseInventorySnapshot, 0x18005DC40). From the server inside 0x0081.
///
/// Digging: 0x0132 from the server, 20 bytes, a character's id u32 at 12 and
/// a value i32 at 16 (GhostCore 0x18004E910): 1 digging begun, -1 dig mode
/// over, an ore's id one dug. Kept for every character, asked for yours.
///
/// None of it has been seen on the owner's client: the sizes are taken as
/// GoC takes them, and every first frame of a kind is written to the log.
class TripPackets {
public:
    static constexpr std::uint16_t kWorn = 0x0065;
    static constexpr std::uint16_t kEquip1 = 0x0066;
    static constexpr std::uint16_t kOther = 0x0069;
    static constexpr std::uint16_t kDig = 0x0132;
    static constexpr std::size_t   kWornSlots = 17;
    static constexpr std::size_t   kTabSlots = 72;

    struct Tab {
        bool                  known = false;
        std::vector<TripSlot> slots;
        std::uint64_t         at_ms = 0;
        std::uint64_t         seq = 0;      ///< frames of it taken, ever
    };
    /// 0 none since the start or a map entry, 1 dig mode over, 2 digging.
    struct Dig {
        int           state = 0;
        std::uint32_t last_item = 0;
        std::uint64_t seq = 0;
        std::uint64_t at_ms = 0;
    };

    void on_packets(const std::vector<GamePacket>& all);
    void reset();

    Tab worn() const;
    Tab equip1() const;
    Tab other() const;
    /// The digging of character `id`; with no id known, the newest of anyone's.
    Dig dig(std::optional<std::uint32_t> id) const;

    /// A frame's slots, or none for a frame that is not one: `slots` ids at 12
    /// + 4j, and -- for a tab -- counts at 300 + 2j. For the tests.
    static std::optional<std::vector<TripSlot>> read_tab(const GamePacket& p, std::size_t slots, bool counts);
    /// 0x0132's id and value, or none.
    static std::optional<std::pair<std::uint32_t, std::int32_t>> read_dig(const GamePacket& p);

private:
    mutable std::mutex mutex_;
    Tab worn_, equip1_, other_;
    std::map<std::uint32_t, Dig> digs_;
    std::uint32_t newest_digger_ = 0;
    std::uint64_t dig_seq_ = 0;
    int said_ = 0;      ///< first frames written to the log
};

/// Whether `now` holds anything `before` did not (2026.11.24, the money guard
/// of a purchase in an NPC's shop): a slot with more of its item, or with
/// another item -- under any id, one GoBot knows or not -- than it had. A slot
/// emptied or with less is no gain; nor is a tab not read. A `before` not read
/// had nothing: whatever `now` holds is new.
bool tab_gained(const TripPackets::Tab& before, const TripPackets::Tab& now);

// ---- the settings: configs\expedition.ini ----

/// [Mining]. GoC's [Mining] of item_processor.ini, the parts GoBot does: the
/// junk drop, the card exchange and the selling are not here.
struct MiningConfig {
    bool          enabled = false;
    std::string   map_key = "t21_s71";      ///< the mine to dig in
    bool          auto_replace = true;      ///< equip another when one breaks
    bool          auto_buy = false;         ///< buy at Thợ Mỏ
    std::uint32_t buy_pickaxe = 8090111;    ///< Cuốc Gỗ [30]
    int           buy_when = 0;             ///< pickaxes left at or under which to buy; 0 = none left
    int           buy_quantity = 1;
    std::uint32_t talk_key = 0x20;          ///< the normal attack: talks to an NPC, starts digging

    void load(const IniFile& ini);
    void store(IniFile& ini) const;
};

/// [CoThu] and [CoLong]: GoC's sections of configs\dungeons.ini, the parts
/// GoBot does. One shape for both, as GoC has it.
struct CoThuConfig {
    bool          enabled = false;
    int           role = 0;                 ///< 0 Đội trưởng, 1 Thành viên
    int           minimum_members = 2;      ///< the party, you included, before going in
    int           wait_seconds = 120;       ///< once it is there, before going in
    int           boss_position = 2;        ///< 0 Tầng trên, 1 Tầng dưới, 2 where you come in
    /// Tự thử lại khi không vào được: unread since 2026.11.24 -- a refusal is
    /// always tried again after entry_retry_seconds (off, it stopped the tab
    /// for good) -- still read and written, so that old files load.
    bool          entry_retry = true;
    int           entry_retry_seconds = 60;
    int           maximum_runs = 0;         ///< 0: no limit
    int           loot_presses = 12;        ///< at the spot once the boss is dead
    std::uint32_t talk_key = 0x20;
    std::uint32_t loot_key = 0;             ///< 0: the hunter's

    void load(const IniFile& ini, const char* section = "CoThu");
    void store(IniFile& ini, const char* section = "CoThu") const;
};
using DungeonConfig = CoThuConfig;

/// configs\expedition.ini, read and written whole.
struct ExpeditionSettings {
    MiningConfig mining;
    CoThuConfig  cothu;
    CoThuConfig  colong;    ///< [CoLong]
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

// ---- what a planner sees, and asks for ----

/// A monster as a planner needs it.
struct TripMob {
    std::uint32_t type = 0;
    std::uint32_t hp = 0;
    bool          dead = false;
    float         x = 0.0f, y = 0.0f;
};

/// One moment, as the worker hands it to a planner.
struct TripSnap {
    bool          world = false;    ///< the packets know the map and where you stand
    std::string   stem;             ///< "t20_s70"
    std::uint32_t epoch = 0;        ///< map entries ever (PacketWorld::Place::epoch)
    float         x = 0.0f, y = 0.0f;
    bool          settled = false;  ///< on the ground
    bool          hp_known = false;
    std::uint32_t hp = 0;
    TripPackets::Tab worn, equip1, other;
    TripPackets::Dig dig;
    std::vector<TripMob> mobs;
    std::vector<std::pair<float, float>> players;   ///< others on the map; GoBot reads none yet
    int           party = -1;       ///< the party HUD, you included; -1 not read
    /// Whether x2 lies on the floor x, y stands on, followed across: the map
    /// file's (GameMapBook floors::follow_to). Unset: anywhere goes.
    std::function<bool(int x, int y, int x2)> same_floor;
    /// A round of the pet's HP food is owed for this entry into the lobby, or
    /// under way (PetRestock::waits, 2026.11.24): the lobby's steps wait for
    /// it, PetRestock::kWaitMostMs at most.
    bool          pet_restock = false;
};

/// What a planner asks the worker to do.
enum class TripJobKind : std::uint8_t {
    None,           ///< nothing this tick: `label` says why
    Walk,           ///< go to stem x, y within tol, nothing fought
    Fight,          ///< go to x, y and fight there: the boss
    Gate,           ///< ↓ every 300 ms, up to `count` times, until the map changes
    Talk,           ///< the talk key once: the NPC stood beside answers
    PickRow,        ///< the row `text` of a menu clicked; with `confirm` the box after it too
    ClickOk,        ///< the Xác Nhận of a box, `count` looks
    WatchRefusal,   ///< about 5 s for a one-button notice: Refused if one came (and it is closed)
    StartDig,       ///< the talk key, then 3 s for 0x0132 to say digging
    ExitDig,        ///< ESC, its box's Xác Nhận, then 0x0132 to say over
    Equip,          ///< the bag's Trang bị 1, slot `slot`, double-clicked
    Buy,            ///< in an NPC's shop: `item` (`text` its name), `count` of them
    CloseNotice,    ///< a one-button notice closed: the pickaxe broke
    Loot,           ///< the loot key `count` times
    CompleteQuest,  ///< the quest's complete button, up to 3 times
    ExitRoom,       ///< up to 40 x: Xác Nhận if a box is up, else the talk key, until the map changes
    // No Fatal since 2026.11.24: no job stops a tab for good (kTripRetryMs).
};
const char* trip_job_name(TripJobKind k);

struct TripJob {
    TripJobKind   kind = TripJobKind::None;
    std::string   label;            ///< the card's line, in Vietnamese
    std::string   stem;
    float         x = 0.0f, y = 0.0f;
    int           tol_x = expedition::kArriveTolX, tol_y = expedition::kArriveTolY;
    std::string   text;
    bool          confirm = false;
    int           slot = -1;
    std::uint32_t item = 0;
    std::uint32_t item2 = 0;        ///< Buy: another id that counts as `item`
    bool          other_tab = false;    ///< Buy: it comes into bag Khác, not Trang bị 1
    int           count = 1;
    std::uint64_t max_ms = 0;       ///< Walk, ExitRoom: given up after this (0: the job's own)
    /// Talk, ExitRoom: a click on the NPC standing at x,y of `stem` (where it
    /// is on screen from your own name plate), the talk key when that fails.
    bool          by_click = false;
};

enum class TripResult : std::uint8_t {
    Ok,         ///< done
    Retry,      ///< did not work: tried again after a pause
    Busy,       ///< could not start (the game not in front, a hold): asked again soon
    Refused,    ///< WatchRefusal: the notice came
    // No Fatal since 2026.11.24: a job that does not go is a Retry.
};
const char* trip_result_name(TripResult r);

// ---- a purchase in an NPC's shop (2026.11.24) ----
//
// The owner, 2026-11-24: "khi làm đội trưởng để đi Cổ Long thì không mua được
// Lư Hương. Cụ thể là khi ấn vào Lư Hương thì không ấn mua mà lại tắt luôn
// shop mua". The Buy job bought by a click on the item's name and one on Mua,
// a Xác Nhận looked for once half a second later, and ESC three times on any
// miss. Now as the player buys, as the potion shop has bought live since
// 2026-10-31 (BagCleaner::shop_purchase): the item's cell double-clicked, the
// quantity question answered (simple_expedition.cpp, do_buy). What it decides
// with no screen is here, for the tests.

/// The words of an item's name as a shop's list draws them: with no spaces --
/// "CuốcHắcQuang" for Cuốc Hắc Quang in GoC's own picture of Thợ Mỏ's shop,
/// "Rượulátre" for Rượu lá tre in this client's potion shop.
std::string shop_list_text(const std::string& name);

/// What a shop's quantity box needs typed to hold `want` when it reads `have`
/// (CountReader::read_field: 0 for nothing in it): nothing when it holds it
/// already -- the game puts a 1 there -- the digits when it is empty, the rest
/// when what it holds is the start of `want`, and otherwise `erase`
/// Backspaces before all of `want`. Not `ok` -- nothing typed, the question
/// answered Hủy Bỏ -- for a box that does not read or is gone (a number under
/// 0), or a `want` under 1.
struct QuantityKeys {
    bool        ok = false;
    int         erase = 0;
    std::string digits;
};
QuantityKeys quantity_keys(int have, int want);

/// One look at the game after the double-click on an item of an NPC's shop,
/// as do_buy sees it.
struct BuyLookIn {
    bool more_in_bag = false;   ///< the bag's frames have more of the item than before the double-click
    bool full = false;          ///< the full-bag message (bag::shop_full_up)
    bool message = false;       ///< a message with one Xác Nhận and no dialog of two buttons (bag::lone_ok)
    int  dialogs = 0;           ///< dialogs of two buttons, the shop's own Mua | Thoát left out
    /// That one dialog is a question GoBot knows to be another's: the bin's,
    /// a fragment's registration, or -- with no box -- the revive, drop or
    /// sell question by its words.
    bool known = false;
    bool box = false;           ///< a quantity box's field where its box would be (bag::kFieldBox)
    bool bare = false;          ///< the light bar there, no box (bag::kFieldNone)
    /// Drawn in the game's own question frame, as the sell question is, and
    /// none of the questions GoBot has a picture of; with a box, the box
    /// reading (bag::shop_question).
    bool question = false;
};
/// What that look shows. Bought: the item came, with no question at all.
/// Question: the quantity question, to read and answer. Confirm: a question
/// with no box in the game's question frame -- an item bought one at a time,
/// a piece of equipment -- to be answered Xác Nhận. Full and Message: the game
/// said no (the bag full, the money short, ...): its message closed, nothing
/// more bought in this shop. Other: a dialog that is none of these -- never
/// typed into or confirmed, answered Hủy Bỏ. Nothing: none of it yet, or a box
/// half drawn: looked at again. do_buy acts on Confirm, Other and Message only
/// when two looks in a row agree, a frame caught being drawn being none.
enum class BuyLook : std::uint8_t { Nothing, Bought, Question, Confirm, Full, Message, Other };
BuyLook buy_look(const BuyLookIn& in);
const char* buy_look_name(BuyLook l);

/// The part of a dungeon run the other features' timed jobs keep out of
/// (2026.11.21, the audit's B6; the owner: "không để các chức năng block
/// nhau"): the boss's room -- the fight, the loot -- and the reward's room,
/// out within kLeaveWithinMs. A timed trash pass, the charms, the shop, a
/// quick quest round (the one 30 s after a revive too), a Tự chat line and a
/// party invite held the game there -- the hunter swings at nothing under a
/// hold -- and the reward room's NPC, its way out and its 30 s went with
/// them. None anywhere else: ordinary hunting, the lobby, the waiting room.
enum class TripQuiet : std::uint8_t { None, Boss, Reward };
const char* trip_quiet_name(TripQuiet q);   ///< "boss fight", "reward room", "none"
/// What the cards say of it: "đang đánh boss phó bản", ...
const char* trip_quiet_vi(TripQuiet q);

/// The pet's HP food bought on reaching TổCổLong (2026.11.24, the owner: "Khi
/// ra tới bản đồ TổCổLong thì cần mua lại HP cho pet. Cái này ở trong tab Linh
/// Thú của người giữ sân và là item "Thịtbò" => cần có lựa chọn để mua và mua
/// bao nhiêu slot tương tự như mua HP MP"). TổCổLong is t60_s19 -- Cổ Long's
/// lobby, expedition::kCoLongLobby, where a run leaves you. Each entry into it
/// -- a map entry of the packets' (PacketWorld::Place::epoch, never cleared)
/// not looked at before, on that map -- with the box ticked and the bot running
/// (`armed`) owes one round of CleanSettings::buy_pet_slots purchases of the
/// pet's Thịt bò; the trash worker buys it as soon as it may (GuiApp::
/// trash_loop), and the dungeon's lobby steps wait for it (TripSnap::
/// pet_restock) -- kWaitMostMs at most from the arrival, never longer: past
/// that the round is given up until the next arrival, and the dungeon goes on.
/// A round that went wrong is tried again kAgainMs on (the owner's 30 s for the
/// shop's trouble) while the entry's time lasts; one the game said no to (its
/// bag full, a message), or that bought, is over. An entry looked at while not
/// armed owes nothing, not even later in the same stay: only an arrival does.
/// Safe from any thread: the window's tick and the expedition worker both
/// look, whichever sees the entry first.
class PetRestock {
public:
    static constexpr std::uint64_t kWaitMostMs = 90'000;
    static constexpr std::uint64_t kAgainMs = 30'000;

    /// One look at where the character is: the packets' map (`stem`, empty
    /// when not known) and their map entries ever (`epoch`, 0 for none yet).
    /// True for the look that sees an armed arrival: a round owed from `now`.
    /// An entry onto any other map ends a round owed for the last one.
    bool look(const std::string& stem, std::uint32_t epoch, bool armed, std::uint64_t now);
    /// The entry whose round is owed -- put off or not, under way or not --
    /// within kWaitMostMs of its arrival; 0 for none.
    std::uint32_t owed(std::uint64_t now) const;
    /// The entry whose round may go now: owed, and its put-off over; 0 none.
    std::uint32_t due(std::uint64_t now) const;
    /// The round of `epoch` tried again no sooner than `at_ms`.
    void put_off(std::uint32_t epoch, std::uint64_t at_ms);
    /// The round of `epoch` over -- bought, the game's no, given up, the bot
    /// stopped -- until the next arrival.
    void over(std::uint32_t epoch);
    /// Owed, and kWaitMostMs gone by since its arrival: over now, and true --
    /// once -- for whoever ran it to say so.
    bool expire(std::uint64_t now);
    /// The round of `epoch` had its first full try and it ended in trouble
    /// (2026.11.24 review): still owed and tried again kAgainMs on, but the
    /// lobby no longer waits for it -- a failure that repeats every time
    /// (Thịtbò not on the first page, the strip not read) held the whole
    /// party 90 s at every arrival ("không để các chức năng block nhau").
    void tried(std::uint32_t epoch);
    /// Whether the dungeon's lobby steps wait at entry `epoch`: its round
    /// owed, within kWaitMostMs of the arrival, and not yet tried in vain.
    bool waits(std::uint32_t epoch, std::uint64_t now) const;
    /// When the owed round's entry came; 0 for none.
    std::uint64_t arrived_ms() const;

private:
    mutable std::mutex mutex_;
    std::uint32_t looked_ = 0;      ///< the last entry looked at (0: none yet)
    std::uint32_t owed_ = 0;        ///< the entry whose round is owed (0: none)
    std::uint64_t arrived_ms_ = 0;
    std::uint64_t again_ms_ = 0;    ///< not tried before this
    bool          tried_ = false;   ///< tried once in vain: the lobby goes on (tried())
};

// ---- the planners ----

/// Đào khoáng, as GoC's MiningUpdate (0x1400FAFD0) and its planner
/// (0x1400FC160) have it: one job at a time, asked for by next() and answered
/// by done().
class MiningPlanner {
public:
    enum class Phase : std::uint8_t {
        Plan, Gate, TalkTele, PickTele, WaitMap, TalkMiner, PickShop, Buy,
    };

    /// The next job, or None with `label` the card's line. Never a second job
    /// before done() has answered the last.
    TripJob next(const TripSnap& s, const MiningConfig& c, std::uint64_t now);
    void    done(const TripJob& j, TripResult r, const TripSnap& s, std::uint64_t now);
    void    reset();

    const std::string& status() const { return status_; }
    Phase phase() const { return phase_; }
    /// A gap of kTripRetryMs running (2026.11.24): after a round of failures
    /// -- a job three times, the map not changing three times, no dig spot,
    /// no spot reached -- or a purchase that did not go, in place of the stop
    /// there was. Nothing is asked of the worker meanwhile, which ends the
    /// trip so that the hunter fights (the other useful thing first).
    bool resting(std::uint64_t now) const { return now < rest_until_; }
    /// A pickaxe bought -- or perhaps bought -- with no frame of Trang bị 1
    /// since to say so (the money guard, 2026.11.24): none is bought again
    /// until one comes.
    bool pickaxe_unconfirmed() const { return pick_out_; }

    /// Where to walk from `from` toward `to` (GoC's route table, 0x1400FBB40):
    /// a gate (↓) or a Teleporter (talk, then its row). None for no way.
    struct Route {
        float       x = 0.0f, y = 0.0f;
        bool        teleporter = false;
        const char* row = "";
        const char* label = "";
    };
    static std::optional<Route> route(const std::string& from, const std::string& to);

    /// The dig spot GoC takes: along your height, kSpotStepPx steps either
    /// way in turn starting with `dir`, the `skip`-th that keeps clear of the
    /// mine's exit gate and of other players and -- with a map -- is on your
    /// floor, and is none of `avoid` (2026.11.24: within half a step of a spot
    /// where digging did not start). None for none.
    static std::optional<float> dig_spot(const TripSnap& s, const expedition::Mine& m, int dir, int skip,
                                         const std::vector<float>& avoid = {});

    /// The pickaxes at hand: worn (1 or 0) and in Trang bị 1.
    static bool wears_pickaxe(const TripSnap& s);
    static int  pickaxes_in_bag(const TripSnap& s);
    static int  first_pickaxe_slot(const TripSnap& s);

private:
    TripJob wait(const std::string& why);
    TripJob job(TripJobKind k, const std::string& label);
    TripJob walk(const std::string& stem, float x, float y, int tx, int ty, const std::string& label);
    TripJob toward(const TripSnap& s, const std::string& to, const char* why);
    /// A gap of `gap` from `now` -- kTripRetryMs for the shop's trouble,
    /// kRoundGapMs for the rest -- `why` the card's line.
    void    rest(std::uint64_t now, const std::string& why, std::uint64_t gap = expedition::kTripRetryMs);

    Phase         phase_ = Phase::Plan;
    std::string   status_;
    bool          pending_ = false;     ///< a job out, not yet done()
    std::string   stem_;
    std::uint32_t epoch_ = 0;
    std::uint64_t backoff_until_ = 0;
    std::uint64_t rest_until_ = 0;      ///< a kTripRetryMs gap (resting)
    std::uint64_t wait_since_ = 0;      ///< WaitMap
    int           fails_ = 0;
    int           map_fails_ = 0;       ///< WaitMap run out, since the last map entry
    int           skips_ = 0;
    int           dir_ = 1;
    bool          on_spot_ = false;
    float         spot_x_ = 0.0f, spot_y_ = 0.0f;
    bool          walking_spot_ = false;
    float         want_x_ = 0.0f;
    /// Where digging did not start on this map entry: no spot within half a
    /// step of them is walked to again until none is left (2026.11.24).
    std::vector<float> no_dig_x_;
    bool          was_worn_ = false;    ///< a pickaxe was seen worn
    bool          broken_ = false;
    bool          notice_closed_ = true;
    // The money guard (2026.11.24): Trang bị 1 and the pickaxe worn as the
    // Buy went out; a purchase that may have gone, and the frame it was
    // judged on.
    TripPackets::Tab pick_before_;
    bool          pick_worn_ = false;
    bool          pick_out_ = false;
    std::uint64_t pick_seq_ = 0;
    std::uint64_t dig_seq_at_start_ = 0;
};

/// Cổ Thụ, as GoC's planner (0x140118D30) and its 25 steps have it; Cổ Long
/// (0x1400E2860) the same with the ticket bought first and no boss known.
class DungeonPlanner {
public:
    explicit DungeonPlanner(const expedition::Dungeon& d = expedition::kCoThu) : d_(&d) {}

    enum class Zone : std::uint8_t { None, Lobby, Wait, Boss, Reward };
    static Zone zone_in(const expedition::Dungeon& d, const std::string& stem);
    static Zone zone_of(const std::string& stem) { return zone_in(expedition::kCoThu, stem); }
    Zone zone(const std::string& stem) const { return zone_in(*d_, stem); }
    const expedition::Dungeon& dungeon() const { return *d_; }
    /// The other features' timed jobs kept out (B6): the boss's room and the
    /// reward's; nothing in the lobby or the waiting room.
    static TripQuiet quiet_in(Zone z);
    /// Whether the hunter's trip is kept when Tự đánh quái stops in this zone
    /// (2026.11.21, the review of the audit's B3/B20): the boss's room only.
    /// Its Fight trip -- the boss's wide area -- is set once, by the Spot job;
    /// Fighting, Loot and AfterLoot send nothing again, so a stop of GoBot's
    /// own (the bin's hold, a chat doubt) or F11 that ended it left the rest of
    /// the fight on the card's spot and area, and "outside the area = stop
    /// fighting + walk back" stopped the fight. Anywhere else the trip ends,
    /// as B3 has it: not left set for the next start.
    static bool trip_outlives_stop(Zone z) { return z == Zone::Boss; }

    enum class Step : std::uint8_t {
        Idle,
        // lobby, the leader: the ticket bought (Cổ Long)
        BuyWalk, BuyTalk, BuyShop, BuyTicket,
        // lobby, the leader
        Ready, Countdown, WalkNpc, TalkNpc, PickRow, Confirm, Watch, RetryWait, WaitEntry,
        // lobby, a member
        Member,
        // Phòng chờ
        WalkDoor, Door,
        // the boss's room
        Spot, Fighting, Loot, AfterLoot,
        // the reward's room
        WalkReward, TalkReward, Complete, Leave,
        Done,
    };

    /// The lobby's steps that wait for the pet's HP food (2026.11.24,
    /// TripSnap::pet_restock): those with nothing of the NPC's begun -- the
    /// leader's Ready (the ticket, the party), its countdown and its retry's
    /// wait, a member waiting for its leader. A walk, a talk, a row, the
    /// entry's watch already under way are not stopped half way. The wait is
    /// the planner's own as well as PetRestock's: kWaitMostMs from the first
    /// step that waited in this stay in the lobby, then on, said once.
    static bool waits_for_pet(Step s) {
        return s == Step::Ready || s == Step::Countdown || s == Step::RetryWait || s == Step::Member;
    }

    TripJob next(const TripSnap& s, const CoThuConfig& c, std::uint64_t now);
    void    done(const TripJob& j, TripResult r, const TripSnap& s, std::uint64_t now);
    void    reset();

    const std::string& status() const { return status_; }
    Step step() const { return step_; }
    int  runs() const { return runs_; }
    /// A gap of kTripRetryMs running (2026.11.24): the room's steps failed
    /// over and over (kDungeonMaxRestarts), or the leader's ticket was not
    /// bought -- in place of the stop there was. Never in the reward room:
    /// out within 30 s. The worker ends the trip meanwhile outside the boss's
    /// room, so that the hunter fights; in it the fight goes on.
    bool resting(std::uint64_t now) const { return now < rest_until_; }
    /// The leader's ticket bought -- or perhaps bought -- with no frame of bag
    /// Khác since to say so (the money guard, 2026.11.24): none is bought
    /// again until one comes, or the ticket is used.
    bool ticket_unconfirmed() const { return buy_out_; }

private:
    TripJob wait(const std::string& why);
    TripJob job(TripJobKind k, const std::string& label);
    void    enter(Zone z, const TripSnap& s, const CoThuConfig& c, std::uint64_t now);
    void    fail(const std::string& what, std::uint64_t now);
    /// A gap of `gap` from `now`: kTripRetryMs for the ticket's purchase,
    /// kRoundGapMs for the room's restarts (2026.11.24 review).
    void    rest(std::uint64_t now, std::uint64_t gap = expedition::kTripRetryMs);
    void    npc_click(TripJob& j) const;   ///< click_reward_npc: the job clicks the reward NPC
    /// The leader's ticket in bag Khác: 1 yes, 0 no, -1 the tab not read.
    int     has_ticket(const TripSnap& s) const;

    const expedition::Dungeon* d_;
    Step          step_ = Step::Idle;
    Zone          zone_ = Zone::None;
    std::string   status_;
    bool          pending_ = false;
    std::uint32_t epoch_ = 0;
    int           runs_ = 0;
    int           fails_ = 0;
    int           restarts_ = 0;
    int           rounds_ = 0;          ///< gaps after kDungeonMaxRestarts, for the log
    std::uint64_t until_ = 0;           ///< the countdown, the retry's wait, the entry's 30 s
    std::uint64_t retry_ms_ = 60000;    ///< CoThuConfig::entry_retry_seconds as the watch went out
    std::uint64_t backoff_until_ = 0;
    std::uint64_t rest_until_ = 0;      ///< a kTripRetryMs gap (resting)
    bool          boss_seen_ = false;
    std::uint64_t boss_last_ms_ = 0;
    bool          boss_dead_ = false;
    bool          entry_known_ = false;
    float         entry_x_ = 0.0f, entry_y_ = 0.0f;
    int           completes_ = 0;
    std::uint64_t reward_since_ = 0;    ///< came into the reward room
    bool          reward_cut_ = false;  ///< the reward given up for time
    int           buy_fails_ = 0;       ///< purchases of the ticket that did not go this stay: the card's "lần N", never a cap
    // The money guard (2026.11.24): bag Khác as the Buy went out; a purchase
    // that may have gone, and the frame it was judged on.
    TripPackets::Tab buy_before_;
    bool          buy_out_ = false;
    std::uint64_t buy_seq_ = 0;
    /// A purchase that came back Busy with nothing seen gained yet (2026.11.24
    /// review): the hold may have gone after Xác Nhận. The baseline taken as
    /// that Buy went out is kept -- not replaced by one that may already hold
    /// that ticket -- and a frame waited for a moment before buying again.
    bool          buy_busy_ = false;
    std::uint64_t bag_wait_since_ = 0;  ///< the lobby with bag Khác not read yet
    std::uint64_t pet_since_ = 0;       ///< this stay in the lobby waited for the pet's food since (0: not)
    bool          pet_cut_ = false;     ///< that wait over by its cap: not waited for again this stay
};
using CoThuPlanner = DungeonPlanner;

/// The dungeon run's TripQuiet, as the expedition worker last set it, for the
/// other features' workers to read (2026.11.21, the audit's B6): their timed
/// jobs -- a trash pass, the charms, the shop, a quest round, a chat line, a
/// party invite -- are put off while it is on, never dropped, and run as it
/// ends. Never a button's job, the revive, a stop, the potions, nor the popup
/// closer: a box over the game is still closed. Capped, so that nothing is
/// shut out for good: the boss's room for kBossMostMs from its entry (Cổ Long
/// lasts some 23 min; the bag fills, the potions run down), the reward's for
/// kRewardMostMs, and none at all once the worker has not set it for
/// kStaleMs -- a worker stopped or stuck. Safe from any thread.
class TripQuietFlag {
public:
    static constexpr std::uint64_t kBossMostMs = 30ull * 60'000;
    static constexpr std::uint64_t kRewardMostMs = expedition::kLeaveWithinMs + 15'000;
    static constexpr std::uint64_t kStaleMs = 30'000;
    /// A timed job put off by the boss's room goes anyway once it has waited
    /// this long. The review of B6 asked for 5 min (the shop is the only
    /// restock of the potions in a ~23 min fight); the owner chose otherwise
    /// (2026.11.21, asked "hoãn tối đa 30 phút ... có cho mua bình chen vào
    /// sớm hơn không?": "Ok"): 30 min, the shop put off with the rest.
    /// `off_since`: when the worker's current put-off began (0: not yet).
    static constexpr std::uint64_t kBossPutOffMostMs = kBossMostMs;
    static bool holds_job(TripQuiet q, std::uint64_t off_since, std::uint64_t now) {
        if (q == TripQuiet::None) return false;
        if (q != TripQuiet::Boss || off_since == 0) return true;
        return now < off_since + kBossPutOffMostMs;
    }

    /// The worker, each turn: `q` now. A kind that changes starts its cap.
    void set(TripQuiet q, std::uint64_t now);
    void clear() { set(TripQuiet::None, 0); }
    /// What holds now: None when off, past its cap or stale.
    TripQuiet at(std::uint64_t now) const;
    bool on(std::uint64_t now) const { return at(now) != TripQuiet::None; }

private:
    mutable std::mutex mutex_;
    TripQuiet     kind_ = TripQuiet::None;
    std::uint64_t since_ = 0;   ///< `kind_` set first
    std::uint64_t seen_ = 0;    ///< last set
};

} // namespace gobot
