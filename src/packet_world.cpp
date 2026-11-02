#include "gobot/packet_world.hpp"
#include "gobot/logger.hpp"
#include "gobot/monster_frames.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

namespace gobot {
namespace {

constexpr std::size_t kHeader = 8;      // frame bytes before the payload

/// Where a frame the world reads comes from. Each is read from one side only:
/// the server sends 0x0027, 0x0029 and 0x002A for other actors' moves in the
/// layouts your client uses for yours, and an opcode inside a wrapper can mean
/// something other than it does outside one.
enum class From : std::uint8_t {
    ClientUdp,      ///< your client, over UDP, straight off a datagram
    Server,         ///< the server, straight off the TCP stream
    ServerWrapped,  ///< the server, inside an 0x0081 and unpacked from it
};

/// The frames it reads, the side each comes from, and the size, header
/// included, that every one of them had in the recordings (§4e, §4f). A game
/// update that moves fields about may well change the size too, and a frame
/// of another size is then refused rather than read as anything at all.
struct Kind {
    std::uint16_t opcode;
    std::size_t   size;
    From          from;
};
constexpr Kind kKinds[] = {
    { 0x0027, 48, From::ClientUdp },        // a walk of yours starts, stops or turns
    { 0x0028, 36, From::ClientUdp },        // where you are: four of them at a map entry
    { 0x0029, 44, From::ClientUdp },        // a third report of yours, not yet understood
    { 0x002A, 40, From::ClientUdp },        // you leave the ground, jump again, or land
    { 0x0031, 92, From::ClientUdp },        // a skill cast, where you cast it
    { 0x0038, 96, From::Server },           // a monster stops, walks or is hit
    { 0x003F, 52, From::Server },           // a monster spawns
    { 0x0042, 2568, From::ServerWrapped },  // the map's table of monsters
    { 0x0086, 24, From::Server },           // you enter a map
};
static_assert(std::size(kKinds) == PacketWorld::kOpcodeCount, "kOpcodeCount counts kKinds");

/// Where `opcode` is in kKinds; kOpcodeCount for one it does not read.
std::size_t kind_index(std::uint16_t opcode) {
    for (std::size_t k = 0; k < std::size(kKinds); ++k)
        if (kKinds[k].opcode == opcode) return k;
    return std::size(kKinds);
}

bool from_its_side(const GamePacket& p, From from) {
    switch (from) {
    case From::ClientUdp:     return !p.from_server && p.udp && p.depth == 0;
    case From::Server:        return p.from_server && !p.udp && p.depth == 0;
    case From::ServerWrapped: return p.from_server && !p.udp && p.depth != 0;
    }
    return false;
}

bool within(float v, float lo, float hi) { return v >= lo && v <= hi; }   // false for NaN too

/// A worked-out x or y: never off the map's u16 range, whatever a walk says.
float to_coord(double v) {
    return static_cast<float>(std::clamp(v, 0.0, static_cast<double>(PacketWorld::kMaxCoord)));
}

/// You, from your last fix to `now`: along your facing at your speed, for as
/// long as a walk or a jump is followed; and in a jump, up at its own speed,
/// slowing under gravity, to its top, where y holds until the next report. The
/// way down is not followed: where the ground is, no frame says, and the
/// landing's report does.
void follow_me(PacketWorld::Me& me, double rise, std::uint64_t now, std::uint64_t own_follow_ms) {
    const std::uint64_t age = now > me.report_ms ? now - me.report_ms : 0;
    const std::uint64_t followed = me.airborne ? PacketWorld::kAirReckonMs : own_follow_ms;
    if (me.moving && me.facing != 0) {
        const double secs = static_cast<double>(std::min(age, followed)) / 1000.0;
        me.x = to_coord(me.x + static_cast<double>(me.facing) * me.speed * PacketWorld::kStepsPerSecond * secs);
    }
    if (me.airborne && rise < 0.0) {
        const double top = -rise / PacketWorld::kGravity;     // seconds from the fix to the top
        const double secs = std::min(static_cast<double>(age) / 1000.0, top);
        me.y = to_coord(me.y + rise * secs + 0.5 * PacketWorld::kGravity * secs * secs);
    }
    me.uncertain = (me.moving && (me.facing == 0 || age > followed)) ||
                   (me.airborne && age > PacketWorld::kAirReckonMs);
}

/// A monster, from its last frame to `now`: a walker along its facing at its
/// speed until its walk has run its course, whatever turns it made; anything
/// else where that frame put it. A walk that reaches the map's wall ends there
/// with a stop frame (§4f), so none is followed through one.
void follow_mob(PacketWorld::Mob& m, std::uint64_t walk_from, std::uint64_t now) {
    if (!m.moving || m.dead) {
        m.moving = false;
        return;
    }
    const std::uint64_t walk_end = walk_from + PacketWorld::kMobWalkMs;
    const std::uint64_t until = std::min(now, walk_end);
    if (until > m.event_ms) {
        const double secs = static_cast<double>(until - m.event_ms) / 1000.0;
        m.x = to_coord(m.x + static_cast<double>(m.facing) * m.speed * PacketWorld::kStepsPerSecond * secs);
    }
    m.moving = now < walk_end;
}

/// The most HP a slot has been seen with stands for its max HP while neither
/// the table nor a spawn has said, and max HP is never below the HP it has.
void saw_hp(PacketWorld::Mob& m, std::int64_t hp) {
    if (hp > static_cast<std::int64_t>(m.max_hp) &&
        hp <= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
        m.max_hp = static_cast<std::uint32_t>(hp);
}

/// A monster as the table or a spawn gives it, with its type and its max HP,
/// at `hp`: above 0 alive, a walker in the table walking on along its facing;
/// at 0 or below dead, where it is.
PacketWorld::Mob mob_from(const MapMonster& e, std::int64_t hp, std::uint64_t t) {
    PacketWorld::Mob m;
    m.slot = e.slot;
    m.type_id = e.type_id;
    m.level = e.level;
    m.x = static_cast<float>(e.x);
    m.y = static_cast<float>(e.y);
    m.facing = e.facing;
    m.speed = e.speed;
    m.max_hp = e.max_hp;
    m.max_hp_known = true;
    m.event_ms = t;
    if (hp > 0) {
        m.hp = static_cast<std::uint32_t>(hp);
        m.moving = e.speed > 0.0f;
        saw_hp(m, hp);
    } else {
        m.dead = true;
        m.dead_ms = t;
    }
    return m;
}

} // namespace

const PacketWorld::Mob* PacketWorld::View::mob(std::uint32_t slot) const {
    for (const Mob& m : mobs)
        if (m.slot == slot) return &m;
    return nullptr;
}

PacketWorld::Stats::Frames PacketWorld::Stats::of(std::uint16_t opcode) const {
    const std::size_t k = kind_index(opcode);
    if (k < frames.size()) return frames[k];
    Frames none;
    none.opcode = opcode;
    return none;
}

std::uint64_t PacketWorld::Stats::own(OwnReport kind) const {
    switch (kind) {
    case OwnReport::Move:  return of(0x0027).used;
    case OwnReport::Air:   return of(0x002A).used;
    case OwnReport::Other: return of(0x0029).used;
    case OwnReport::Here:  return of(0x0028).used;
    case OwnReport::Cast:  return of(0x0031).used;
    }
    return 0;
}

PacketWorld::PacketWorld() { forget(); }

const std::vector<std::uint16_t>& PacketWorld::opcodes() {
    static const std::vector<std::uint16_t> all = [] {
        std::vector<std::uint16_t> v;
        for (const Kind& k : kKinds) v.push_back(k.opcode);
        return v;
    }();
    return all;
}

void PacketWorld::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    forget();
}

void PacketWorld::forget() {
    fix_ = Fix{};
    slots_.clear();
    map_ = 0;
    area_ = 0;
    map_since_ms_ = 0;
    table_seen_ = false;
    first_monster_ms_ = 0;
    walk_timed_ = false;
    walk_from_ms_ = 0;
    stats_ = Stats{};
    for (std::size_t k = 0; k < kOpcodeCount; ++k) stats_.frames[k].opcode = kKinds[k].opcode;
    unknown_logged_.fill(0);
    // updates_ goes on: a picture number taken before this is not a newer one.
    // So does map_epoch_: the map entries are ever, not since the capture --
    // and the last one's spawn with it, which the hunting spot measures an
    // entry by however soon after it a capture is started again.
}

void PacketWorld::on_packets(const std::vector<GamePacket>& batch) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool used = false;
    for (const GamePacket& p : batch) {
        // Any frame at all says how late it is, the server's tick among them.
        time_walk(p.timestamp_ms);

        const std::size_t k = kind_index(p.opcode);
        if (k == kOpcodeCount || !from_its_side(p, kKinds[k].from)) continue;
        Stats::Frames& counts = stats_.frames[k];
        if (p.payload.size() + kHeader != kKinds[k].size) {
            ++counts.wrong_size;
            stats_.last_wrong_size_ms = p.timestamp_ms;
            // One of your reports' opcodes -- your client's, over UDP: a walk
            // of yours may be what went unread.
            if (kKinds[k].from == From::ClientUdp) stats_.last_own_wrong_size_ms = p.timestamp_ms;
            continue;
        }

        Taken taken = Taken::Refused;
        switch (p.opcode) {
        case 0x0086: taken = take_entry(p); break;
        case 0x0042: taken = take_table(p); break;
        case 0x003F: taken = take_spawn(p); break;
        case 0x0038: taken = take_state(p); break;
        default:     taken = take_own(p); break;
        }
        if (taken == Taken::Used) {
            ++counts.used;
            used = true;
        } else if (taken == Taken::Refused) {
            ++counts.refused;
        }
    }
    // Once a batch, however many frames it used: the capture hands frames over
    // a batch at a time, and a batch is what the hunter can see change.
    if (used) ++updates_;
}

