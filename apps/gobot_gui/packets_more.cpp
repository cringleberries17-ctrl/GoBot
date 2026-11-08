// Your casts, your buffs and the jump test by the packets (the packets plan,
// items F, H and M); the keeper's hints (item I) are written where the keeper
// is, simple_keeper.cpp and keeper_loop.
//
// Casts and buffs are only written to the log: every cast your client sends
// is paired with the press before it of a macro key that can cast -- the
// potions' are passed over, a drink sends no cast (CastMatch), and a buff's frames
// set beside its key (BuffWatch), once a minute. The jump test goes by your
// client's own take-offs whenever the packets can tell (packet_takeoffs), and
// by the screen as before when they cannot.
//
// Nothing here acts on the game, and nothing here asks Windows for anything
// but which process owns the game's window. The capture thread feeds the casts
// and buffs; GoBot's thread pairs them and writes the lines; the jump test's
// thread asks for the take-offs.

#include "gui_app.hpp"
#include "gobot/cast_match.hpp"
#include "gobot/logger.hpp"
#include "gobot/macro.hpp"
#include "gobot/net_discovery.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gobot {
namespace {

/// How long more_tick's look at which game the capture is on holds for
/// packet_takeoffs: it is taken every second while the packets are wanted.
constexpr std::uint64_t kOnGameFreshMs = 3000;

/// A report of yours under another id this recently, and a take-off counted
/// may be another client's: the packets cannot tell.
constexpr std::uint64_t kForeignQuietMs = 10'000;

/// MorePart is made by whichever of its users comes first -- the capture
/// thread, GoBot's thread, the jump test's -- under this one lock, and the
/// pointer is read under it too.
std::mutex g_more_made;

template <class Part, class Free>
Part& made(std::unique_ptr<Part, Free>& part) {
    std::lock_guard<std::mutex> lock(g_more_made);
    if (!part) part.reset(new Part);
    return *part;
}

template <class Part, class Free>
const Part* made_already(const std::unique_ptr<Part, Free>& part) {
    std::lock_guard<std::mutex> lock(g_more_made);
    return part.get();
}

/// The macro's buff keys: what each press of theirs should cast is a buff.
std::vector<std::uint32_t> buff_keys_of(const MacroConfig& cfg) {
    std::vector<std::uint32_t> out;
    for (const MacroKey& k : cfg.keys)
        if (k.keycode != 0 && macro_key_kind(k) == MacroKeyKind::Buff) out.push_back(k.keycode);
    return out;
}

/// The macro's potion keys -- HP's, MP's and the pet's -- that nothing else of
/// it presses: a drink sends no cast, and a press of one paired would take the
/// cast of the skill pressed after it. A key a skill, a buff or a step of the
/// sequence also uses is kept: its presses may cast.
std::vector<std::uint32_t> potion_keys_of(const MacroConfig& cfg) {
    std::vector<std::uint32_t> casting, out;
    for (const MacroKey& k : cfg.keys) {
        const MacroKeyKind kind = macro_key_kind(k);
        if (k.keycode != 0 && (kind == MacroKeyKind::Skill || kind == MacroKeyKind::Buff)) casting.push_back(k.keycode);
    }
    for (const MacroStep& s : cfg.sequence)
        if (s.keycode != 0) casting.push_back(s.keycode);
    for (const MacroKey& k : cfg.keys) {
        const MacroKeyKind kind = macro_key_kind(k);
        if (k.keycode == 0 ||
            (kind != MacroKeyKind::HpPotion && kind != MacroKeyKind::MpPotion && kind != MacroKeyKind::PetPotion))
            continue;
        if (std::find(casting.begin(), casting.end(), k.keycode) == casting.end()) out.push_back(k.keycode);
    }
    return out;
}

} // namespace

struct GuiApp::MorePart {
    CastMatch casts;
    BuffWatch buffs;
    /// The hunter's own presses of its attack key since the last pairing
    /// (Hunter::set_on_attack), from its thread: the macro skips that key, so
    /// a cast of it was given to some macro key (2026.11.04d).
    std::mutex                    attack_mutex;
    std::vector<CastMatch::Press> attacks;
    /// Whether the capture was on the game GoBot follows when more_tick last
    /// looked, and when that was: position_link_ is GoBot's thread's, and the
    /// jump test asks from a thread of its own.
    std::atomic<bool>          on_game{ false };
    std::atomic<std::uint64_t> on_game_ms{ 0 };
};

void GuiApp::MorePartFree::operator()(MorePart* p) const { delete p; }

void GuiApp::more_on_packets(const std::vector<GamePacket>& all) {
    // On the capture thread, after the packet world has had the batch: a
    // named cast's target is looked for where the world has it at the cast's
    // own time.
    MorePart& part = made(more_part_);
    part.casts.on_packets(all, packet_world_);
    part.buffs.on_packets(all);
}

