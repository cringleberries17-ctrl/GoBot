#include "gobot/expedition.hpp"

#include "gobot/byte_reader.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace gobot {

// ============================================================ the numbers

namespace expedition {

const std::array<Mine, 4> kMines = { {
    { "t21_s71", "Mỏ Quặng Thủy Quật Hoàng Nham [1]", 225.0f, 650.0f, 1665.0f, 710.0f },
    { "t21_s72", "Mỏ Quặng Thủy Quật Hoàng Nham [2]", 380.0f, 655.0f, 1730.0f, 590.0f },
    { "t20_s71", "Mỏ Quặng Động Thạch Nhũ Xanh [1]", 600.0f, 655.0f, 2965.0f, 815.0f },
    { "t20_s72", "Mỏ Quặng Động Thạch Nhũ Xanh [2]", 765.0f, 655.0f, 265.0f, 815.0f },
} };

const Mine* mine(const std::string& stem) {
    for (const Mine& m : kMines)
        if (stem == m.stem) return &m;
    return nullptr;
}

const std::array<Pickaxe, 14> kPickaxes = { {
    { 8090111, "Cuốc Gỗ" },
    { 8090121, "Cuốc Huyền Thiết" },
    { 8090131, "Cuốc Hắc Quang" },
    { 8090141, "Cuốc Kim Cang" },
    { 8090151, "Cuốc Cực Huyết" },
    { 8090161, "Cuốc Thạch Băng" },
    { 8090171, "Cuốc Âm Dương" },
    { 8090172, "Cuốc Kỷ Niệm 8 Năm" },
    { 8090173, "Cuốc Kỷ Niệm 10 Năm" },
    { 8090181, "Cuốc Tiên Nhân" },
    { 8090182, "Cuốc Tiên Nhân" },
    { 8090183, "Cuốc Tiên Nhân" },
    { 8090191, "Cuốc Thái Hóa" },
    { 8090192, "Cuốc Thái Hóa" },
} };

bool is_pickaxe(std::uint32_t id) {
    return std::any_of(kPickaxes.begin(), kPickaxes.end(), [id](const Pickaxe& p) { return p.id == id; });
}

const char* pickaxe_name(std::uint32_t id) {
    for (const Pickaxe& p : kPickaxes)
        if (p.id == id) return p.name;
    return "";
}

bool in_mining_zone(const std::string& stem) {
    return stem == kHub || stem == kTown || stem == kTown4 || mine(stem) != nullptr;
}

const Dungeon kCoThu = {
    "CoThu", "Cổ Thụ", "Trung Tâm Rừng Đại Thụ",
    kCoThuLobby, kCoThuWait, kCoThuBoss, kCoThuReward,
    kCoThuNpcX, kCoThuNpcY,
    kCoThuDoorX, kCoThuDoorY,
    { kCoThuUpperX, kCoThuLowerX }, { kCoThuUpperY, kCoThuLowerY }, { "Tầng trên", "Tầng dưới" },
    kCoThuRewardX, kCoThuRewardY,
    kCoThuRow, "Ngọc Thụ Thần",
    kLampItem, 0, "Đèn Cấp Cao", false,
    kCoThuBossType, 20,
};

const Dungeon kCoLong = {
    "CoLong", "Cổ Long", "sảnh Ngọc Long Tổ (t60_s19)",
    kCoLongLobby, kCoLongWait, kCoLongBoss, kCoLongReward,
    kCoLongNpcX, kCoLongNpcY,
    kCoLongDoorX, kCoLongDoorY,
    { kCoLongLeftX, kCoLongRightX }, { kCoLongLeftY, kCoLongRightY }, { "Bên trái", "Bên phải" },
    kCoLongRewardX, kCoLongRewardY,
    kCoLongRow, "Ngọc Long Tổ",
    kIncenseItem, kIncenseItem2, kIncenseName, true,
    0, 40,
    "Thoát khỏi Cõi Vàng", true,
};

} // namespace expedition

using namespace expedition;

// ============================================================ the packets

namespace {
constexpr std::size_t kHeader = 8;
constexpr std::size_t kCountsAt = 300;
constexpr int kSayFirst = 6;

std::string slots_text(const std::vector<TripSlot>& s) {
    std::ostringstream o;
    int used = 0;
    for (std::size_t j = 0; j < s.size(); ++j) {
        if (s[j].item == 0) continue;
        if (used < 8) o << (used ? ", " : "") << "slot " << j << " " << s[j].item << " x" << s[j].count;
        ++used;
    }
    if (used > 8) o << ", ...";
    std::ostringstream head;
    head << used << " of " << s.size() << " slots used" << (used ? ": " : "") << o.str();
    return head.str();
}
} // namespace

std::optional<std::vector<TripSlot>> TripPackets::read_tab(const GamePacket& p, std::size_t slots, bool counts) {
    if (!p.from_server || p.udp) return std::nullopt;
    const std::size_t need = counts ? kCountsAt + 2 * slots : 12 + 4 * slots;
    if (p.payload.size() + kHeader < need) return std::nullopt;
    ByteReader r(p.payload);
    std::vector<TripSlot> out(slots);
    r.seek(12 - kHeader);
    for (std::size_t j = 0; j < slots; ++j) out[j].item = r.u32();
    if (counts) {
        r.seek(kCountsAt - kHeader);
        for (std::size_t j = 0; j < slots; ++j) out[j].count = r.u16();
    } else {
        for (TripSlot& s : out) s.count = s.item ? 1 : 0;
    }
    if (!r.ok()) return std::nullopt;
    return out;
}

std::optional<std::pair<std::uint32_t, std::int32_t>> TripPackets::read_dig(const GamePacket& p) {
    if (p.opcode != kDig || !p.from_server) return std::nullopt;
    if (p.payload.size() + kHeader != 20) return std::nullopt;
    ByteReader r(p.payload);
    r.seek(12 - kHeader);
    const std::uint32_t id = r.u32();
    const std::int32_t value = r.i32();
    if (!r.ok()) return std::nullopt;
    return std::make_pair(id, value);
}

void TripPackets::on_packets(const std::vector<GamePacket>& all) {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const GamePacket& p : all) {
            Tab* tab = nullptr;
            std::size_t slots = kTabSlots;
            bool counts = true;
            const char* name = "";
            switch (p.opcode) {
                case kWorn:
                    tab = &worn_;
                    slots = kWornSlots;
                    counts = false;
                    name = "0x0065 (worn)";
                    break;
                case kEquip1:
                    tab = &equip1_;
                    name = "0x0066 (Trang bị 1)";
                    break;
                case kOther:
                    tab = &other_;
                    name = "0x0069 (Khác)";
                    break;
                case 0x0086:    // a map entry: digging is over, as GhostCore has it
                case 0x0014:
                    digs_.clear();
                    continue;
                default:
                    break;
            }
            if (tab) {
                const auto s = read_tab(p, slots, counts);
                if (!s) {
                    if (said_ < kSayFirst) {
                        ++said_;
                        char b[160];
                        std::snprintf(b, sizeof b, "Trip packets: a %s frame not read: %zu bytes, %s, depth %u", name,
                                      p.payload.size() + kHeader, p.from_server ? "from the server" : "from the client",
                                      static_cast<unsigned>(p.depth));
                        lines.emplace_back(b);
                    }
                    continue;
                }
                const bool first = !tab->known;
                tab->known = true;
                tab->slots = *s;
                tab->at_ms = p.timestamp_ms;
                ++tab->seq;
                if (first && said_ < kSayFirst) {
                    ++said_;
                    lines.push_back(std::string("Trip packets: first ") + name + ", " +
                                    std::to_string(p.payload.size() + kHeader) + " B: " + slots_text(*s));
                }
                continue;
            }
            if (p.opcode != kDig) continue;
            const auto d = read_dig(p);
            if (!d) {
                if (said_ < kSayFirst) {
                    ++said_;
                    lines.push_back("Trip packets: a 0x0132 not read: " + std::to_string(p.payload.size() + kHeader) +
                                    " bytes");
                }
                continue;
            }
            Dig& dig = digs_[d->first];
            if (d->second == 1) {
                dig.state = 2;
            } else if (d->second == -1) {
                dig.state = 1;
            } else {
                dig.last_item = static_cast<std::uint32_t>(d->second);
                if (dig.state == 0) dig.state = 2;
            }
            dig.seq = ++dig_seq_;
            dig.at_ms = p.timestamp_ms;
            newest_digger_ = d->first;
            if (said_ < kSayFirst) {
                ++said_;
                lines.push_back("Trip packets: 0x0132 for character " + std::to_string(d->first) + ": value " +
                                std::to_string(d->second));
            }
        }
    }
    for (const std::string& l : lines) GB_INFO("%s", l.c_str());
}