PacketWorld::Taken PacketWorld::take_own(const GamePacket& p) {
    const std::optional<OwnPosition> r = read_own_position(p);
    if (!r) return Taken::Refused;
    // A game update that moves the fields and keeps the size reads anything at
    // all: values no report has had are refused, not followed.
    if (!within(r->x, 0.0f, kMaxCoord) || !within(r->y, 0.0f, kMaxCoord)) return Taken::Refused;
    if (r->kind != OwnReport::Cast && (!r->speed || !within(*r->speed, 0.0f, kMaxSpeed)))
        return Taken::Refused;
    // In the air the vertical speed is followed up to the jump's top, so a
    // jump needs one, and one rising faster than kMaxSpeed is refused. A
    // landing's and a fall's are not followed, and the frame is not lost over
    // them: what a landing's holds no recording says, and if it is the speed
    // you came down at, a fall of some 830 px takes it past kMaxSpeed.
    if (r->kind == OwnReport::Air &&
        (!r->air_state ||
         (*r->air_state != 0 && (!r->vertical_speed || *r->vertical_speed < -kMaxSpeed))))
        return Taken::Refused;

    const std::uint64_t t = p.timestamp_ms;
    // Yours by your id: the first one seen, and after each map entry the next
    // one again, so a character changed at the selection screen is learned
    // anew. Another id meanwhile is a second game client on this PC whose
    // datagrams the capture caught, or a frame read wrong: not you.
    if (fix_.id_locked && r->id != fix_.me.id) {
        ++stats_.foreign;
        stats_.last_foreign_ms = t;
        return Taken::Foreign;
    }
    fix_.id_locked = true;

    Me& me = fix_.me;
    const bool was_moving = me.known && me.moving;
    me.known = true;
    me.id = r->id;
    me.x = r->x;
    me.y = r->y;
    me.report_x = r->x;
    me.report_y = r->y;
    me.report_ms = t;
    me.kind = r->kind;
    // Only 0x0027, 0x0029 and 0x0028 say which way you face: a jump goes on
    // the way you were facing. A 0x0027's is also kept apart, with its time
    // -- what the hunter weighs against its own last key and turns the
    // character by (WorldFix::facing). Only a 0x0027's: its facing byte is the
    // one checked against the game (84 of 84), while what sends a 0x0029 is
    // not settled and a 0x0028's facing is only assumed to be its flags' low
    // byte -- good enough to follow a walk by, not to press a key over. A cast
    // or a jump, saying nothing of it, leaves both as they were.
    if (r->facing != 0) {
        me.facing = r->facing;
        if (r->kind == OwnReport::Move) {
            me.said_facing = r->facing;
            me.facing_ms = t;
        }
    }
    // A cast says nothing of how you move, so a walk goes on from where you
    // cast, at the speed it had.
    if (r->kind != OwnReport::Cast) me.speed = *r->speed;
    me.moving = me.speed > 0.0f;
    // y holds from every report but a jump's, which rises at its own speed.
    // Whether a cast or a turn in the air stops the jump, nothing says, so y
    // holds from those too, until the landing's report.
    fix_.rise = 0.0;
    // A cast while in the air -- one a jump attack would send -- within the
    // time a landing is waited for.
    if (r->kind == OwnReport::Cast && me.airborne && t <= fix_.air_from_ms + kAirReckonMs)
        ++stats_.casts_in_air;
    if (r->kind == OwnReport::Air) {
        // 1 off the ground, 6 a second jump, 2 a high jump, 3 seen at a
        // vertical speed of -10: every state but 0, landed, is in the air.
        switch (*r->air_state) {
            case 1:  ++stats_.air_jumps; break;
            case 6:  ++stats_.air_second; break;
            case 2:  ++stats_.air_high; break;
            case 0:  ++stats_.air_landed; break;
            default: ++stats_.air_other; break;
        }
        me.airborne = *r->air_state != 0;
        if (me.airborne) {
            fix_.rise = static_cast<double>(*r->vertical_speed) * kStepsPerSecond;
            fix_.air_from_ms = t;
        }
    } else if (me.airborne && t > fix_.air_from_ms + kAirReckonMs) {
        // This long after the last 0x002A that had you in the air, and no
        // landing: it was lost on the way, or refused, and this report has
        // you on the ground. A UDP datagram the capture missed would
        // otherwise keep you in the air until your next jump.
        me.airborne = false;
    }
    stats_.last_own_ms = t;

    // Whether letting go of a walk key always sends a stop, timed from each
    // walk started from standing: a report that has you standing ends it.
    if (r->kind == OwnReport::Move && me.moving && !was_moving) {
        ++stats_.own_walks;
        walk_timed_ = true;
        walk_from_ms_ = t;
    } else if (walk_timed_ && r->kind != OwnReport::Cast && !me.moving) {
        ++stats_.own_walks_stopped;
        walk_timed_ = false;
    }
    return Taken::Used;
}