void GuiApp::more_tick(std::uint64_t now) {
    MorePart& part = made(more_part_);
    // Which game the capture is on, for packet_takeoffs: only the one GoBot
    // follows, by the process that owns its window.
    const bool on_game = capture_.running() && position_capture_ && position_link_.pid != 0 &&
                         position_link_.pid == pid_for_window(input_.target_window());
    part.on_game.store(on_game);
    part.on_game_ms.store(now);

    // The macro's presses since the last pairing, each with the cast it made
    // -- a potion's passed over: a drink sends no 0x0031.
    const std::vector<std::uint32_t> potions = potion_keys_of(macro_config_);
    std::vector<CastMatch::Press> presses;
    for (const PacketSelf::Press& p : packet_self_.presses_since(part.casts.pair_from())) {
        if (std::find(potions.begin(), potions.end(), p.vk) != potions.end()) continue;
        CastMatch::Press press;
        press.vk = p.vk;
        press.at_ms = p.at_ms;
        presses.push_back(press);
    }
    {
        // And the hunter's attack key's (2026.11.04d): those the pairing has
        // not passed yet, the rest let go of.
        const std::uint64_t from = part.casts.pair_from();
        std::lock_guard<std::mutex> lock(part.attack_mutex);
        std::erase_if(part.attacks, [from](const CastMatch::Press& p) { return p.at_ms < from; });
        presses.insert(presses.end(), part.attacks.begin(), part.attacks.end());
    }
    part.casts.pair(presses, buff_keys_of(macro_config_), now);
    // Each buff key's burst, once judged: whether the packets saw it cast.
    for (const CastMatch::Burst& b : part.casts.take_bursts())
        Logger::instance().write(LogLevel::Info, burst_line(b));
}

void GuiApp::more_note_attack(std::uint32_t vk, std::uint64_t at_ms) {
    // On the hunter's thread: kept, and paired at the next more_tick.
    MorePart& part = made(more_part_);
    std::lock_guard<std::mutex> lock(part.attack_mutex);
    if (part.attacks.size() < CastMatch::kCasts) part.attacks.push_back(CastMatch::Press{ vk, at_ms });
}

void GuiApp::note_casts_minute(std::uint64_t) {
    MorePart& part = made(more_part_);
    // Each a copy, taken under its own lock; the lines are built here.
    const CastMatch::Minute casts = part.casts.take_minute();
    const BuffWatch::Minute buffs = part.buffs.take_minute();
    const std::string cast_line = casts_line(casts);
    if (!cast_line.empty()) Logger::instance().write(LogLevel::Info, cast_line);

    // Each buff key's interval in the macro, beside what its frames said: the
    // unit of 0x0077's value is proven when two buffs' values stand in the
    // proportion of their intervals.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> intervals;
    for (const MacroKey& k : macro_config_.keys)
        if (k.keycode != 0 && macro_key_kind(k) == MacroKeyKind::Buff)
            intervals.emplace_back(k.keycode, macro_timer_interval_ms(k) / 1000);
    const std::string buff_line = buffs_line(pair_buffs(casts.keys, buffs), buffs, intervals);
    if (!buff_line.empty()) Logger::instance().write(LogLevel::Info, buff_line);
}

std::optional<std::uint64_t> GuiApp::packet_takeoffs() const {
    // The packets tell a take-off only on the game GoBot follows, with your
    // reports coming under your own id and no other client's among them.
    if (!capture_.running()) return std::nullopt;
    const MorePart* part = made_already(more_part_);
    if (part == nullptr) return std::nullopt;
    const std::uint64_t now = now_ms();
    const std::uint64_t looked = part->on_game_ms.load();
    if (!part->on_game.load() || looked == 0 || now > looked + kOnGameFreshMs) return std::nullopt;

    const PacketWorld::Stats s = packet_world_.stats();
    // Your id is locked by the first report of yours since the start, the
    // relink or the last map entry: none since, and nothing is yours yet.
    if (s.last_own_ms == 0 || s.last_own_ms < s.entry_ms) return std::nullopt;
    if (s.last_foreign_ms != 0 && now < s.last_foreign_ms + kForeignQuietMs) return std::nullopt;
    if (!packet_world_.view(now).me.known) return std::nullopt;
    // Left the ground, a second jump in the air, a high jump.
    return s.air_jumps + s.air_second + s.air_high;
}

std::optional<PacketWorld::AirCounts> GuiApp::packet_air_counts() const {
    // packet_takeoffs' own rules, and the counts by kind: which jump went out
    // -- ↑, ↑↑ (a second), Ctrl + ↑ (high) -- and the walk-offs among the
    // jumps (2026.11.05b).
    if (!packet_takeoffs()) return std::nullopt;
    return packet_world_.stats().air();
}

std::vector<PacketWorld::OwnTrace> GuiApp::packet_own_since(std::uint64_t ms) const {
    // Under the same rules: a report of another client's is none of yours.
    if (!packet_takeoffs()) return {};
    return packet_world_.own_since(ms);
}

} // namespace gobot