void TripPackets::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    worn_ = equip1_ = other_ = Tab{};
    digs_.clear();
    newest_digger_ = 0;
}

TripPackets::Tab TripPackets::worn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return worn_;
}
TripPackets::Tab TripPackets::equip1() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return equip1_;
}
TripPackets::Tab TripPackets::other() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return other_;
}

TripPackets::Dig TripPackets::dig(std::optional<std::uint32_t> id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = digs_.find(id ? *id : newest_digger_);
    return it == digs_.end() ? Dig{} : it->second;
}

// ============================================================ the settings

namespace {
std::string hex_key(std::uint32_t vk) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%02X", vk);
    return b;
}
std::uint32_t key_of(const IniFile& ini, const char* sec, const char* key, std::uint32_t def) {
    const std::string v = ini.get(sec, key);
    if (v.empty()) return def;
    try {
        return static_cast<std::uint32_t>(std::stoul(v, nullptr, 0));
    } catch (...) {
        return def;
    }
}
} // namespace

void MiningConfig::load(const IniFile& ini) {
    const char* s = "Mining";
    enabled = ini.get_bool(s, "enabled", enabled);
    const std::string m = ini.get(s, "map_key", map_key);
    if (mine(m)) map_key = m;
    auto_replace = ini.get_bool(s, "auto_replace_pickaxe", auto_replace);
    auto_buy = ini.get_bool(s, "auto_buy_pickaxe", auto_buy);
    const std::uint32_t p = key_of(ini, s, "buy_pickaxe_id", buy_pickaxe);
    if (is_pickaxe(p)) buy_pickaxe = p;
    buy_when = std::clamp(ini.get_int(s, "buy_when_pickaxes", buy_when), 0, 20);
    buy_quantity = std::clamp(ini.get_int(s, "buy_quantity", buy_quantity), 1, 20);
    talk_key = key_of(ini, s, "talk_key", talk_key);
}

void MiningConfig::store(IniFile& ini) const {
    const char* s = "Mining";
    ini.set_bool(s, "enabled", enabled);
    ini.set(s, "map_key", map_key);
    ini.set_bool(s, "auto_replace_pickaxe", auto_replace);
    ini.set_bool(s, "auto_buy_pickaxe", auto_buy);
    ini.set_int(s, "buy_pickaxe_id", static_cast<int>(buy_pickaxe));
    ini.set_int(s, "buy_when_pickaxes", buy_when);
    ini.set_int(s, "buy_quantity", buy_quantity);
    ini.set(s, "talk_key", hex_key(talk_key));
}

void CoThuConfig::load(const IniFile& ini, const char* s) {
    enabled = ini.get_bool(s, "enabled", enabled);
    role = std::clamp(ini.get_int(s, "role", role), 0, 1);
    minimum_members = std::clamp(ini.get_int(s, "minimum_party_members", minimum_members), 1, 6);
    wait_seconds = std::clamp(ini.get_int(s, "boss_wait_seconds", wait_seconds), 0, 600);
    boss_position = std::clamp(ini.get_int(s, "boss_position", boss_position), 0, 2);
    entry_retry = ini.get_bool(s, "entry_retry_enabled", entry_retry);
    entry_retry_seconds = std::clamp(ini.get_int(s, "entry_retry_seconds", entry_retry_seconds), 5, 600);
    maximum_runs = std::clamp(ini.get_int(s, "maximum_runs", maximum_runs), 0, 999);
    loot_presses = std::clamp(ini.get_int(s, "loot_presses", loot_presses), 0, 60);
    talk_key = key_of(ini, s, "talk_key", talk_key);
    loot_key = key_of(ini, s, "loot_key", loot_key);
}

void CoThuConfig::store(IniFile& ini, const char* s) const {
    ini.set_bool(s, "enabled", enabled);
    ini.set_int(s, "role", role);
    ini.set_int(s, "minimum_party_members", minimum_members);
    ini.set_int(s, "boss_wait_seconds", wait_seconds);
    ini.set_int(s, "boss_position", boss_position);
    ini.set_bool(s, "entry_retry_enabled", entry_retry);
    ini.set_int(s, "entry_retry_seconds", entry_retry_seconds);
    ini.set_int(s, "maximum_runs", maximum_runs);
    ini.set_int(s, "loot_presses", loot_presses);
    ini.set(s, "talk_key", hex_key(talk_key));
    ini.set(s, "loot_key", hex_key(loot_key));
}

bool ExpeditionSettings::load(const std::string& path) {
    IniFile ini;
    const bool ok = ini.load(path);
    mining.load(ini);
    cothu.load(ini, "CoThu");
    colong.load(ini, "CoLong");
    return ok;
}

bool ExpeditionSettings::save(const std::string& path) const {
    IniFile ini;
    mining.store(ini);
    cothu.store(ini, "CoThu");
    colong.store(ini, "CoLong");
    return ini.save(path, "GoBot: Đào khoáng ([Mining]), phó bản Cổ Thụ ([CoThu]) and Cổ Long ([CoLong]), as GoC "
                          "0.6.5 does them");
}

// ============================================================ the job's words

const char* trip_job_name(TripJobKind k) {
    switch (k) {
        case TripJobKind::None:          return "none";
        case TripJobKind::Walk:          return "walk";
        case TripJobKind::Fight:         return "fight";
        case TripJobKind::Gate:          return "gate";
        case TripJobKind::Talk:          return "talk";
        case TripJobKind::PickRow:       return "pick row";
        case TripJobKind::ClickOk:       return "click OK";
        case TripJobKind::WatchRefusal:  return "watch refusal";
        case TripJobKind::StartDig:      return "start digging";
        case TripJobKind::ExitDig:       return "exit dig mode";
        case TripJobKind::Equip:         return "equip";
        case TripJobKind::Buy:           return "buy";
        case TripJobKind::CloseNotice:   return "close notice";
        case TripJobKind::Loot:          return "loot";
        case TripJobKind::CompleteQuest: return "complete quest";
        case TripJobKind::ExitRoom:      return "leave the room";
    }
    return "?";
}

const char* trip_result_name(TripResult r) {
    switch (r) {
        case TripResult::Ok:      return "ok";
        case TripResult::Retry:   return "retry";
        case TripResult::Busy:    return "busy";
        case TripResult::Refused: return "refused";
    }
    return "?";
}

bool tab_gained(const TripPackets::Tab& before, const TripPackets::Tab& now) {
    if (!now.known) return false;
    for (std::size_t j = 0; j < now.slots.size(); ++j) {
        const TripSlot& n = now.slots[j];
        if (n.item == 0) continue;
        const TripSlot b = before.known && j < before.slots.size() ? before.slots[j] : TripSlot{};
        // Another item in the slot -- the purchase's under an id GoBot does
        // not know (VTC's Lư Hương may not be GoC's 8990188 or 8991014) --
        // or more of the one there.
        if (n.item != b.item || n.count > b.count) return true;
    }
    return false;
}

// ============================================================ an NPC's shop

std::string shop_list_text(const std::string& name) {
    // A space is one byte in UTF-8, never inside a letter's bytes.
    std::string out;
    for (const char c : name)
        if (c != ' ') out.push_back(c);
    return out;
}

QuantityKeys quantity_keys(int have, int want) {
    QuantityKeys k;
    if (want < 1 || have < 0) return k;
    k.ok = true;
    if (have == want) return k;
    const std::string target = std::to_string(want);
    if (have == 0) {
        k.digits = target;
        return k;
    }
    // Typed at its caret, after what it holds (BagCleaner::shop_purchase: the
    // 1 the game puts there and "00" typed make 100).
    const std::string shown = std::to_string(have);
    if (shown.size() < target.size() && target.compare(0, shown.size(), shown) == 0) {
        k.digits = target.substr(shown.size());
        return k;
    }
    // One Backspace more than its digits, as the potion shop erases.
    k.erase = static_cast<int>(shown.size()) + 1;
    k.digits = target;
    return k;
}

BuyLook buy_look(const BuyLookIn& in) {
    // The bag first: the item came, however the game took the double-click.
    if (in.more_in_bag) return BuyLook::Bought;
    if (in.full) return BuyLook::Full;
    if (in.dialogs > 1) return BuyLook::Other;
    if (in.dialogs == 1) {
        if (in.known) return BuyLook::Other;
        // A box GoBot cannot tell for the shop's own is never typed into, nor
        // a question in another frame -- an invitation, a trade -- confirmed.
        if (in.box) return in.question ? BuyLook::Question : BuyLook::Other;
        if (in.bare) return in.question ? BuyLook::Confirm : BuyLook::Other;
        return BuyLook::Nothing;
    }
    return in.message ? BuyLook::Message : BuyLook::Nothing;
}

