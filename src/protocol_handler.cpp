#include "gobot/protocol_handler.hpp"
#include "gobot/logger.hpp"

#include <initializer_list>
#include <variant>

namespace gobot {
namespace {

/// Field-name aliases, so a layout can call the id field "id", "entity_id" or
/// "monster_id" and still work. Protocol discovery is messy and forcing one
/// canonical spelling just produces silently-empty decodes.
std::int64_t first_int(const DecodedPacket& d, std::initializer_list<const char*> names,
                       std::int64_t def = 0) {
    for (const char* n : names) {
        if (d.has(n)) return d.get_int(n, def);
    }
    return def;
}

double first_float(const DecodedPacket& d, std::initializer_list<const char*> names,
                   double def = 0.0) {
    for (const char* n : names) {
        if (d.has(n)) return d.get_float(n, def);
    }
    return def;
}

std::string first_string(const DecodedPacket& d, std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (d.has(n)) return d.get_string(n);
    }
    return {};
}

} // namespace

ProtocolHandler::ProtocolHandler(const OpcodeRegistry& registry, GameState& state)
    : registry_(registry), state_(state) {}

void ProtocolHandler::on_packets(const std::vector<GamePacket>& packets) {
    for (const auto& p : packets) on_packet(p);
}

void ProtocolHandler::on_packet(const GamePacket& pkt) {
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.total;
        // The world model is what the bot acts on. A frame from inside a
        // wrapper or from UDP is decoded by layouts written for plain frames,
        // and one opcode number can mean something else there, so it changes
        // nothing here until P0-3 says what these frames are.
        if (!pkt.plain()) {
            ++stats_.held_back;
            return;
        }
    }

    auto decoded = registry_.decode(pkt);

    if (!decoded) {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            if (registry_.known(pkt.opcode)) {
                // Registered but wouldn't parse — the layout is wrong or the
                // packet is a variant. Worth distinguishing from truly unmapped.
                ++stats_.malformed;
            } else {
                ++stats_.unknown;
                ++stats_.unknown_opcodes[pkt.opcode];
            }
        }
        // Callback runs outside the lock: it reaches into the sniffer's own
        // bookkeeping and must not be able to deadlock against stats().
        if (on_unknown_) on_unknown_(pkt);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.decoded;
    }

    apply(*decoded, pkt);
}

void ProtocolHandler::apply(const DecodedPacket& d, const GamePacket& raw) {
    GB_TRACE("%s", d.describe().c_str());

    switch (d.semantic) {
        case Semantic::SelfState:
        case Semantic::SelfMove:
            handle_self_state(d);
            break;

        case Semantic::SelfHpMp:
            handle_self_hpmp(d);
            break;

        case Semantic::SelfDead:
            state_.set_self_dead(true);
            GB_INFO("Player died");
            if (on_death_) on_death_();
            break;

        case Semantic::SelfRevive:
            state_.set_self_dead(false);
            GB_INFO("Player revived");
            break;

        case Semantic::SelfExp:
            state_.set_self_exp(first_int(d, { "exp", "experience" }),
                                static_cast<std::int32_t>(first_int(d, { "level" })));
            break;

        case Semantic::MonsterSpawn:  handle_monster(d, true);  break;
        case Semantic::MonsterUpdate: handle_monster(d, false); break;
        case Semantic::MonsterRemove: handle_monster_remove(d); break;

        case Semantic::MonsterSelected:
            state_.set_selected_monster(
                static_cast<std::int32_t>(first_int(d, { "id", "monster_id", "target_id" })));
            break;

        case Semantic::PlayerSpawn:
        case Semantic::PlayerMove:
            handle_player(d);
            break;

        case Semantic::PlayerRemove:
            state_.remove_player(static_cast<std::int32_t>(first_int(d, { "id", "player_id" })));
            break;

        case Semantic::ItemDrop:   handle_item(d, true);  break;
        case Semantic::ItemRemove: handle_item(d, false); break;

        case Semantic::MapWarp: handle_map_warp(d); break;
        case Semantic::MapData: handle_map_data(d, raw); break;

        case Semantic::PartyUpdate:
        case Semantic::PartyHp:
            handle_party(d);
            break;

        case Semantic::PartyLeave:
            state_.set_party({});
            break;

        case Semantic::Chat: handle_chat(d); break;

        case Semantic::Disconnect:
            GB_WARN("Disconnect packet received");
            break;

        case Semantic::PartyInvite:
        case Semantic::SkillConfirm:
        case Semantic::AttackConfirm:
        case Semantic::BuffApplied:
            // Acknowledged but not modelled yet. They matter for tightening
            // cast timing; the executor currently paces off its own clock.
            break;

        case Semantic::Unknown:
            break;
    }
}

