#pragma once

#include "gobot/common.hpp"
#include "gobot/game_state.hpp"
#include "gobot/opcode_registry.hpp"
#include "gobot/pathfinder.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gobot {

/// Applies decoded packets to the world model.
///
/// The bridge between "bytes off the wire" and "the bot knows a monster is at
/// (x, y)". Everything here is driven by Semantic, never by raw opcode
/// numbers, so re-mapping an opcode in protocol.ini takes effect without any
/// code change.
class ProtocolHandler {
public:
    ProtocolHandler(const OpcodeRegistry& registry, GameState& state);

    /// Feed a batch straight from PacketCapture.
    void on_packets(const std::vector<GamePacket>& packets);
    void on_packet(const GamePacket& pkt);

    // ---- notifications for higher layers ----
    using MapWarpCallback   = std::function<void(const std::string& map, std::int32_t id)>;
    using DeathCallback     = std::function<void()>;
    using ChatCallback      = std::function<void(const std::string& from, const std::string& text)>;
    using MapDataCallback   = std::function<void(const MapData&)>;
    using UnknownCallback   = std::function<void(const GamePacket&)>;

    void set_map_warp_callback(MapWarpCallback cb) { on_map_warp_ = std::move(cb); }
    void set_death_callback(DeathCallback cb)      { on_death_ = std::move(cb); }
    void set_chat_callback(ChatCallback cb)        { on_chat_ = std::move(cb); }
    void set_map_data_callback(MapDataCallback cb) { on_map_data_ = std::move(cb); }

    /// Fires for opcodes with no entry in protocol.ini. This is the hook the
    /// discovery workflow hangs off — unmapped traffic is exactly what you
    /// still have to identify.
    void set_unknown_callback(UnknownCallback cb)  { on_unknown_ = std::move(cb); }

    struct Stats {
        std::uint64_t total = 0;
        std::uint64_t decoded = 0;
        std::uint64_t unknown = 0;
        std::uint64_t malformed = 0;
        std::map<std::uint16_t, std::uint64_t> unknown_opcodes;
    };
    Stats stats() const;
    void  reset_stats();

private:
    void apply(const DecodedPacket& d, const GamePacket& raw);

    void handle_self_state(const DecodedPacket& d);
    void handle_self_hpmp(const DecodedPacket& d);
    void handle_monster(const DecodedPacket& d, bool spawn);
    void handle_monster_remove(const DecodedPacket& d);
    void handle_player(const DecodedPacket& d);
    void handle_item(const DecodedPacket& d, bool drop);
    void handle_party(const DecodedPacket& d);
    void handle_chat(const DecodedPacket& d);
    void handle_map_warp(const DecodedPacket& d);
    void handle_map_data(const DecodedPacket& d, const GamePacket& raw);

    const OpcodeRegistry& registry_;
    GameState&            state_;

    MapWarpCallback on_map_warp_;
    DeathCallback   on_death_;
    ChatCallback    on_chat_;
    MapDataCallback on_map_data_;
    UnknownCallback on_unknown_;

    mutable std::mutex stats_mutex_;
    Stats              stats_;
};

} // namespace gobot