PacketWorld::Taken PacketWorld::take_entry(const GamePacket& p) {
    const std::optional<MapEntry> e = read_map_entry(p);
    if (!e) return Taken::Refused;
    const std::uint64_t t = p.timestamp_ms;
    // A new place, whichever it is: (10, 4) and (10, 5) are two areas of one
    // map, with monsters of their own. The last place's monsters are gone;
    // this one's table comes within about a second, and each of its slots'
    // events within a few.
    map_ = e->map;
    area_ = e->area;
    map_since_ms_ = t;
    table_seen_ = false;
    first_monster_ms_ = 0;
    slots_.clear();

    // You stand at the spawn point. Which way you face it does not say; the
    // client's four 0x0028 that follow do. The next report's id is yours.
    Me& me = fix_.me;
    me.known = true;
    me.x = static_cast<float>(e->x);
    me.y = static_cast<float>(e->y);
    me.report_x = me.x;
    me.report_y = me.y;
    me.facing = 0;
    me.said_facing = 0;
    me.facing_ms = 0;
    me.speed = 0.0f;
    me.moving = false;
    me.airborne = false;
    me.report_ms = t;
    me.kind.reset();
    fix_.rise = 0.0;
    fix_.id_locked = false;
    // A walk being timed ends standing here, as at a stop.
    if (walk_timed_) {
        ++stats_.own_walks_stopped;
        walk_timed_ = false;
    }
    ++stats_.map_entries;
    // Counted on through every reset(): a door taken across a capture started
    // again is still a door taken.
    ++map_epoch_;
    entry_x_ = me.x;
    entry_y_ = me.y;
    stats_.entry_map = e->map;
    stats_.entry_area = e->area;
    stats_.entry_x = me.x;
    stats_.entry_y = me.y;
    stats_.entry_ms = t;
    return Taken::Used;
}

