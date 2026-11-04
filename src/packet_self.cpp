#include "gobot/packet_self.hpp"
#include "gobot/logger.hpp"
#include "gobot/own_position.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>

namespace gobot {
namespace {

constexpr std::size_t kHeader = 8;      // frame bytes before the payload

/// The opcodes listed by PacketSelf::opcodes(), in its order.
constexpr std::uint16_t kOpcodes[] = { 0x0051, 0x0050, 0x02F0, 0x0068, 0x006B,
                                       0x0077, 0x005A, 0x002E, 0x0014, 0x03C6 };
static_assert(std::size(kOpcodes) == PacketSelf::kOpcodeCount, "kOpcodeCount counts kOpcodes");

std::size_t opcode_index(std::uint16_t opcode) {
    for (std::size_t k = 0; k < std::size(kOpcodes); ++k)
        if (kOpcodes[k] == opcode) return k;
    return std::size(kOpcodes);
}

/// The kinds whose bursts make a world entry: wrapped 0x001E, 0x0042, 0x0051
/// and 0x0050, in that order; 4 for none of them.
std::size_t burst_index(std::uint16_t opcode) {
    switch (opcode) {
        case 0x001E: return 0;
        case 0x0042: return 1;
        case 0x0051: return 2;
        case 0x0050: return 3;
        default:     return 4;
    }
}

bool server_tcp(const GamePacket& p) { return p.from_server && !p.udp; }

PacketUse use_of(const IniFile& ini, const char* key) {
    std::string v = ini.get("Packets", key, "auto");
    std::string low;
    for (const char c : v)
        if (!std::isspace(static_cast<unsigned char>(c)))
            low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low == "off" || low == "false" || low == "no" || low == "0" ? PacketUse::Off : PacketUse::Auto;
}

/// Where a frame came from, for its first-frame line.
const char* side_text(const GamePacket& p) {
    if (!p.from_server) return p.udp ? "your client, UDP" : "your client, TCP";
    if (p.udp) return p.depth != 0 ? "server UDP, inside 0x0081" : "server UDP";
    return p.depth != 0 ? "inside 0x0081" : "plain";
}

std::string hex_of(const Bytes& b, std::size_t most) {
    std::string out;
    char buf[4];
    const std::size_t n = std::min(b.size(), most);
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", static_cast<unsigned>(b[i]));
        if (i) out += ' ';
        out += buf;
    }
    if (b.size() > most) out += " ... (" + std::to_string(b.size() - most) + " more)";
    return out;
}

/// What a first frame of a decoded kind says, by its fields only.
std::string decoded_text(const GamePacket& p) {
    if (p.opcode == 0x0050) {
        if (const auto s = read_stat_sheet(p))
            return "max HP " + std::to_string(s->max_hp) + ", HP " + std::to_string(s->hp) + ", max MP " +
                   std::to_string(s->max_mp) + ", MP " + std::to_string(s->mp);
    } else if (p.opcode == 0x0068) {
        if (const auto bag = read_bag(p)) {
            std::size_t used = 0;
            std::string slots;
            for (std::size_t j = 0; j < bag->size(); ++j) {
                const BagSlotFrame& s = (*bag)[j];
                if (s.item == 0) continue;
                if (++used <= 12)
                    slots += (slots.empty() ? "" : ", ") + std::string("slot ") + std::to_string(j) + " " +
                             std::to_string(s.item) + " x" + std::to_string(s.count);
            }
            std::string out = std::to_string(used) + " of " + std::to_string(kBagSlots) + " slots used";
            if (!slots.empty()) out += ": " + slots + (used > 12 ? ", ..." : "");
            return out;
        }
    }
    return "not of the recorded size: not read";
}

} // namespace

const char* packet_use_name(PacketUse use) { return use == PacketUse::Off ? "off" : "auto"; }

PacketsSettings packets_settings(const IniFile& app_ini) {
    PacketsSettings s;
    s.vitals = use_of(app_ini, "vitals");
    s.bag = use_of(app_ini, "bag");
    s.detector_rest = use_of(app_ini, "detector_rest");
    return s;
}