const char* buy_look_name(BuyLook l) {
    switch (l) {
        case BuyLook::Nothing:  return "nothing";
        case BuyLook::Bought:   return "bought";
        case BuyLook::Question: return "the quantity question";
        case BuyLook::Confirm:  return "a question with no box";
        case BuyLook::Full:     return "the full-bag message";
        case BuyLook::Message:  return "a message";
        case BuyLook::Other:    return "another dialog";
    }
    return "?";
}

// ============================================================ mining

namespace {
constexpr std::uint64_t kBackoffMs = 3000;
constexpr std::uint64_t kBusyMs = 500;
/// A ticket's purchase come back Busy with nothing seen gained (2026.11.24
/// review): the moment given to bag Khác's frame before the next Buy -- the
/// shop's own kShopBagFrameMs.
constexpr std::uint64_t kBusyBuyFrameMs = 5000;
constexpr std::uint64_t kRewardBackoffMs = 300;
/// The map change after a gate's ↓ or a Teleporter's row: 30 s since
/// 2026.11.24 (15 s before) -- the owner's wait for laggy PCs. A slow load of
/// the Teleporter's map taken for a failure was its row picked again, and
/// paid for again.
constexpr std::uint64_t kMapWaitMs = 30000;
constexpr int kMaxFails = 3;
constexpr int kMaxSkips = 16;

/// The pickaxes a tab holds, as pickaxes_in_bag counts them.
int pickaxes_of(const TripPackets::Tab& t) {
    if (!t.known) return 0;
    int n = 0;
    for (const TripSlot& s : t.slots)
        if (is_pickaxe(s.item)) n += std::max<int>(1, s.count);
    return n;
}
} // namespace

void MiningPlanner::reset() {
    *this = MiningPlanner{};
}

void MiningPlanner::rest(std::uint64_t now, const std::string& why, std::uint64_t gap) {
    backoff_until_ = rest_until_ = now + gap;
    status_ = why;
    GB_INFO("Mining: %s -- a %llu s gap, the hunter fighting meanwhile, then again (no limit)", why.c_str(),
            static_cast<unsigned long long>(gap / 1000));
}

TripJob MiningPlanner::wait(const std::string& why) {
    status_ = why;
    TripJob j;
    j.label = why;
    return j;
}

TripJob MiningPlanner::job(TripJobKind k, const std::string& label) {
    status_ = label;
    pending_ = true;
    TripJob j;
    j.kind = k;
    j.label = label;
    return j;
}

TripJob MiningPlanner::walk(const std::string& stem, float x, float y, int tx, int ty, const std::string& label) {
    TripJob j = job(TripJobKind::Walk, label);
    j.stem = stem;
    j.x = x;
    j.y = y;
    j.tol_x = tx;
    j.tol_y = ty;
    return j;
}

std::optional<MiningPlanner::Route> MiningPlanner::route(const std::string& from, const std::string& to) {
    if (from == to) return std::nullopt;
    Route r;
    if (from == kTown4) {
        r.x = kTown4GateX;
        r.y = kTown4GateY;
        r.label = "Đến cổng sang Thanh Âm Quan";
        return r;
    }
    if (from == kTown) {
        if (to == kTown4) {
            r.x = kTownGateX;
            r.y = kTownGateY;
            r.label = "Đến cổng";
            return r;
        }
        r.x = kTownTeleX;
        r.y = kTownTeleY;
        r.teleporter = true;
        r.row = kToHubRow;
        r.label = "Đến Teleporter (đi Mỏ Quặng Thủy Quật)";
        return r;
    }
    if (from == kHub) {
        if (const Mine* m = mine(to)) {
            r.x = m->entry_x;
            r.y = m->entry_y;
            r.label = "Đến cổng vào mỏ";
            return r;
        }
        r.x = kHubTeleX;
        r.y = kHubTeleY;
        r.teleporter = true;
        r.row = kToTownRow;
        r.label = "Đến Teleporter (về Thanh Âm Quan)";
        return r;
    }
    if (const Mine* m = mine(from)) {
        r.x = m->exit_x;
        r.y = m->exit_y;
        r.label = "Đến cổng ra khỏi mỏ";
        return r;
    }
    return std::nullopt;
}

std::optional<float> MiningPlanner::dig_spot(const TripSnap& s, const Mine& m, int dir, int skip,
                                             const std::vector<float>& avoid) {
    const int me_x = static_cast<int>(std::lround(s.x));
    const int me_y = static_cast<int>(std::lround(s.y));
    int found = 0;
    // Where you stand first (GoC starts a step away: it has a server to say a
    // spot is no good; standing still costs nothing), then a step either way
    // in turn, `dir` first.
    for (int k = 0; k <= kSpotSteps; ++k) {
        for (int side = 0; side < (k == 0 ? 1 : 2); ++side) {
            const int d = side == 0 ? dir : -dir;
            const int x = me_x + k * d * kSpotStepPx;
            if (x < 32) continue;
            if (std::abs(static_cast<float>(x) - m.exit_x) < static_cast<float>(kGateClearPx)) continue;
            // A spot where digging did not start: not that one again.
            if (std::any_of(avoid.begin(), avoid.end(), [x](float a) {
                    return std::abs(static_cast<float>(x) - a) < static_cast<float>(kSpotStepPx / 2);
                }))
                continue;
            bool clear = true;
            for (const auto& p : s.players)
                if (std::abs(static_cast<float>(x) - p.first) < kPlayerClearPx &&
                    std::abs(static_cast<float>(me_y) - p.second) < kPlayerClearPx)
                    clear = false;
            if (!clear) continue;
            if (s.same_floor && !s.same_floor(me_x, me_y, x)) continue;
            if (found++ == skip) return static_cast<float>(x);
        }
    }
    return std::nullopt;
}

bool MiningPlanner::wears_pickaxe(const TripSnap& s) {
    if (!s.worn.known) return false;
    return std::any_of(s.worn.slots.begin(), s.worn.slots.end(), [](const TripSlot& t) { return is_pickaxe(t.item); });
}

int MiningPlanner::pickaxes_in_bag(const TripSnap& s) {
    return pickaxes_of(s.equip1);
}

int MiningPlanner::first_pickaxe_slot(const TripSnap& s) {
    if (!s.equip1.known) return -1;
    for (std::size_t j = 0; j < s.equip1.slots.size(); ++j)
        if (is_pickaxe(s.equip1.slots[j].item)) return static_cast<int>(j);
    return -1;
}

TripJob MiningPlanner::toward(const TripSnap& s, const std::string& to, const char* why) {
    const auto r = route(s.stem, to);
    if (!r) return wait(std::string("Không có đường từ ") + s.stem + " tới " + to);
    phase_ = r->teleporter ? Phase::TalkTele : Phase::Gate;
    TripJob j = walk(s.stem, r->x, r->y, kArriveTolX, kArriveTolY, std::string(r->label) + " -- " + why);
    j.text = r->row;
    return j;
}