PacketWorld::Taken PacketWorld::take_table(const GamePacket& p) {
    const std::optional<std::vector<MapMonster>> table = read_map_monsters(p);
    if (!table) return Taken::Refused;
    // The HP column is the slot's 0x0038 HP (24 of 24): a signed number, which
    // a kill leaves below 0. Read as a u32, a monster killed just before the
    // load, its spawn still to come, would be alive with some 4 billion HP;
    // read as signed, it is listed dead. A living walker's speed is followed,
    // so a living monster's is held to kMaxSpeed; a dead one's is not
    // followed, and may be what its blow threw it at.
    const auto hp_of = [](const MapMonster& e) { return static_cast<std::int32_t>(e.hp); };
    for (const MapMonster& e : *table)
        if (hp_of(e) > 0 && !within(e.speed, 0.0f, kMaxSpeed)) return Taken::Refused;
    const std::uint64_t t = p.timestamp_ms;
    heard_monster(t);
    // Every monster of the map as it stands at the load: what was known
    // before is replaced. A walker is mid-walk; when it began is not known,
    // so it is followed for a whole walk from here, until its next frame.
    slots_.clear();
    for (const MapMonster& e : *table) {
        Slot& s = slots_[e.slot];
        s.at = mob_from(e, hp_of(e), t);
        s.walk_from_ms = t;
    }
    table_seen_ = true;
    ++stats_.tables;
    return Taken::Used;
}

