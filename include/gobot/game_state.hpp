#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gobot {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

float distance(const Vec2& a, const Vec2& b);

enum class Facing { Left, Right };

struct Monster {
    std::int32_t  id = 0;
    std::int32_t  kind = 0;          ///< template/species id
    Vec2          pos;
    std::int32_t  hp = -1;           ///< -1 = unknown
    std::int32_t  max_hp = -1;
    bool          flying = false;
    bool          boss = false;
    bool          dead = false;
    std::string   name;
    std::uint64_t last_seen_ms = 0;

    float hp_pct() const {
        if (hp < 0 || max_hp <= 0) return 100.0f;
        return 100.0f * static_cast<float>(hp) / static_cast<float>(max_hp);
    }
};

struct PlayerEntity {
    std::int32_t  id = 0;
    Vec2          pos;
    std::string   name;
    bool          in_party = false;
    std::uint64_t last_seen_ms = 0;
};

struct DroppedItem {
    std::int32_t  id = 0;
    std::int32_t  item_id = 0;
    Vec2          pos;
    std::uint64_t last_seen_ms = 0;
};

struct PartyMember {
    std::int32_t id = 0;
    std::string  name;
    std::int32_t hp = -1;
    std::int32_t max_hp = -1;
    bool         leader = false;
};

struct Self {
    std::int32_t id = 0;
    std::string  name;
    Vec2         pos;
    Facing       facing = Facing::Right;
    std::int32_t hp = 0, max_hp = 0;
    std::int32_t mp = 0, max_mp = 0;
    std::int32_t level = 0;
    std::int64_t exp = 0;
    bool         dead = false;
    bool         jumping = false;
    std::string  map;
    std::int32_t map_id = 0;

    float hp_pct() const { return max_hp > 0 ? 100.0f * hp / max_hp : 100.0f; }
    float mp_pct() const { return max_mp > 0 ? 100.0f * mp / max_mp : 100.0f; }
};

/// The bot's model of the world, rebuilt from sniffed packets.
///
/// Written by the protocol thread, read by the brain and the UI, so every
/// accessor takes the lock and returns a copy. Entities not refreshed within
/// `stale_after_ms` are swept — with packet capture there is no authoritative
/// "despawn" for everything, and a stale monster is worse than a missing one.
class GameState {
public:
    // ---- writes (protocol thread) ----
    void update_self_position(float x, float y);
    void update_self_hp(std::int32_t hp, std::int32_t max_hp);
    void update_self_mp(std::int32_t mp, std::int32_t max_mp);
    void update_self_id(std::int32_t id);
    void set_self_dead(bool dead);
    void set_self_facing(Facing f);
    void set_self_exp(std::int64_t exp, std::int32_t level);
    void set_map(const std::string& name, std::int32_t map_id);

    void upsert_monster(const Monster& m);
    void remove_monster(std::int32_t id);
    void set_selected_monster(std::int32_t id);

    void upsert_player(const PlayerEntity& p);
    void remove_player(std::int32_t id);

    void upsert_item(const DroppedItem& d);
    void remove_item(std::int32_t id);

    void set_party(const std::vector<PartyMember>& members);
    void update_party_hp(std::int32_t id, std::int32_t hp, std::int32_t max_hp);

    void push_chat(const std::string& sender, const std::string& text);

    /// Drop entities older than `stale_after_ms`. Call on a timer.
    void sweep_stale(std::uint64_t stale_after_ms = 8000);

    /// Wipe entities but keep self/map — used on a map warp.
    void clear_entities();
    void reset();

    // ---- reads (any thread) ----
    Self                      self() const;
    std::vector<Monster>      monsters() const;
    std::vector<PlayerEntity> players() const;
    std::vector<DroppedItem>  items() const;
    std::vector<PartyMember>  party() const;
    std::int32_t              selected_monster() const;
    std::optional<Monster>    monster(std::int32_t id) const;
    std::size_t               monster_count() const;

    struct ChatLine {
        std::string   sender, text;
        std::uint64_t timestamp_ms = 0;
    };
    std::vector<ChatLine> recent_chat(std::size_t max = 50) const;

    /// Wall-clock of the last state-changing packet — the liveness signal.
    std::uint64_t last_update_ms() const;

private:
    mutable std::mutex mutex_;

    Self                                          self_;
    std::unordered_map<std::int32_t, Monster>     monsters_;
    std::unordered_map<std::int32_t, PlayerEntity> players_;
    std::unordered_map<std::int32_t, DroppedItem> items_;
    std::vector<PartyMember>                      party_;
    std::vector<ChatLine>                         chat_;
    std::int32_t                                  selected_monster_ = 0;
    std::uint64_t                                 last_update_ms_ = 0;

    static constexpr std::size_t kMaxChat = 200;

    void touch();   // caller holds the lock
};

} // namespace gobot