std::string describe_packets_settings(const PacketsSettings& s) {
    return std::string("vitals ") + packet_use_name(s.vitals) + ", bag " + packet_use_name(s.bag) +
           ", detector_rest " + packet_use_name(s.detector_rest);
}

PacketSelf::Stats::Frames PacketSelf::Stats::of(std::uint16_t opcode) const {
    const std::size_t k = opcode_index(opcode);
    if (k < frames.size()) return frames[k];
    Frames none;
    none.opcode = opcode;
    return none;
}

PacketSelf::PacketSelf() {
    for (std::size_t k = 0; k < kOpcodeCount; ++k) stats_.frames[k].opcode = kOpcodes[k];
}

const std::vector<std::uint16_t>& PacketSelf::opcodes() {
    static const std::vector<std::uint16_t> all(std::begin(kOpcodes), std::end(kOpcodes));
    return all;
}

PacketSelf::FirstFrame PacketSelf::first_frame_kind(std::uint16_t opcode) {
    switch (opcode) {
        // Their bytes: none of these is known to carry a name.
        case 0x0051: case 0x02F0: case 0x006B: case 0x0031: case 0x004A:
        case 0x004D: case 0x004B: case 0x005A: case 0x0077: case 0x0026:
        case 0x010D:
            return FirstFrame::Hex;
        // Their fields only: the stat sheet and the bag are long, and read.
        case 0x0050: case 0x0068:
            return FirstFrame::Decoded;
        // 0x0014 (a character selected), 0x001E (your own record) and 0x0065
        // (a snapshot of your character) carry its name; nothing else is
        // written either.
        default:
            return FirstFrame::Never;
    }
}

void PacketSelf::on_packets(const std::vector<GamePacket>& all, std::uint64_t start_count) {
    std::vector<First> firsts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Another start of the capture, whoever started it: nothing kept from
        // before it goes by anything until a frame brings it again.
        if (start_count != start_count_) {
            start_count_ = start_count;
            ++epoch_;
            ++stats_.starts_seen;
        }
        for (const GamePacket& p : all) take(p, firsts);
    }
    // Written outside the lock, on the capture thread: a line or two per kind
    // per game process.
    for (const First& f : firsts) {
        const GamePacket& p = f.packet;
        char head[96];
        std::snprintf(head, sizeof head, "Packets: first 0x%04X (%s, %zu B): ", static_cast<unsigned>(p.opcode),
                      side_text(p), p.payload.size() + kHeader);
        const std::string body =
            first_frame_kind(p.opcode) == FirstFrame::Decoded ? decoded_text(p) : hex_of(p.payload, 256);
        Logger::instance().write(LogLevel::Info, head + body);
    }
}

void PacketSelf::count(std::uint16_t opcode, FrameFit fit) {
    const std::size_t k = opcode_index(opcode);
    if (k >= stats_.frames.size()) return;
    Stats::Frames& f = stats_.frames[k];
    switch (fit) {
        case FrameFit::Taken:       ++f.used; break;
        case FrameFit::WrongSide:   ++f.wrong_side; break;
        case FrameFit::WrongSize:   ++f.wrong_size; break;
        case FrameFit::OtherOpcode: break;
    }
}

void PacketSelf::burst_kind(std::size_t k, std::uint64_t t) {
    if (k >= burst_ms_.size()) return;
    burst_ms_[k] = t;
    check_burst();
}

void PacketSelf::check_burst() {
    if (link_.last_entry_ms == 0 || burst_counted_) return;
    int kinds = 0;
    for (const std::uint64_t at : burst_ms_) {
        if (at == 0) continue;
        const std::uint64_t apart = at > link_.last_entry_ms ? at - link_.last_entry_ms : link_.last_entry_ms - at;
        if (apart <= kBurstMs) ++kinds;
    }
    if (kinds >= 2) {
        ++link_.bursts;
        burst_counted_ = true;
    }
}