TripJob MiningPlanner::next(const TripSnap& s, const MiningConfig& c, std::uint64_t now) {
    if (pending_) return wait(status_);
    if (!c.enabled) {
        // Unticked: everything forgotten, a gap with it, to start afresh.
        if (!stem_.empty()) reset();
        return wait("Đào khoáng: chưa bật");
    }
    if (!s.world || s.stem.empty()) return wait("Đào khoáng: chờ gói tin cho biết map và chỗ đứng");
    if (!in_mining_zone(s.stem)) {
        phase_ = Phase::Plan;
        on_spot_ = false;
        return wait("Đào khoáng: cần đứng ở map thuộc khu đào khoáng (Thanh Âm Quan, Mỏ Quặng Thủy Quật, các mỏ)");
    }
    // A map entry: everything about the last map goes (GoC: phase 1, the job
    // cancelled).
    if (s.stem != stem_ || s.epoch != epoch_) {
        if (!stem_.empty()) GB_INFO("Mining: now on %s (map entry %u)", s.stem.c_str(), static_cast<unsigned>(s.epoch));
        stem_ = s.stem;
        epoch_ = s.epoch;
        phase_ = Phase::Plan;
        on_spot_ = false;
        walking_spot_ = false;
        skips_ = 0;
        map_fails_ = 0;
        no_dig_x_.clear();
    }
    if (s.hp_known && s.hp == 0) return wait("Đào khoáng: nhân vật đã chết, chờ hồi sinh");
    if (now < backoff_until_) return wait(status_);

    switch (phase_) {
        case Phase::Gate: {
            phase_ = Phase::WaitMap;
            TripJob j = job(TripJobKind::Gate, "Vào cổng");
            j.count = 12;
            return j;
        }
        case Phase::TalkTele: {
            phase_ = Phase::PickTele;
            TripJob j = job(TripJobKind::Talk, "Nói chuyện với Teleporter");
            j.item = c.talk_key;
            return j;
        }
        case Phase::PickTele: {
            phase_ = Phase::WaitMap;
            TripJob j = job(TripJobKind::PickRow, "Chọn điểm đến của Teleporter");
            j.text = s.stem == kTown ? kToHubRow : kToTownRow;
            j.confirm = true;
            return j;
        }
        case Phase::WaitMap:
            if (wait_since_ == 0) wait_since_ = now;
            if (now - wait_since_ > kMapWaitMs) {
                wait_since_ = 0;
                phase_ = Phase::Plan;
                backoff_until_ = now + kBackoffMs;
                // Counted apart from fails_ -- the talk and the row before
                // each wait went, which clears that -- and until a map entry
                // (2026.11.24): the third a gap, then again; never a stop.
                if (++map_fails_ >= kMaxFails) {
                    map_fails_ = 0;
                    rest(now, "Không chuyển được map sau khi vào cổng hoặc chọn Teleporter (3 lần) -- thử lại sau 5 "
                              "giây",
                         kRoundGapMs);
                    return wait(status_);
                }
                return wait("Không chuyển được map sau khi vào cổng hoặc chọn Teleporter -- thử lại");
            }
            return wait("Chờ chuyển map");
        case Phase::TalkMiner: {
            phase_ = Phase::PickShop;
            TripJob j = job(TripJobKind::Talk, "Nói chuyện với Thợ Mỏ");
            j.item = c.talk_key;
            return j;
        }
        case Phase::PickShop: {
            phase_ = Phase::Buy;
            TripJob j = job(TripJobKind::PickRow, "Chọn Shop của Thợ Mỏ");
            j.text = kShopRowText;
            return j;
        }
        case Phase::Buy: {
            phase_ = Phase::Plan;
            // What came of it is judged against the bag as it goes out (the
            // money guard, 2026.11.24).
            pick_before_ = s.equip1;
            pick_worn_ = wears_pickaxe(s);
            TripJob j = job(TripJobKind::Buy, std::string("Mua ") + pickaxe_name(c.buy_pickaxe));
            j.item = c.buy_pickaxe;
            j.text = pickaxe_name(c.buy_pickaxe);
            j.count = c.buy_quantity;
            return j;
        }
        case Phase::Plan:
            break;
    }

    // ---- the planner (GoC 0x1400FC160), first rule that applies ----
    const bool worn = wears_pickaxe(s);
    if (s.worn.known && worn) {
        was_worn_ = true;
        broken_ = false;
    }
    const int in_bag = pickaxes_in_bag(s);
    const int at_hand = (worn ? 1 : 0) + in_bag;
    // The money guard (2026.11.24). Three purchases that did not go stopped
    // Đào khoáng until it was unticked; now a purchase that did not go is
    // tried again kTripRetryMs on, with no limit -- and one that went, or
    // may have (the shop's "no frame of the bag came to say": counted bought,
    // never bought twice; Trang bị 1 holding more of anything than as the Buy
    // went out), is not made again until a frame of Trang bị 1 says what came
    // of it: more pickaxes than then (bought, seen), a newer frame with
    // nothing new in it (nothing came: buying again), or a pickaxe worn that
    // was not then (one put on). A pickaxe worn all along clears nothing:
    // with "Mua khi còn" over 0 one is worn while more are bought.
    if (pick_out_) {
        const bool more = pickaxes_of(s.equip1) > pickaxes_of(pick_before_);
        // "Nothing new" is about pickaxes (2026.11.24 review): their ids are
        // GoBot's own (is_pickaxe), and any other item come into Trang bị 1
        // meanwhile -- a drop picked up on the trip -- kept the guard on for
        // ever, rule 5 waiting with no pickaxe left.
        const bool nothing = s.equip1.known && s.equip1.seq > pick_seq_ && !more;
        const bool put_on = worn && !pick_worn_;
        if (more || nothing || put_on) {
            pick_out_ = false;
            GB_INFO("Mining: %s -- buying as the pickaxes at hand say again",
                    more      ? "the pickaxe bought is in Trang bị 1"
                    : nothing ? "Trang bị 1 came again with nothing new since the purchase: it did not go"
                              : "a pickaxe is worn now");
        }
    }
    const bool need_buy = c.auto_buy && s.equip1.known && s.worn.known && at_hand <= c.buy_when && !pick_out_;

    // 1. The pickaxe was worn and is not now: it broke. Its notice closed.
    if (s.worn.known && !worn && was_worn_) {
        was_worn_ = false;
        broken_ = true;
        notice_closed_ = false;
        on_spot_ = false;
        GB_INFO("Mining: the pickaxe is no longer worn (0x0065) -- it broke");
    }
    if (!notice_closed_) return job(TripJobKind::CloseNotice, "Đóng thông báo cuốc đã bị phá hủy");

    // 2. Digging: idle, unless something else is due.
    if (s.dig.state == 2 && (worn || !s.worn.known)) {
        if (!need_buy) return wait("Đang đào khoáng");
        return job(TripJobKind::ExitDig, "Thoát chế độ đào khoáng (để mua cuốc)");
    }

    // 3. Pickaxes to buy: Thợ Mỏ on the hub.
    if (need_buy) {
        if (s.stem != kHub) return toward(s, kHub, "đi mua cuốc");
        phase_ = Phase::TalkMiner;
        return walk(s.stem, kMinerX, kMinerY, kArriveTolX, kArriveTolY, "Đến Thợ Mỏ");
    }

    // 4. Not in the mine yet.
    if (s.stem != c.map_key) return toward(s, c.map_key, "đi tới mỏ đã chọn");
    const Mine* m = mine(s.stem);
    if (m == nullptr) return wait("Đào khoáng: không biết mỏ " + s.stem);

    // 5. In the mine with no pickaxe worn. What is missing is waited for --
    //    no key -- and looked at again each turn, the settings re-read every
    //    second: a pickaxe put in Trang bị 1, the box ticked, and it goes on
    //    by itself (2026.11.24; each of these stopped Đào khoáng until it was
    //    unticked, Trang bị 1 not yet read taken for none left).
    if (s.worn.known && !worn) {
        if (!s.equip1.known) return wait("Đào khoáng: chờ gói tin túi Trang bị 1 (xem còn cuốc không)");
        if (in_bag == 0) {
            if (pick_out_) return wait("Đã mua cuốc -- chờ gói tin túi Trang bị 1 để biết cuốc ở ô nào (không mua thêm)");
            // Tự mua cuốc on went buying (rule 3): it is off.
            return wait("Đào khoáng: hết cuốc -- bỏ cuốc vào Trang bị 1 hoặc bật Tự mua cuốc; GoBot đào tiếp khi thấy "
                        "cuốc");
        }
        if (broken_ && !c.auto_replace)
            return wait("Cuốc đã hỏng, Tự động thay thế cuốc đang tắt -- chờ bạn trang bị cuốc (hoặc bật ô đó)");
        TripJob j = job(TripJobKind::Equip, std::string("Trang bị ") + pickaxe_name(s.equip1.slots[first_pickaxe_slot(s)].item));
        j.slot = first_pickaxe_slot(s);
        j.item = s.equip1.slots[static_cast<std::size_t>(j.slot)].item;
        return j;
    }

    // 6. On a spot still clear: dig.
    if (on_spot_ && std::abs(s.x - spot_x_) <= 2.0f * kSpotTolX && std::abs(s.y - spot_y_) <= kSpotTolY) {
        dig_seq_at_start_ = s.dig.seq;
        TripJob j = job(TripJobKind::StartDig, "Nhấn đánh thường để vào chế độ đào");
        j.item = c.talk_key;
        return j;
    }
    // 7. A spot to dig at -- none where digging did not start. None at all
    //    (2026.11.24): looked for again after a gap, the other way first and
    //    every spot afresh (a player gone, a dig that failed for a moment);
    //    it stopped Đào khoáng before.
    const auto x = dig_spot(s, *m, dir_, skips_, no_dig_x_);
    if (!x) {
        skips_ = 0;
        dir_ = -dir_;
        no_dig_x_.clear();
        rest(now, "Chưa tìm được chỗ đào cách cổng 192 px và người chơi 64 px -- thử lại sau 5 giây", kRoundGapMs);
        return wait(status_);
    }
    walking_spot_ = true;
    want_x_ = *x;
    char b[96];
    std::snprintf(b, sizeof b, "Dịch chuyển đến chỗ đào (x %d)", static_cast<int>(*x));
    return walk(s.stem, *x, s.y, kSpotTolX, kSpotTolY, b);
}

