// Your casts, your buffs and the jump test by the packets (the packets plan,
// items F, H and M); the keeper's hints (item I) are written where the keeper
// is, simple_keeper.cpp and keeper_loop.
//
// Casts and buffs are only written to the log: every cast your client sends
// is paired with the press before it of a macro key that can cast -- the
// potions' are passed over, a drink sends no cast (CastMatch), and a buff's frames
// set beside its key (BuffWatch), once a minute. Your own casts are also
// handed to the macro, which paces its skills on them when cast_paced is on
// (2026.11.08) and does nothing with them otherwise. The jump test goes by your
// client's own take-offs whenever the packets can tell (packet_takeoffs), and
// by the screen as before when they cannot.
//
// Nothing here acts on the game, and nothing here asks Windows for anything
// but which process owns the game's window, and the time of day for the log.
// The capture thread feeds the casts and buffs; GoBot's thread pairs them and
// writes the lines; the jump test's thread asks for the take-offs.

#include "gui_app.hpp"
#include "gobot/cast_match.hpp"
#include "gobot/logger.hpp"
#include "gobot/macro.hpp"
#include "gobot/net_discovery.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
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

/// The time of day `at_ms` (now_ms()'s clock) was, as the log writes it:
/// HH:MM:SS.mmm. For the cast lines' times.
std::string cast_clock_text(std::uint64_t at_ms) {
    const std::uint64_t now = now_ms();
    const std::chrono::system_clock::time_point wall =
        std::chrono::system_clock::now() - std::chrono::milliseconds(now > at_ms ? now - at_ms : 0);
    const std::time_t t = std::chrono::system_clock::to_time_t(wall);
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() % 1000;
    std::tm local{};
    if (localtime_s(&local, &t) != 0) return "?";
    char buf[24];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03lld", local.tm_hour, local.tm_min, local.tm_sec, ms);
    return buf;
}

/// The macro's buff keys: what each press of theirs should cast is a buff.
/// Only rows the timers press (macro_buff_on): a buff row switched off with a
/// skill's key on it made that skill a "buff" in every Casts line and a "maybe
/// not cast" every few seconds (2026-11-08, A and S).
std::vector<std::uint32_t> buff_keys_of(const MacroConfig& cfg) {
    return macro_buff_keycodes(cfg.keys);
}

/// The macro's potion keys -- HP's, MP's and the pet's -- that nothing else of
/// it presses: a drink sends no cast, and a press of one paired would take the
/// cast of the skill pressed after it. A key a skill, a buff or a step of the
/// sequence also uses is kept: its presses may cast -- a skill or buff row
/// only while on (macro_timer_on), as a row switched off presses nothing.
std::vector<std::uint32_t> potion_keys_of(const MacroConfig& cfg) {
    std::vector<std::uint32_t> casting, out;
    for (const MacroKey& k : cfg.keys) {
        const MacroKeyKind kind = macro_key_kind(k);
        if (macro_timer_on(k) && (kind == MacroKeyKind::Skill || kind == MacroKeyKind::Buff))
            casting.push_back(k.keycode);
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
    // Whether the keys are held for a job as the batch's casts came, for the
    // casts no press took (2026.11.08): an atomic of each, no lock.
    part.casts.set_held(coord_.critical_active());
    // And who presses, on which capture: a gap across a stop and start, or a
    // capture's restart, is no pause of a run's (2026.11.08). Atomics only.
    part.casts.set_run(macro_ && macro_->running(), hunter_ && hunter_->running(), capture_.start_count());
    part.casts.on_packets(all, packet_world_);
    part.buffs.on_packets(all);
    // And your own casts to the macro, for the skills paced on them
    // (cast_paced, 2026.11.08): your client's 0x0031 over UDP under your own
    // id -- the world has had the batch, so a first report of yours since a
    // map entry has locked it -- at the frame's own time. Atomics only on the
    // macro's side: no lock of its is taken here.
    const std::optional<std::uint32_t> me = packet_world_.own_id();
    if (macro_ && me) {
        for (const GamePacket& p : all) {
            if (p.opcode != 0x0031) continue;
            const std::optional<CastFrame> c = read_cast(p);
            if (c && c->id == *me) macro_->note_own_cast(c->skill, p.timestamp_ms);
        }
    }
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
    // Each a copy, taken under its own lock; the lines are built here and
    // written off this thread, in this order, as one batch (log_later).
    const CastMatch::Minute casts = part.casts.take_minute();
    const BuffWatch::Minute buffs = part.buffs.take_minute();
    std::vector<std::string> lines;
    const std::string cast_line = casts_line(casts);
    if (!cast_line.empty()) lines.push_back(cast_line);
    // How evenly they came, and the casts no press of GoBot's took, each
    // described -- at most CastMatch::kLoneSaid a minute (2026.11.08).
    const std::string gaps_line = cast_gaps_line(casts, &cast_clock_text);
    if (!gaps_line.empty()) lines.push_back(gaps_line);
    for (const CastMatch::Lone& c : casts.lone) lines.push_back(lone_cast_line(c, &cast_clock_text));
    if (casts.lone_more > 0)
        lines.push_back("Cast (packets) with no GoBot key: " + std::to_string(casts.lone_more) +
                        " more this minute, not described");
    // Those with no press of GoBot's near them and no hold: your own play
    // beside GoBot, counted in one line.
    if (casts.lone_far > 0)
        lines.push_back("Cast (packets) with no GoBot key: " + std::to_string(casts.lone_far) +
                        " with no GoBot press within " + std::to_string(CastMatch::kLoneNearMs / 1000) +
                        " s either side and no hold (by hand or another tool), not described");

    // Each buff key's interval in the macro, beside what its frames said: the
    // unit of 0x0077's value is proven when two buffs' values stand in the
    // proportion of their intervals. Only rows the timers press.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> intervals;
    for (const MacroKey& k : macro_config_.keys)
        if (macro_buff_on(k)) intervals.emplace_back(k.keycode, macro_timer_interval_ms(k) / 1000);
    const std::string buff_line = buffs_line(pair_buffs(casts.keys, buffs), buffs, intervals);
    if (!buff_line.empty()) lines.push_back(buff_line);
    log_later(std::move(lines));
}

void GuiApp::log_later(std::vector<std::string> lines) {
    // Said here and written off this thread, so the file's write and flush
    // never hold up the window (2026.11.08): kept in the order said, and the
    // worker writes whatever is kept. A post refused -- kMaxJobs writes
    // waiting, none begun -- loses nothing: the first of them takes these too.
    if (lines.empty()) return;
    {
        std::lock_guard<std::mutex> lock(log_later_mutex_);
        for (std::string& l : lines) log_later_.push_back(std::move(l));
    }
    log_worker_.post([this] { write_logged_later(); });
}

void GuiApp::write_logged_later() {
    // On log_worker_'s thread: no file is written under log_later_mutex_.
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(log_later_mutex_);
        lines.swap(log_later_);
    }
    for (const std::string& l : lines) Logger::instance().write(LogLevel::Info, l);
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