void PacketSelf::take(const GamePacket& p, std::vector<First>& firsts) {
    const std::uint64_t t = p.timestamp_ms;
    if (server_tcp(p) && t > link_.last_server_ms) link_.last_server_ms = t;

    // The first frames of a kind, once per game process: from the server, or
    // your client's own datagrams. Never your client's TCP, whose "opcode" is
    // a check word and whose content is ciphered.
    const FirstFrame said = first_frame_kind(p.opcode);
    if (said != FirstFrame::Never && (p.from_server || p.udp)) {
        auto it = std::find_if(firsts_said_.begin(), firsts_said_.end(),
                               [&p](const std::pair<std::uint16_t, int>& e) { return e.first == p.opcode; });
        if (it == firsts_said_.end()) {
            firsts_said_.emplace_back(p.opcode, 0);
            it = firsts_said_.end() - 1;
        }
        if (it->second < kFirstFrames) {
            ++it->second;
            firsts.push_back(First{ p });
        }
    }

    // A world entry's frames, by their kind and time alone.
    if (server_tcp(p) && p.depth != 0) burst_kind(burst_index(p.opcode), t);

    FrameFit fit = FrameFit::OtherOpcode;
    switch (p.opcode) {
    case 0x0051:
        if (const auto v = read_self_vitals(p, &fit)) {
            Vitals& s = vitals_;
            // Above a maximum the stat sheet gave: counted, and the maximum
            // is raised all the same -- it is never below the HP.
            if (s.max_from == MaxFrom::StatSheet && v->hp > s.max_hp) ++s.over_max;
            s.hp = v->hp;
            s.mp = v->mp;
            s.hp_ms = s.mp_ms = t;
            s.hp_epoch = s.mp_epoch = epoch_;
            if (v->hp > s.max_hp || v->mp > s.max_mp) {
                s.max_hp = std::max(s.max_hp, v->hp);
                s.max_mp = std::max(s.max_mp, v->mp);
                s.max_ms = t;
                if (s.max_from == MaxFrom::None) s.max_from = MaxFrom::Seen;
            }
            s.points = ++points_taken_;
            history_[history_n_++ % kHistory] = VitalsPoint{ t, v->hp, v->mp, s.points };
        }
        break;
    case 0x0050:
        if (const auto sheet = read_stat_sheet(p, &fit)) {
            Vitals& s = vitals_;
            // The sheet's maxima are the game's: a buff that wore off lowers
            // them. One of 0 is no maximum.
            if (sheet->max_hp > 0 && sheet->max_mp > 0) {
                s.max_hp = sheet->max_hp;
                s.max_mp = sheet->max_mp;
                s.max_ms = t;
                s.max_from = MaxFrom::StatSheet;
                if (sheet->hp > sheet->max_hp) ++s.over_max;
            }
            // Its HP and MP are those of the moment, as 0x0051's.
            s.hp = sheet->hp;
            s.mp = sheet->mp;
            s.hp_ms = s.mp_ms = t;
            s.hp_epoch = s.mp_epoch = epoch_;
            s.max_hp = std::max(s.max_hp, s.hp);
            s.max_mp = std::max(s.max_mp, s.mp);
            s.points = ++points_taken_;
            history_[history_n_++ % kHistory] = VitalsPoint{ t, sheet->hp, sheet->mp, s.points };
        }
        break;
    case 0x02F0:
        if (const auto mp = read_udp_mp(p, &fit)) {
            vitals_.udp_mp = *mp;
            vitals_.udp_mp_ms = t;
        }
        break;
    case 0x0068:
        if (const auto slots = read_bag(p, &fit)) {
            bag_.known = true;
            bag_.slots = *slots;
            bag_.at_ms = t;
            bag_.epoch = epoch_;
            bag_seen_[bag_seen_n_++ % kBagFrames] = BagSeen{ t, *slots };
        }
        break;
    case 0x006B:
        if (const auto gold = read_gold(p, &fit)) {
            bag_.gold = *gold;
            bag_.gold_ms = t;
        }
        break;
    case 0x03C6:
        // The server's tick: plain, a 4-byte body, once a second.
        if (!server_tcp(p) || p.depth != 0) {
            fit = FrameFit::WrongSide;
        } else if (p.payload.size() + kHeader != 12) {
            fit = FrameFit::WrongSize;
        } else {
            fit = FrameFit::Taken;
            ++link_.ticks;
            link_.last_tick_ms = t;
            tick_times_[tick_n_++ % kTickTimes] = t;
        }
        break;
    case 0x0014:
        // Inside 0x0081 a character selected, plain a channel switch: only
        // counted, never read -- it carries the character's name.
        if (!server_tcp(p)) {
            fit = FrameFit::WrongSide;
        } else {
            fit = FrameFit::Taken;
            ++(p.depth != 0 ? link_.selected : link_.channel);
        }
        break;
    case 0x0086:
        if (read_map_entry(p)) {
            ++link_.entries;
            link_.last_entry_ms = t;
            burst_counted_ = false;
            check_burst();
        }
        break;
    case 0x0077:
    case 0x005A:
    case 0x002E:
        // Read by the casts' and buffs' part (Slice 5); only counted here.
        fit = FrameFit::Taken;
        break;
    default:
        break;
    }
    count(p.opcode, fit);
}