void MiningPlanner::done(const TripJob& j, TripResult r, const TripSnap& s, std::uint64_t now) {
    pending_ = false;
    GB_INFO("Mining: %s -- %s (%s)", trip_job_name(j.kind), trip_result_name(r), j.label.c_str());
    const bool spot_walk = j.kind == TripJobKind::Walk && walking_spot_;
    walking_spot_ = false;
    // The money guard (2026.11.24): bought, or Trang bị 1 holding more than
    // as the Buy went out -- whatever the shop said, Busy too (a hold lost
    // after the purchase) -- may have bought: none again until a frame says.
    if (j.kind == TripJobKind::Buy && (r == TripResult::Ok || tab_gained(pick_before_, s.equip1))) {
        pick_out_ = true;
        pick_seq_ = s.equip1.seq;
        GB_INFO("Mining: the purchase %s -- no pickaxe bought again until Trang bị 1 says what came of it (frame %llu)",
                r == TripResult::Ok ? "went" : "said no, but Trang bị 1 holds more than before it",
                static_cast<unsigned long long>(pick_seq_));
    }
    switch (r) {
        case TripResult::Ok:
            // A walk there is no progress past what failed after it: a talk
            // failing after each good walk would otherwise go on for ever.
            if (j.kind != TripJobKind::Walk) fails_ = 0;
            if (spot_walk) {
                on_spot_ = true;
                spot_x_ = s.x;
                spot_y_ = s.y;
                dir_ = -dir_;
                skips_ = 0;
            }
            if (j.kind == TripJobKind::CloseNotice) notice_closed_ = true;
            if (j.kind == TripJobKind::Equip || j.kind == TripJobKind::StartDig) was_worn_ = true;
            if (j.kind == TripJobKind::StartDig) no_dig_x_.clear();
            if (phase_ == Phase::WaitMap) wait_since_ = now;
            return;
        case TripResult::Busy:
            if (j.kind == TripJobKind::Walk || j.kind == TripJobKind::Talk || j.kind == TripJobKind::PickRow ||
                j.kind == TripJobKind::Gate || j.kind == TripJobKind::Buy)
                phase_ = Phase::Plan;
            backoff_until_ = now + kBusyMs;
            return;
        case TripResult::Refused:
        case TripResult::Retry:
            phase_ = Phase::Plan;
            backoff_until_ = now + kBackoffMs;
            // None of these stops Đào khoáng any more (2026.11.24): each of
            // them did, until it was unticked.
            if (spot_walk) {
                // kMaxSkips spots not reached: a gap, then the other way.
                if (++skips_ > kMaxSkips) {
                    skips_ = 0;
                    dir_ = -dir_;
                    rest(now, "Không đi tới được chỗ đào -- thử lại sau 5 giây", kRoundGapMs);
                }
                return;
            }
            // The shop's trouble -- Thợ Mỏ's Shop row, the purchase -- tried
            // again after the owner's 30 s (bag::kShopTroubleAgainMs), not 3.
            // One the money guard holds (Trang bị 1 gained something): no
            // purchase follows, so no gap promising one (2026.11.24 review).
            if (j.kind == TripJobKind::Buy) {
                if (!pick_out_)
                    rest(now, std::string("Chưa mua được ") + pickaxe_name(j.item) + " ở Thợ Mỏ -- thử lại sau 30 giây");
                return;
            }
            if (j.kind == TripJobKind::PickRow && j.text == kShopRowText) {
                rest(now, "Chưa mở được Shop của Thợ Mỏ -- thử lại sau 30 giây");
                return;
            }
            // Digging that did not start: another spot next, never that one
            // again until none is left.
            if (j.kind == TripJobKind::StartDig) {
                on_spot_ = false;
                no_dig_x_.push_back(spot_x_);
            }
            if (++fails_ >= kMaxFails) {
                fails_ = 0;
                rest(now, "Đào khoáng: " + j.label + " thất bại 3 lần -- thử lại sau 5 giây", kRoundGapMs);
            }
            return;
    }
}

// ============================================================ Cổ Thụ, Cổ Long

namespace {
constexpr std::uint64_t kEntryWaitMs = 30000;
/// A room's steps from the start this many times in one run, and a gap of
/// kTripRetryMs before the next round of them (2026.11.24; a stop before).
constexpr int kDungeonMaxRestarts = 5;
/// The lobby with bag Khác not read: GoC waits for it ("Chờ dữ liệu hành
/// trang"); GoBot goes on after this, in case the frame never comes mid-game.
constexpr std::uint64_t kBagWaitMs = 10000;
} // namespace

DungeonPlanner::Zone DungeonPlanner::zone_in(const Dungeon& d, const std::string& stem) {
    if (stem == d.lobby) return Zone::Lobby;
    if (stem == d.wait) return Zone::Wait;
    if (stem == d.boss) return Zone::Boss;
    if (stem == d.reward) return Zone::Reward;
    return Zone::None;
}

void DungeonPlanner::reset() {
    const int runs = runs_;
    const Dungeon* d = d_;
    *this = DungeonPlanner{ *d };
    runs_ = runs;
}

void DungeonPlanner::rest(std::uint64_t now, std::uint64_t gap) {
    backoff_until_ = rest_until_ = now + gap;
}

TripJob DungeonPlanner::wait(const std::string& why) {
    status_ = why;
    TripJob j;
    j.label = why;
    return j;
}

TripJob DungeonPlanner::job(TripJobKind k, const std::string& label) {
    status_ = label;
    pending_ = true;
    TripJob j;
    j.kind = k;
    j.label = label;
    return j;
}

int DungeonPlanner::has_ticket(const TripSnap& s) const {
    if (!s.other.known) return -1;
    for (const TripSlot& t : s.other.slots)
        if (t.count > 0 && t.item != 0 && (t.item == d_->ticket || t.item == d_->ticket2)) return 1;
    return 0;
}

void DungeonPlanner::enter(Zone z, const TripSnap& s, const CoThuConfig& c, std::uint64_t now) {
    const Zone was = zone_;
    zone_ = z;
    fails_ = 0;
    entry_known_ = false;
    // Another room: its own steps at once (2026.11.24). A gap is the last
    // room's -- its steps failed over and over -- and the boss's room or the
    // reward's (out within 30 s) never waits out the rest of one.
    backoff_until_ = rest_until_ = 0;
    if (was == Zone::Lobby && z == Zone::Wait) {
        ++runs_;
        // A run under way: the restarts of the runs before it are no longer
        // held against this one (2026.11.21, the audit's B5). Counted for the
        // night, five rare stalls -- walks run out under a trash pass's hold,
        // say -- stopped Phó bản until it was unticked.
        restarts_ = 0;
        // The ticket used (2026.11.24): the guard over its purchase goes.
        buy_out_ = false;
        buy_busy_ = false;
        GB_INFO("%s: into the waiting room -- run %d", d_->tag, runs_);
    }
    switch (z) {
        case Zone::Lobby:
            buy_fails_ = 0;
            buy_out_ = false;
            buy_busy_ = false;
            bag_wait_since_ = 0;
            // A new stay: the pet's food waited for afresh (2026.11.24).
            pet_since_ = 0;
            pet_cut_ = false;
            step_ = c.role == 0 ? Step::Ready : Step::Member;
            break;
        case Zone::Wait:
            step_ = Step::WalkDoor;
            break;
        case Zone::Boss:
            boss_seen_ = false;
            boss_last_ms_ = 0;
            step_ = boss_dead_ ? Step::Loot : Step::Spot;
            break;
        case Zone::Reward:
            boss_dead_ = false;
            completes_ = 0;
            reward_since_ = now;
            reward_cut_ = false;
            // Cổ Long: the NPC clicked from where you come in, no walk.
            step_ = d_->click_reward_npc ? Step::TalkReward : Step::WalkReward;
            break;
        case Zone::None:
            step_ = Step::Idle;
            break;
    }
    (void)s;
}

void DungeonPlanner::npc_click(TripJob& j) const {
    if (!d_->click_reward_npc) return;
    j.by_click = true;
    j.stem = d_->reward;
    j.x = d_->reward_x;
    j.y = d_->reward_y;
}