PacketWorld::Taken PacketWorld::take_spawn(const GamePacket& p) {
    const std::optional<MapMonster> s = read_monster_spawn(p);
    if (!s) return Taken::Refused;
    const std::uint64_t t = p.timestamp_ms;
    heard_monster(t);
    // A new monster in the slot, full of HP, standing at one of the map's
    // spawn points; its first stop comes 9-36 px lower, as it lands.
    Slot& slot = slots_[s->slot];
    slot.at = mob_from(*s, s->hp, t);
    slot.walk_from_ms = 0;
    ++stats_.spawns;
    return Taken::Used;
}

PacketWorld::Taken PacketWorld::take_state(const GamePacket& p) {
    const std::optional<MonsterState> s = read_monster_state(p);
    if (!s) return take_other_state(p);
    // A walk's speed is followed, so a walk past kMaxSpeed or below 0 is
    // refused. A stop's is not, nor a hit's -- how far the blow throws the
    // monster, 7 or 18 in the fight -- and the frame is not lost over one: a
    // killing blow that throws harder is a kill all the same.
    if (s->event == MonsterEvent::Walk && !within(s->speed, 0.0f, kMaxSpeed)) return Taken::Refused;
    const std::uint64_t t = p.timestamp_ms;
    heard_monster(t);

    Slot& slot = slots_[s->slot];
    Mob& m = slot.at;
    // A walk that follows a walk still under way is a turn: a walk's 200
    // steps count from its start, not from the turn. One that comes after
    // the walk has run its course is a new walk.
    const bool turn = s->event == MonsterEvent::Walk && m.moving && t < slot.walk_from_ms + kMobWalkMs;
    m.slot = s->slot;
    m.x = static_cast<float>(s->x);
    m.y = static_cast<float>(s->y);
    m.facing = s->facing;
    m.speed = s->speed;
    m.event_ms = t;
    // A stopped monster stays where it is, and so does one that was hit,
    // whatever speed the blow gives it.
    m.moving = s->event == MonsterEvent::Walk;
    if (m.moving && !turn) slot.walk_from_ms = t;
    switch (s->event) {
    case MonsterEvent::Stop: ++stats_.stops; break;
    case MonsterEvent::Walk: ++stats_.walks; break;
    case MonsterEvent::Hit:  ++stats_.hits; break;
    }

    // In a hit, what the monster had before it is HP seen as well: the HP
    // after it plus the damage, 122 of 122 in the fight.
    if (s->event == MonsterEvent::Hit) saw_hp(m, std::int64_t{ s->hp } + s->damage);
    if (s->killed()) {
        // Dead now, and nothing more will say so: its slot falls silent until
        // a spawn brings the next monster, 2-4 s later.
        m.hp = 0;
        if (!m.dead) {
            m.dead = true;
            m.dead_ms = t;
            ++stats_.kills;
        }
        return Taken::Used;
    }
    m.hp = s->hp > 0 ? static_cast<std::uint32_t>(s->hp) : 0;
    saw_hp(m, s->hp);
    // HP above 0 in the slot of a monster killed is a new monster whose spawn
    // was not seen. A slot keeps its type over respawns (122 of 122).
    if (m.dead && s->hp > 0) {
        m.dead = false;
        m.dead_ms = 0;
    }
    return Taken::Used;
}