void ProtocolHandler::handle_self_state(const DecodedPacket& d) {
    if (d.has("id") || d.has("player_id")) {
        const auto id = static_cast<std::int32_t>(first_int(d, { "id", "player_id" }));
        if (id != 0) state_.update_self_id(id);
    }

    const bool has_x = d.has("x") || d.has("pos_x");
    const bool has_y = d.has("y") || d.has("pos_y");
    if (has_x && has_y) {
        state_.update_self_position(static_cast<float>(first_float(d, { "x", "pos_x" })),
                                    static_cast<float>(first_float(d, { "y", "pos_y" })));
    }

    if (d.has("hp")) {
        state_.update_self_hp(static_cast<std::int32_t>(first_int(d, { "hp" })),
                              static_cast<std::int32_t>(first_int(d, { "max_hp", "hp_max" })));
    }
    if (d.has("mp")) {
        state_.update_self_mp(static_cast<std::int32_t>(first_int(d, { "mp" })),
                              static_cast<std::int32_t>(first_int(d, { "max_mp", "mp_max" })));
    }
    if (d.has("facing")) {
        state_.set_self_facing(first_int(d, { "facing" }) != 0 ? Facing::Right : Facing::Left);
    }
}

void ProtocolHandler::handle_self_hpmp(const DecodedPacket& d) {
    if (d.has("hp")) {
        state_.update_self_hp(static_cast<std::int32_t>(first_int(d, { "hp" })),
                              static_cast<std::int32_t>(first_int(d, { "max_hp", "hp_max" })));
    }
    if (d.has("mp")) {
        state_.update_self_mp(static_cast<std::int32_t>(first_int(d, { "mp" })),
                              static_cast<std::int32_t>(first_int(d, { "max_mp", "mp_max" })));
    }
}

void ProtocolHandler::handle_monster(const DecodedPacket& d, bool spawn) {
    auto build = [&](const std::unordered_map<std::string, FieldValue>* g) {
        Monster m;
        if (g) {
            m.id     = static_cast<std::int32_t>(DecodedPacket::group_int(*g, "id",
                        DecodedPacket::group_int(*g, "monster_id")));
            m.kind   = static_cast<std::int32_t>(DecodedPacket::group_int(*g, "kind",
                        DecodedPacket::group_int(*g, "type")));
            m.pos.x  = static_cast<float>(DecodedPacket::group_float(*g, "x"));
            m.pos.y  = static_cast<float>(DecodedPacket::group_float(*g, "y"));
            m.hp     = static_cast<std::int32_t>(DecodedPacket::group_int(*g, "hp", -1));
            m.max_hp = static_cast<std::int32_t>(DecodedPacket::group_int(*g, "max_hp", -1));
            m.flying = DecodedPacket::group_int(*g, "flying", 0) != 0;
            m.boss   = DecodedPacket::group_int(*g, "boss", 0) != 0;
            m.name   = DecodedPacket::group_string(*g, "name");
        } else {
            m.id     = static_cast<std::int32_t>(first_int(d, { "id", "monster_id", "entity_id" }));
            m.kind   = static_cast<std::int32_t>(first_int(d, { "kind", "type", "template_id" }));
            m.pos.x  = static_cast<float>(first_float(d, { "x", "pos_x" }));
            m.pos.y  = static_cast<float>(first_float(d, { "y", "pos_y" }));
            m.hp     = static_cast<std::int32_t>(first_int(d, { "hp" }, -1));
            m.max_hp = static_cast<std::int32_t>(first_int(d, { "max_hp", "hp_max" }, -1));
            m.flying = first_int(d, { "flying", "is_fly" }) != 0;
            m.boss   = first_int(d, { "boss", "is_boss" }) != 0;
            m.name   = first_string(d, { "name" });
        }
        m.dead = (m.hp == 0 && m.max_hp > 0);
        return m;
    };

    if (!d.groups.empty()) {
        for (const auto& g : d.groups) {
            Monster m = build(&g);
            if (m.id != 0) state_.upsert_monster(m);
        }
        return;
    }

    Monster m = build(nullptr);
    if (m.id == 0) {
        GB_DEBUG("Monster packet 0x%04X has no usable id field", d.opcode);
        return;
    }
    if (m.dead) {
        state_.remove_monster(m.id);
        return;
    }
    state_.upsert_monster(m);
    if (spawn) GB_TRACE("Monster %d spawned at (%.0f, %.0f)", m.id, m.pos.x, m.pos.y);
}

void ProtocolHandler::handle_monster_remove(const DecodedPacket& d) {
    if (!d.groups.empty()) {
        for (const auto& g : d.groups) {
            const auto id = static_cast<std::int32_t>(
                DecodedPacket::group_int(g, "id", DecodedPacket::group_int(g, "monster_id")));
            if (id != 0) state_.remove_monster(id);
        }
        return;
    }
    const auto id = static_cast<std::int32_t>(first_int(d, { "id", "monster_id", "entity_id" }));
    if (id != 0) state_.remove_monster(id);
}

void ProtocolHandler::handle_player(const DecodedPacket& d) {
    PlayerEntity p;
    p.id    = static_cast<std::int32_t>(first_int(d, { "id", "player_id", "entity_id" }));
    p.pos.x = static_cast<float>(first_float(d, { "x", "pos_x" }));
    p.pos.y = static_cast<float>(first_float(d, { "y", "pos_y" }));
    p.name  = first_string(d, { "name" });
    if (p.id == 0) return;

    // The server broadcasts our own movement too; that's a self update.
    const Self me = state_.self();
    if (me.id != 0 && p.id == me.id) {
        state_.update_self_position(p.pos.x, p.pos.y);
        return;
    }
    state_.upsert_player(p);
}