void DungeonPlanner::fail(const std::string& what, std::uint64_t now) {
    // The reward room: out within kLeaveWithinMs, so no 3 s waits, and the
    // way out tried again and again, never the reward's steps once on it.
    if (zone_ == Zone::Reward) {
        // Past kLeaveWithinMs and still in (2026.11.21, the audit's B11): the
        // way out goes on being tried, never given up -- but kBackoffMs apart,
        // not 0.3 s: each try holds the keys and the mouse 1-3 s, and with
        // 0.3 s between them the macro's buffs, the popups, chat and the
        // trash pass hardly ever had the game for as long as the room
        // stayed.
        const bool late = reward_since_ != 0 && now >= reward_since_ + kLeaveWithinMs;
        backoff_until_ = now + (late ? kBackoffMs : kRewardBackoffMs);
        if (step_ == Step::Leave || reward_cut_) {
            step_ = Step::Leave;
            return;
        }
        // The reward's steps failing over and over (2026.11.24): the NPC
        // walked to again -- never a restart counted, never a gap of
        // kTripRetryMs. The room is left within 30 s (the owner's rule), the
        // way out never given up; counted, the fifth restart of the run (the
        // waiting room's walks, say, and these) stopped Phó bản in the room.
        if (++fails_ < kMaxFails) return;
        fails_ = 0;
        GB_INFO("%s: %s failed 3 times in the reward room -- to the NPC again (no gap: out within %llu s)", d_->tag,
                what.c_str(), static_cast<unsigned long long>(kLeaveWithinMs / 1000));
        step_ = reward_cut_ ? Step::Leave : Step::WalkReward;
        return;
    }
    backoff_until_ = now + kBackoffMs;
    if (++fails_ < kMaxFails) return;
    fails_ = 0;
    if (++restarts_ >= kDungeonMaxRestarts) {
        // A round of restarts that did not go (2026.11.24): a gap of
        // kRoundGapMs -- the worker ends the trip outside the boss's room,
        // the hunter fighting meanwhile -- then the room's steps from the
        // start again, with no limit. It stopped Phó bản until it was
        // unticked; a few seconds, not the shop's 30 s (the review of
        // 2026.11.24: "no long pause as a replacement for a stop").
        restarts_ = 0;
        ++rounds_;
        rest(now, kRoundGapMs);
        status_ = std::string(d_->name) + ": " + what + " chưa được sau nhiều lần -- thử lại từ đầu sau 5 giây";
        GB_INFO("%s: %s failed %d times over -- a %llu s gap, then the room's steps from the start (round %d, no limit)",
                d_->tag, what.c_str(), kMaxFails * kDungeonMaxRestarts,
                static_cast<unsigned long long>(kRoundGapMs / 1000), rounds_);
    } else {
        GB_INFO("%s: %s failed 3 times -- the room's steps from the start (%d)", d_->tag, what.c_str(), restarts_);
    }
    // From the zone's start again.
    switch (zone_) {
        case Zone::Lobby:  step_ = step_ == Step::Member ? Step::Member : Step::Ready; break;
        case Zone::Wait:   step_ = Step::WalkDoor; break;
        case Zone::Boss:   step_ = boss_dead_ ? Step::Loot : Step::Spot; break;
        case Zone::Reward: step_ = Step::WalkReward; break;
        case Zone::None:   step_ = Step::Idle; break;
    }
}