PacketWorld::Taken PacketWorld::take_other_state(const GamePacket& p) {
    // The reader knows stops, walks and hits, the only states the fight had.
    // One of those it refused -- a speed that is no number -- stays refused.
    const std::optional<MonsterSpot> spot = read_monster_spot(p);
    if (!spot || spot->state == 0 || spot->state == 1 || spot->state == 9) return Taken::Refused;
    const std::uint64_t t = p.timestamp_ms;

    // Any other is counted, by its state, and its first frames written down
    // for the next look at the protocol to explain.
    ++stats_.unknown_states;
    Stats::Kind* kind = nullptr;
    for (Stats::Kind& k : stats_.unknown_kinds) {
        if (k.count != 0 && k.state != spot->state) continue;
        k.state = spot->state;
        ++k.count;
        kind = &k;
        break;
    }
    if (unknown_logged_[spot->state] < kUnknownLogged) {
        ++unknown_logged_[spot->state];
        std::string bytes;
        static const char* const kDigits = "0123456789abcdef";
        for (const std::uint8_t b : p.payload) {
            if (!bytes.empty()) bytes += ' ';
            bytes += kDigits[b >> 4];
            bytes += kDigits[b & 15];
        }
        GB_INFO("Positions: a monster frame (0x0038) of state %u, which is not known -- slot %u at %u,%u; "
                "its bytes from 8: %s",
                static_cast<unsigned>(spot->state), static_cast<unsigned>(spot->slot),
                static_cast<unsigned>(spot->x), static_cast<unsigned>(spot->y), bytes.c_str());
    }

    // Where it says the monster is, if that is near where the monster was
    // worked out to be: standing there, and nothing else about it changed.
    // Bytes 32 and 34 are the monster's place in each of the three states
    // known, and the layout is one; if they are something else here, most such
    // frames land far off and are refused, and the kind's average says so.
    // Refused too: a frame for a slot with no monster known, or a dead one.
    const auto it = slots_.find(spot->slot);
    if (it == slots_.end() || it->second.at.dead) return Taken::Refused;
    Mob now = it->second.at;
    follow_mob(now, it->second.walk_from_ms, t);
    const float off = std::max(std::abs(now.x - static_cast<float>(spot->x)),
                               std::abs(now.y - static_cast<float>(spot->y)));
    if (kind) {
        ++kind->judged;
        kind->off_px += static_cast<double>(off);
    }
    if (off > kUnknownPlacePx) return Taken::Refused;

    heard_monster(t);
    Mob& m = it->second.at;
    m.x = static_cast<float>(spot->x);
    m.y = static_cast<float>(spot->y);
    m.moving = false;
    m.event_ms = t;
    ++stats_.unknown_placed;
    if (kind) ++kind->placed;
    return Taken::Used;
}

void PacketWorld::heard_monster(std::uint64_t t) {
    if (first_monster_ms_ == 0) first_monster_ms_ = t;
    stats_.last_monster_ms = t;
}

void PacketWorld::time_walk(std::uint64_t t) {
    if (walk_timed_ && t > walk_from_ms_ + kStopWithinMs) {
        ++stats_.own_walks_unstopped;
        walk_timed_ = false;
    }
}

PacketWorld::View PacketWorld::view(std::uint64_t now_ms, std::uint64_t own_follow_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    View v;
    v.own_follow_ms = own_follow_ms;
    v.me = fix_.me;
    if (v.me.known) follow_me(v.me, fix_.rise, now_ms, own_follow_ms);
    v.mobs.reserve(slots_.size());
    for (const auto& entry : slots_) {
        Mob m = entry.second.at;
        follow_mob(m, entry.second.walk_from_ms, now_ms);
        v.mobs.push_back(m);
    }
    v.map = map_;
    v.area = area_;
    v.map_since_ms = map_since_ms_;
    v.table_seen = table_seen_;
    v.first_monster_ms = first_monster_ms_;
    v.updates = updates_;
    v.map_epoch = map_epoch_;
    v.entry_x = entry_x_;
    v.entry_y = entry_y_;
    return v;
}

PacketWorld::Stats PacketWorld::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

} // namespace gobot