void PacketSelf::note_press(std::uint32_t vk, std::uint64_t at_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    presses_[presses_n_++ % kPresses] = Press{ vk, at_ms };
}

void PacketSelf::mark_gap(std::uint64_t now, const char* why) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    ++stats_.gaps;
    stats_.last_gap_ms = now;
    stats_.last_gap_why = why != nullptr ? why : "";
}

void PacketSelf::relink() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    ++stats_.relinks;
}

void PacketSelf::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    ++stats_.resets;
    // The points' seq is kept: a reader that fed up to one goes on from it,
    // and the next point after a reset is still newer than any it saw.
    vitals_ = Vitals{};
    history_n_ = 0;
    bag_ = Bag{};
    bag_seen_n_ = 0;
    // The link's counts are the session's; the times are this game's.
    link_.last_server_ms = 0;
    link_.last_tick_ms = 0;
    link_.last_entry_ms = 0;
    tick_n_ = 0;
    burst_ms_.fill(0);
    burst_counted_ = false;
    firsts_said_.clear();
}

void PacketSelf::set_max_from_screen(std::uint32_t max_hp, std::uint32_t max_mp, std::uint64_t at_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vitals_.max_from == MaxFrom::StatSheet || max_hp == 0 || max_mp == 0) return;
    vitals_.max_hp = std::max(max_hp, vitals_.hp);
    vitals_.max_mp = std::max(max_mp, vitals_.mp);
    vitals_.max_ms = at_ms;
    vitals_.max_from = MaxFrom::Screen;
}

std::uint32_t PacketSelf::epoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch_;
}

std::uint64_t PacketSelf::start_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return start_count_;
}

PacketSelf::Vitals PacketSelf::vitals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vitals_;
}

std::vector<PacketSelf::VitalsPoint> PacketSelf::history() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VitalsPoint> out;
    const std::size_t n = std::min(history_n_, kHistory);
    out.reserve(n);
    for (std::size_t i = history_n_ - n; i < history_n_; ++i) out.push_back(history_[i % kHistory]);
    return out;
}

PacketSelf::Bag PacketSelf::bag() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bag_;
}

std::vector<PacketSelf::BagSeen> PacketSelf::bag_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BagSeen> out;
    const std::size_t n = std::min(bag_seen_n_, kBagFrames);
    out.reserve(n);
    for (std::size_t i = bag_seen_n_ - n; i < bag_seen_n_; ++i) out.push_back(bag_seen_[i % kBagFrames]);
    return out;
}

std::vector<PacketSelf::Press> PacketSelf::presses_since(std::uint64_t from_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Press> out;
    const std::size_t n = std::min(presses_n_, kPresses);
    for (std::size_t i = presses_n_ - n; i < presses_n_; ++i) {
        const Press& p = presses_[i % kPresses];
        if (p.at_ms >= from_ms) out.push_back(p);
    }
    return out;
}

PacketSelf::Link PacketSelf::link(std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Link out = link_;
    out.ticks_last_min = 0;
    const std::size_t n = std::min(tick_n_, kTickTimes);
    for (std::size_t i = tick_n_ - n; i < tick_n_; ++i) {
        const std::uint64_t at = tick_times_[i % kTickTimes];
        if (at <= now && now - at < 60'000) ++out.ticks_last_min;
    }
    return out;
}

PacketSelf::Stats PacketSelf::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

} // namespace gobot