TripJob DungeonPlanner::next(const TripSnap& s, const CoThuConfig& c, std::uint64_t now) {
    const std::string name = d_->name;
    if (pending_) return wait(status_);
    if (!c.enabled) {
        if (zone_ != Zone::None) reset();
        return wait(name + ": chưa bật");
    }
    // Nothing stops the planner for good since 2026.11.24 -- a refusal with
    // Tự thử lại unticked did (and was undone when the party moved in all the
    // same, 2026.11.21, the audit's B7): a refusal is tried again after
    // entry_retry_seconds, whatever the old setting says.
    if (!s.world || s.stem.empty()) return wait(name + ": chờ gói tin cho biết map và chỗ đứng");
    const Zone z = zone(s.stem);
    if (z == Zone::None) {
        zone_ = Zone::None;
        step_ = Step::Idle;
        return wait(name + ": đang tắt hoặc chưa ở khu vực phó bản (" + d_->lobby_name + ")");
    }
    if (z != zone_ || s.epoch != epoch_) {
        epoch_ = s.epoch;
        if (z != zone_) enter(z, s, c, now);
    }
    // Where the boss room put you, for "where you come in".
    if (z == Zone::Boss && !entry_known_ && s.settled) {
        entry_known_ = true;
        entry_x_ = s.x;
        entry_y_ = s.y;
    }
    if (s.hp_known && s.hp == 0) {
        boss_seen_ = false;
        return wait("Nhân vật đã chết, chờ hồi sinh về phòng chờ");
    }
    // Too long over the reward: given up, straight to the way out.
    // Still walking to the NPC: the walk goes on (the way out is the NPC's
    // too), and from it straight to leaving.
    if (zone_ == Zone::Reward && !reward_cut_ && step_ != Step::Leave && now >= reward_since_ + kRewardGiveUpMs) {
        reward_cut_ = true;
        if (step_ != Step::WalkReward) step_ = Step::Leave;
        backoff_until_ = 0;
        GB_INFO("%s: %llu s in the reward room -- the reward given up, leaving now (out within %llu s)", d_->tag,
                static_cast<unsigned long long>((now - reward_since_) / 1000),
                static_cast<unsigned long long>(kLeaveWithinMs / 1000));
    }
    // The pet's HP food bought on reaching the lobby (2026.11.24, the owner:
    // "Khi ra tới bản đồ TổCổLong thì cần mua lại HP cho pet"; PetRestock):
    // the lobby's steps with nothing of the NPC's begun wait while its round
    // is owed or under way -- PetRestock::kWaitMostMs at most from the first
    // of them that waited this stay, then on without it, said once: the
    // dungeon is never held up for long, and nothing is stopped for it.
    if (zone_ == Zone::Lobby && s.pet_restock && !pet_cut_ && waits_for_pet(step_)) {
        if (pet_since_ == 0) {
            pet_since_ = now;
            GB_INFO("%s: in the lobby with the pet's HP food to buy -- waiting for it, %llu s at most", d_->tag,
                    static_cast<unsigned long long>(PetRestock::kWaitMostMs / 1000));
        }
        if (now < pet_since_ + PetRestock::kWaitMostMs) {
            char b[160];
            std::snprintf(b, sizeof b, "Chờ mua HP pet (Thịt bò) xong rồi mới đi tiếp (tối đa %d giây nữa)",
                          static_cast<int>((pet_since_ + PetRestock::kWaitMostMs - now + 999) / 1000));
            return wait(b);
        }
        pet_cut_ = true;
        GB_INFO("%s: waited %llu s in the lobby for the pet's HP food -- going on without it", d_->tag,
                static_cast<unsigned long long>((now - pet_since_) / 1000));
    }
    if (now < backoff_until_) return wait(status_);

    switch (step_) {
        case Step::Idle:
        case Step::Done:
            return wait(status_.empty() ? name + ": chờ" : status_);

        // ---- the lobby, the leader's ticket
        case Step::BuyWalk: {
            TripJob j = job(TripJobKind::Walk, std::string("Đang chạy tới NPC mua ") + d_->ticket_name);
            j.stem = d_->lobby;
            j.x = d_->npc_x;
            j.y = d_->npc_y;
            return j;
        }
        case Step::BuyTalk: {
            TripJob j = job(TripJobKind::Talk, std::string("Đang nói chuyện với NPC để mua ") + d_->ticket_name);
            j.item = c.talk_key;
            return j;
        }
        case Step::BuyShop: {
            TripJob j = job(TripJobKind::PickRow, "Đang chọn Shop của NPC");
            j.text = kShopRowText;
            return j;
        }
        case Step::BuyTicket: {
            // What came of it is judged against bag Khác as it goes out (the
            // money guard, 2026.11.24). After a Busy (buy_busy_) the baseline
            // of that Buy is kept: the hold may have gone after Xác Nhận, and
            // a baseline taken now may already hold that ticket -- a frame
            // gained since then is that purchase, and none is made again.
            if (buy_busy_ && tab_gained(buy_before_, s.other)) {
                buy_busy_ = false;
                buy_out_ = true;
                buy_seq_ = s.other.seq;
                step_ = Step::Ready;
                GB_INFO("%s: bag Khác gained something since the purchase that came back busy -- taken as bought, "
                        "none bought again until a frame says otherwise", d_->tag);
                return wait(std::string("Đã mua ") + d_->ticket_name + " (túi Khác có thêm) -- không mua thêm");
            }
            if (!buy_busy_) buy_before_ = s.other;
            TripJob j = job(TripJobKind::Buy, std::string("Đang mua 1 ") + d_->ticket_name);
            j.item = d_->ticket;
            j.item2 = d_->ticket2;
            j.other_tab = true;
            j.text = d_->ticket_name;
            j.count = 1;
            return j;
        }

        // ---- the lobby, the leader
        case Step::Ready: {
            if (c.maximum_runs > 0 && runs_ >= c.maximum_runs) return wait("Đã đi đủ số lượt đã cài");
            const int t = has_ticket(s);
            // The ticket bought -- or perhaps bought -- and not in bag Khác as
            // GoBot knows it (the money guard, 2026.11.24): three purchases
            // that did not go stopped Phó bản, and the purchase agent's own
            // retries could buy more than one in a visit when VTC's ticket is
            // under an id GoBot does not know. Now none is bought again until
            // a newer frame of Khác says nothing came of the last (buying
            // again); until then the leader goes on as with the tab unread --
            // the party, the countdown, the entry -- and the NPC says if it is
            // missing.
            bool unconfirmed = false;
            if (t == 0) {
                if (!d_->buys_ticket)
                    return wait(std::string("Đội trưởng không có ") + d_->ticket_name +
                                " trong túi đồ, hãy bỏ thêm vào túi");
                if (buy_out_ && s.other.seq > buy_seq_ && !tab_gained(buy_before_, s.other)) {
                    buy_out_ = false;
                    GB_INFO("%s: bag Khác came again (frame %llu) with nothing new since the purchase -- it did not go; "
                            "buying again",
                            d_->tag, static_cast<unsigned long long>(s.other.seq));
                }
                if (!buy_out_) {
                    GB_INFO("%s: no %s in bag Khác -- buying one", d_->tag, d_->ticket_name);
                    step_ = Step::BuyWalk;
                    return next(s, c, now);
                }
                unconfirmed = true;
            }
            if (t < 0 && d_->buys_ticket) {
                if (bag_wait_since_ == 0) bag_wait_since_ = now;
                if (now - bag_wait_since_ < kBagWaitMs)
                    return wait(std::string("Chờ gói tin túi Khác (xem có ") + d_->ticket_name + " không)");
                // Not read after all: go on, and the NPC will say.
            }
            const int total = s.party > 0 ? s.party : (c.minimum_members <= 1 ? 1 : -1);
            if (total < 0) return wait(name + ": chưa đọc được tổ đội trên màn hình");
            if (total < c.minimum_members) {
                char b[128];
                std::snprintf(b, sizeof b, "Tổ đội đang có %d người (cần tối thiểu %d)", total, c.minimum_members);
                return wait(b);
            }
            until_ = now + static_cast<std::uint64_t>(c.wait_seconds) * 1000;
            step_ = Step::Countdown;
            GB_INFO("%s: the party is %d (minimum %d) -- going in after %d s%s", d_->tag, total, c.minimum_members,
                    c.wait_seconds, unconfirmed ? " (the ticket bought, not yet seen in bag Khác: none bought again)" : "");
            if (unconfirmed)
                return wait(std::string("Đã mua ") + d_->ticket_name +
                            " nhưng chưa có gói tin túi Khác xác nhận -- không mua thêm; vào phó bản, game sẽ báo nếu "
                            "thiếu");
            return wait("Đủ người -- chờ trước khi vào phó bản");
        }
        case Step::Countdown: {
            const int total = s.party > 0 ? s.party : (c.minimum_members <= 1 ? 1 : -1);
            if (total >= 0 && total < c.minimum_members) {
                step_ = Step::Ready;
                return wait("Tổ đội không còn đủ người");
            }
            if (now < until_) {
                char b[96];
                std::snprintf(b, sizeof b, "Bắt đầu vào phó bản sau %d giây",
                              static_cast<int>((until_ - now + 999) / 1000));
                return wait(b);
            }
            step_ = Step::WalkNpc;
            return next(s, c, now);
        }
        case Step::WalkNpc: {
            TripJob j = job(TripJobKind::Walk, "Đang chạy tới NPC vào phó bản");
            j.stem = d_->lobby;
            j.x = d_->npc_x;
            j.y = d_->npc_y;
            return j;
        }
        case Step::TalkNpc: {
            TripJob j = job(TripJobKind::Talk, "Đang nói chuyện với NPC");
            j.item = c.talk_key;
            return j;
        }
        case Step::PickRow: {
            TripJob j = job(TripJobKind::PickRow, std::string("Đang chọn ") + d_->row_name + " để vào phó bản");
            j.text = d_->row;
            return j;
        }
        case Step::Confirm: {
            TripJob j = job(TripJobKind::ClickOk, "Đang xác nhận vào phó bản");
            j.count = d_->ok_looks;
            return j;
        }
        case Step::Watch:
            retry_ms_ = static_cast<std::uint64_t>(c.entry_retry_seconds) * 1000;
            return job(TripJobKind::WatchRefusal, "Đang chờ vào phó bản");
        case Step::RetryWait:
            if (now < until_) {
                char b[96];
                std::snprintf(b, sizeof b, "Không vào được phó bản, thử lại sau %d giây",
                              static_cast<int>((until_ - now + 999) / 1000));
                return wait(b);
            }
            step_ = Step::Ready;
            return wait("Thử vào phó bản lại");
        case Step::WaitEntry:
            if (now >= until_) {
                step_ = Step::Ready;
                // The round's gap, when this failure ends one, says so.
                status_ = "Chưa vào được phòng chờ sau 30 giây -- thử lại";
                fail("vào phó bản", now);
                return wait(status_);
            }
            return wait("Đang chờ vào phòng chờ");

        // ---- the lobby, a member
        case Step::Member:
            return wait("Đã chuẩn bị xong, đang chờ đội trưởng đưa vào phó bản");

        // ---- the waiting room
        case Step::WalkDoor: {
            TripJob j = job(TripJobKind::Walk, "Đang chạy tới cửa vào phòng boss");
            j.stem = d_->wait;
            j.x = d_->door_x;
            j.y = d_->door_y;
            return j;
        }
        case Step::Door: {
            TripJob j = job(TripJobKind::Gate, "Đang đi vào phòng boss");
            j.count = 34;
            return j;
        }

        // ---- the boss
        case Step::Spot: {
            TripJob j = job(TripJobKind::Fight, "Đang chạy tới chỗ đứng đánh boss");
            j.stem = d_->boss;
            if (c.boss_position == 0 || c.boss_position == 1) {
                j.x = d_->spot_x[c.boss_position];
                j.y = d_->spot_y[c.boss_position];
            } else if (entry_known_) {
                j.x = entry_x_;
                j.y = entry_y_;
            } else {
                pending_ = false;
                return wait("Đang xác định chỗ đứng đánh boss");
            }
            j.tol_x = 48;
            j.tol_y = 31;
            return j;
        }
        case Step::Fighting: {
            // Cổ Long: no boss known -- GoC fights until the game moves you on.
            if (d_->boss_type == 0) return wait("Đang đánh boss; chờ được đưa sang phòng nhận thưởng");
            bool alive = false;
            for (const TripMob& m : s.mobs)
                if (m.type == d_->boss_type && !m.dead && m.hp > 0) alive = true;
            if (alive) {
                if (!boss_seen_) GB_INFO("%s: the boss (type %u) is seen", d_->tag, d_->boss_type);
                boss_seen_ = true;
                boss_last_ms_ = now;
                return wait("Đang đánh boss");
            }
            if (boss_seen_ && now - boss_last_ms_ >= kBossGoneMs) {
                GB_INFO("%s: the boss has been gone %llu ms -- dead", d_->tag,
                        static_cast<unsigned long long>(now - boss_last_ms_));
                boss_dead_ = true;
                step_ = Step::Loot;
                return wait("Boss đã chết, đang nhặt đồ");
            }
            return wait(boss_seen_ ? "Đang đánh boss" : "Bắt đầu đánh boss (chưa thấy boss)");
        }
        case Step::Loot: {
            if (c.loot_presses <= 0) {
                step_ = Step::AfterLoot;
                return wait("Đã nhặt đồ xong, chờ được đưa sang phòng nhận thưởng");
            }
            TripJob j = job(TripJobKind::Loot, "Boss đã chết, đang nhặt đồ");
            j.count = c.loot_presses;
            j.item = c.loot_key;
            return j;
        }
        case Step::AfterLoot:
            return wait("Đã nhặt đồ xong, chờ được đưa sang phòng nhận thưởng");

        // ---- the reward
        case Step::WalkReward: {
            TripJob j = job(TripJobKind::Walk, "Đang chạy tới NPC nhận thưởng");
            j.stem = d_->reward;
            j.x = d_->reward_x;
            j.y = d_->reward_y;
            j.max_ms = 12000;   // out of the room in 30 s: no 65 s walk
            return j;
        }
        case Step::TalkReward: {
            TripJob j = job(TripJobKind::Talk, d_->click_reward_npc ? "Đang bấm vào NPC nhận thưởng"
                                                                     : "Đang nói chuyện với NPC nhận thưởng");
            j.item = c.talk_key;
            npc_click(j);
            return j;
        }
        case Step::Complete: {
            TripJob j = job(TripJobKind::CompleteQuest, "Đang nhận thưởng nhiệm vụ");
            j.item = c.talk_key;   // pressed when the button is not seen
            return j;
        }
        case Step::Leave: {
            TripJob j = job(TripJobKind::ExitRoom, "Đang rời phó bản");
            j.item = c.talk_key;
            j.count = 40;
            if (d_->leave_row) j.text = d_->leave_row;
            j.max_ms = 8000;    // then asked again, the way out row looked for afresh
            npc_click(j);
            return j;
        }
    }
    return wait(status_);
}