void ProtocolHandler::handle_item(const DecodedPacket& d, bool drop) {
    const auto id = static_cast<std::int32_t>(first_int(d, { "id", "drop_id", "object_id" }));
    if (id == 0) return;

    if (!drop) {
        state_.remove_item(id);
        return;
    }

    DroppedItem item;
    item.id      = id;
    item.item_id = static_cast<std::int32_t>(first_int(d, { "item_id", "kind", "type" }));
    item.pos.x   = static_cast<float>(first_float(d, { "x", "pos_x" }));
    item.pos.y   = static_cast<float>(first_float(d, { "y", "pos_y" }));
    state_.upsert_item(item);
}

void ProtocolHandler::handle_party(const DecodedPacket& d) {
    if (d.groups.empty()) {
        // Single-member HP tick.
        const auto id = static_cast<std::int32_t>(first_int(d, { "id", "member_id", "player_id" }));
        if (id != 0) {
            state_.update_party_hp(id,
                static_cast<std::int32_t>(first_int(d, { "hp" }, -1)),
                static_cast<std::int32_t>(first_int(d, { "max_hp", "hp_max" }, -1)));
        }
        return;
    }

    std::vector<PartyMember> members;
    members.reserve(d.groups.size());
    for (const auto& g : d.groups) {
        PartyMember m;
        m.id     = static_cast<std::int32_t>(DecodedPacket::group_int(g, "id",
                    DecodedPacket::group_int(g, "member_id")));
        m.name   = DecodedPacket::group_string(g, "name");
        m.hp     = static_cast<std::int32_t>(DecodedPacket::group_int(g, "hp", -1));
        m.max_hp = static_cast<std::int32_t>(DecodedPacket::group_int(g, "max_hp", -1));
        m.leader = DecodedPacket::group_int(g, "leader", 0) != 0;
        if (m.id != 0) members.push_back(std::move(m));
    }
    state_.set_party(members);
}

void ProtocolHandler::handle_chat(const DecodedPacket& d) {
    const std::string from = first_string(d, { "sender", "from", "name" });
    const std::string text = first_string(d, { "text", "message", "msg" });
    if (text.empty()) return;

    state_.push_chat(from, text);
    GB_DEBUG("Chat <%s> %s", from.c_str(), text.c_str());
    if (on_chat_) on_chat_(from, text);
}

void ProtocolHandler::handle_map_warp(const DecodedPacket& d) {
    const std::string name = first_string(d, { "map", "map_name", "name" });
    const auto id = static_cast<std::int32_t>(first_int(d, { "map_id", "id" }));

    state_.set_map(name, id);
    GB_INFO("Map warp -> %s (%d)", name.empty() ? "?" : name.c_str(), id);

    if (d.has("x") && d.has("y")) {
        state_.update_self_position(static_cast<float>(first_float(d, { "x" })),
                                    static_cast<float>(first_float(d, { "y" })));
    }
    if (on_map_warp_) on_map_warp_(name, id);
}

void ProtocolHandler::handle_map_data(const DecodedPacket& d, const GamePacket& raw) {
    MapData map;
    map.name   = first_string(d, { "map", "map_name", "name" });
    map.id     = static_cast<std::int32_t>(first_int(d, { "map_id", "id" }));
    map.width  = static_cast<int>(first_int(d, { "width", "tiles_x" }));
    map.height = static_cast<int>(first_int(d, { "height", "tiles_y" }));
    map.tile_w = static_cast<int>(first_int(d, { "tile_w", "tile_width" }, 32));
    map.tile_h = static_cast<int>(first_int(d, { "tile_h", "tile_height" }, 32));

    // The attribute grid is a raw blob; the layout names it via a bytes field.
    for (const char* key : { "attrs", "tiles", "data", "grid" }) {
        auto it = d.fields.find(key);
        if (it == d.fields.end()) continue;
        if (auto* b = std::get_if<Bytes>(&it->second)) {
            map.attrs = *b;
            break;
        }
    }

    if (map.attrs.empty() && map.width > 0 && map.height > 0) {
        // No named blob: assume the grid is whatever trails the header.
        const std::size_t need = static_cast<std::size_t>(map.width) * map.height;
        if (raw.payload.size() >= need)
            map.attrs.assign(raw.payload.end() - static_cast<std::ptrdiff_t>(need), raw.payload.end());
    }

    if (!map.valid()) {
        GB_WARN("Map data packet 0x%04X did not yield a usable grid (%dx%d, %zu attr bytes)",
                d.opcode, map.width, map.height, map.attrs.size());
        return;
    }

    GB_INFO("Map data: %s %dx%d tiles", map.name.c_str(), map.width, map.height);
    if (on_map_data_) on_map_data_(map);
}

ProtocolHandler::Stats ProtocolHandler::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void ProtocolHandler::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Stats{};
}

} // namespace gobot