void DungeonPlanner::done(const TripJob& j, TripResult r, const TripSnap& s, std::uint64_t now) {
    pending_ = false;
    GB_INFO("%s: %s -- %s (%s)", d_->tag, trip_job_name(j.kind), trip_result_name(r), j.label.c_str());
    // The leader's ticket (2026.11.24). The money guard first: bought, or
    // bag Khác holding more than as the Buy went out -- under any id: VTC's
    // Lư Hương may not be GoC's -- whatever the shop said, Busy too (a hold
    // lost after the purchase), may have bought: none again until a frame of
    // Khác says (Ready). Then the ticket looked for again; a purchase that did
    // not go tried again after the owner's 30 s for the shop's trouble
    // (bag::kShopTroubleAgainMs), with no limit -- three of them stopped Phó
    // bản until it was unticked.
    if (step_ == Step::BuyTicket) {
        if (r == TripResult::Ok || tab_gained(buy_before_, s.other)) {
            buy_out_ = true;
            buy_seq_ = s.other.seq;
            GB_INFO("%s: the purchase %s -- no %s bought again until bag Khác says what came of it (frame %llu)",
                    d_->tag, r == TripResult::Ok ? "went" : "said no, but bag Khác holds more than before it",
                    d_->ticket_name, static_cast<unsigned long long>(buy_seq_));
        }
        if (r == TripResult::Busy) {
            // Nothing begun -- asked again soon; one that bought before its
            // hold went is looked at from Ready. Not seen gained yet (the
            // 2026.11.24 review): the Buy's baseline kept (buy_busy_) and a
            // moment for bag Khác's frame before the next Buy.
            if (buy_out_) {
                buy_busy_ = false;
                step_ = Step::Ready;
                backoff_until_ = now + kBusyMs;
            } else {
                buy_busy_ = true;
                backoff_until_ = now + kBusyBuyFrameMs;
            }
            return;
        }
        buy_busy_ = false;
        step_ = Step::Ready;
        if (r != TripResult::Ok && !buy_out_) {
            ++buy_fails_;
            rest(now);
            status_ = std::string("Chưa mua được ") + d_->ticket_name + " (lần " + std::to_string(buy_fails_) +
                      ") -- thử lại sau 30 giây";
            GB_INFO("%s: %s not bought (%d this stay) -- tried again in %llu s, no limit", d_->tag, d_->ticket_name,
                    buy_fails_, static_cast<unsigned long long>(kTripRetryMs / 1000));
        } else if (r != TripResult::Ok) {
            // The shop said no, but bag Khác gained something (the money
            // guard holds): no purchase follows, so no 30 s promising one --
            // on at once, unconfirmed (the 2026.11.24 review).
            status_ = std::string("Đã mua ") + d_->ticket_name + " nhưng chưa có gói tin túi Khác xác nhận";
        }
        return;
    }
    if (r == TripResult::Busy) {
        backoff_until_ = now + kBusyMs;
        return;
    }
    const bool ok = r == TripResult::Ok;
    switch (step_) {
        case Step::BuyWalk:  if (ok) step_ = Step::BuyTalk; else fail(j.label, now); break;
        case Step::BuyTalk:  if (ok) step_ = Step::BuyShop; else fail(j.label, now); break;
        case Step::BuyShop:
            if (ok) step_ = Step::BuyTicket;
            else { step_ = Step::BuyTalk; fail(j.label, now); }
            break;
        case Step::WalkNpc:  if (ok) step_ = Step::TalkNpc; else fail(j.label, now); break;
        case Step::TalkNpc:  if (ok) step_ = Step::PickRow; else fail(j.label, now); break;
        case Step::PickRow:
            if (ok) step_ = Step::Confirm;
            else { step_ = Step::TalkNpc; fail(j.label, now); }
            break;
        case Step::Confirm:
            if (ok) step_ = Step::Watch;
            else { step_ = Step::TalkNpc; fail(j.label, now); }
            break;
        case Step::Watch:
            if (r == TripResult::Refused) {
                // Tried again after entry_retry_seconds, always (2026.11.24:
                // with Tự thử lại unticked it stopped Phó bản for good).
                step_ = Step::RetryWait;
                until_ = now + retry_ms_;
            } else {
                step_ = Step::WaitEntry;
                until_ = now + kEntryWaitMs;
            }
            break;
        case Step::WalkDoor: if (ok) step_ = Step::Door; else fail(j.label, now); break;
        case Step::Door:
            // Still here after its presses: walk to the door again.
            step_ = Step::WalkDoor;
            if (!ok) fail(j.label, now);
            break;
        case Step::Spot:     if (ok) step_ = Step::Fighting; else fail(j.label, now); break;
        case Step::Loot:     step_ = Step::AfterLoot; break;
        case Step::WalkReward:
            if (ok) step_ = reward_cut_ ? Step::Leave : Step::TalkReward;
            else fail(j.label, now);
            break;
        case Step::TalkReward: if (ok) step_ = Step::Complete; else fail(j.label, now); break;
        case Step::Complete:
            if (ok || ++completes_ >= 3) step_ = Step::Leave;
            else step_ = Step::TalkReward;
            break;
        case Step::Leave:
            if (!ok) fail(j.label, now);
            break;
        default:
            break;
    }
}

// ============================================ the other features (B6)

const char* trip_quiet_name(TripQuiet q) {
    switch (q) {
        case TripQuiet::Boss:   return "boss fight";
        case TripQuiet::Reward: return "reward room";
        case TripQuiet::None:   break;
    }
    return "none";
}

const char* trip_quiet_vi(TripQuiet q) {
    switch (q) {
        case TripQuiet::Boss:   return "đang đánh boss phó bản";
        case TripQuiet::Reward: return "đang ở phòng nhận thưởng phó bản (phải rời trong 30 giây)";
        case TripQuiet::None:   break;
    }
    return "";
}

TripQuiet DungeonPlanner::quiet_in(Zone z) {
    switch (z) {
        case Zone::Boss:   return TripQuiet::Boss;
        case Zone::Reward: return TripQuiet::Reward;
        case Zone::None:
        case Zone::Lobby:
        case Zone::Wait:   break;
    }
    return TripQuiet::None;
}

void TripQuietFlag::set(TripQuiet q, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (q != kind_) since_ = now;
    kind_ = q;
    seen_ = now;
}

TripQuiet TripQuietFlag::at(std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (kind_ == TripQuiet::None) return TripQuiet::None;
    // A worker stopped or stuck sets it no more: off.
    if (now >= seen_ + kStaleMs) return TripQuiet::None;
    const std::uint64_t most = kind_ == TripQuiet::Boss ? kBossMostMs : kRewardMostMs;
    if (now >= since_ + most) return TripQuiet::None;
    return kind_;
}

// ============================================ the pet's food (2026.11.24)

bool PetRestock::look(const std::string& stem, std::uint32_t epoch, bool armed, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Not in the world, or no entry yet: nothing to go by.
    if (stem.empty() || epoch == 0 || epoch == looked_) return false;
    looked_ = epoch;
    // Another entry: whatever the last one owed is over -- the lobby left, or
    // come into again, which owes afresh.
    owed_ = 0;
    arrived_ms_ = again_ms_ = 0;
    tried_ = false;
    if (stem != expedition::kCoLongLobby || !armed) return false;
    owed_ = epoch;
    arrived_ms_ = now;
    return true;
}

std::uint32_t PetRestock::owed(std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owed_ != 0 && now < arrived_ms_ + kWaitMostMs ? owed_ : 0;
}

std::uint32_t PetRestock::due(std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owed_ != 0 && now < arrived_ms_ + kWaitMostMs && now >= again_ms_ ? owed_ : 0;
}

void PetRestock::put_off(std::uint32_t epoch, std::uint64_t at_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch != 0 && epoch == owed_) again_ms_ = at_ms;
}

void PetRestock::over(std::uint32_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch != 0 && epoch == owed_) {
        owed_ = 0;
        arrived_ms_ = again_ms_ = 0;
        tried_ = false;
    }
}

void PetRestock::tried(std::uint32_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch != 0 && epoch == owed_) tried_ = true;
}

bool PetRestock::expire(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owed_ == 0 || now < arrived_ms_ + kWaitMostMs) return false;
    owed_ = 0;
    arrived_ms_ = again_ms_ = 0;
    tried_ = false;
    return true;
}

bool PetRestock::waits(std::uint32_t epoch, std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch != 0 && epoch == owed_ && !tried_ && now < arrived_ms_ + kWaitMostMs;
}

std::uint64_t PetRestock::arrived_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owed_ != 0 ? arrived_ms_ : 0;
}

} // namespace gobot
