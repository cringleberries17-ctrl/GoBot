#include "gobot/game_state.hpp"
#include "gobot/common.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace gobot {

float distance(const Vec2& a, const Vec2& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

void GameState::touch() { last_update_ms_ = now_ms(); }

// ------------------------------------------------------------------- writes

void GameState::update_self_position(float x, float y) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Infer facing from horizontal movement when the protocol doesn't carry
    // it. A sub-pixel jitter shouldn't flip the character around.
    if (std::abs(x - self_.pos.x) > 1.0f)
        self_.facing = (x > self_.pos.x) ? Facing::Right : Facing::Left;
    self_.pos = { x, y };
    touch();
}

void GameState::update_self_hp(std::int32_t hp, std::int32_t max_hp) {
    std::lock_guard<std::mutex> lock(mutex_);
    self_.hp = hp;
    if (max_hp > 0) self_.max_hp = max_hp;
    if (hp <= 0) self_.dead = true;
    touch();
}

void GameState::update_self_mp(std::int32_t mp, std::int32_t max_mp) {
    std::lock_guard<std::mutex> lock(mutex_);
    self_.mp = mp;
    if (max_mp > 0) self_.max_mp = max_mp;
    touch();
}

void GameState::update_self_id(std::int32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    self_.id = id;
    touch();
}

void GameState::set_self_dead(bool dead) {
    std::lock_guard<std::mutex> lock(mutex_);
    self_.dead = dead;
    if (!dead && self_.hp <= 0 && self_.max_hp > 0) self_.hp = self_.max_hp;
    touch();
}

void GameState::set_self_facing(Facing f) {
    std::lock_guard<std::mutex> lock(mutex_);
    self_.facing = f;
}

void GameState::set_self_exp(std::int64_t exp, std::int32_t level) {
    std::lock_guard<std::mutex> lock(mutex_);
    self_.exp = exp;
    if (level > 0) self_.level = level;
    touch();
}

void GameState::set_map(const std::string& name, std::int32_t map_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool changed = (self_.map != name) || (self_.map_id != map_id);
    self_.map = name;
    self_.map_id = map_id;
    if (changed) {
        // Entities are map-scoped; carrying them across a warp would have the
        // bot swinging at monsters that are no longer in the world.
        monsters_.clear();
        players_.clear();
        items_.clear();
        selected_monster_ = 0;
    }
    touch();
}

void GameState::upsert_monster(const Monster& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = monsters_.find(m.id);
    if (it == monsters_.end()) {
        Monster copy = m;
        copy.last_seen_ms = now_ms();
        monsters_.emplace(m.id, std::move(copy));
    } else {
        Monster& dst = it->second;
        // Merge rather than replace: an update packet carrying only position
        // must not wipe a known HP, and vice versa.
        dst.pos = m.pos;
        if (m.hp >= 0)      dst.hp = m.hp;
        if (m.max_hp > 0)   dst.max_hp = m.max_hp;
        if (m.kind != 0)    dst.kind = m.kind;
        if (!m.name.empty()) dst.name = m.name;
        dst.flying = m.flying || dst.flying;
        dst.boss   = m.boss || dst.boss;
        dst.dead   = m.dead;
        dst.last_seen_ms = now_ms();
    }
    touch();
}

void GameState::remove_monster(std::int32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    monsters_.erase(id);
    if (selected_monster_ == id) selected_monster_ = 0;
    touch();
}

void GameState::set_selected_monster(std::int32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    selected_monster_ = id;
}

void GameState::upsert_player(const PlayerEntity& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& dst = players_[p.id];
    dst = p;
    dst.last_seen_ms = now_ms();
    touch();
}

void GameState::remove_player(std::int32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    players_.erase(id);
    touch();
}

void GameState::upsert_item(const DroppedItem& d) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& dst = items_[d.id];
    dst = d;
    dst.last_seen_ms = now_ms();
    touch();
}

void GameState::remove_item(std::int32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(id);
    touch();
}

void GameState::set_party(const std::vector<PartyMember>& members) {
    std::lock_guard<std::mutex> lock(mutex_);
    party_ = members;
    touch();
}

void GameState::update_party_hp(std::int32_t id, std::int32_t hp, std::int32_t max_hp) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& m : party_) {
        if (m.id == id) {
            m.hp = hp;
            if (max_hp > 0) m.max_hp = max_hp;
            break;
        }
    }
    touch();
}

void GameState::push_chat(const std::string& sender, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    chat_.push_back({ sender, text, now_ms() });
    if (chat_.size() > kMaxChat)
        chat_.erase(chat_.begin(), chat_.begin() + static_cast<std::ptrdiff_t>(chat_.size() - kMaxChat));
    touch();
}

void GameState::sweep_stale(std::uint64_t stale_after_ms) {
    const std::uint64_t cutoff = now_ms();
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = monsters_.begin(); it != monsters_.end();) {
        it = (cutoff > it->second.last_seen_ms + stale_after_ms) ? monsters_.erase(it) : std::next(it);
    }
    for (auto it = players_.begin(); it != players_.end();) {
        it = (cutoff > it->second.last_seen_ms + stale_after_ms) ? players_.erase(it) : std::next(it);
    }
    for (auto it = items_.begin(); it != items_.end();) {
        it = (cutoff > it->second.last_seen_ms + stale_after_ms) ? items_.erase(it) : std::next(it);
    }
    if (selected_monster_ != 0 && !monsters_.count(selected_monster_)) selected_monster_ = 0;
}

void GameState::clear_entities() {
    std::lock_guard<std::mutex> lock(mutex_);
    monsters_.clear();
    players_.clear();
    items_.clear();
    selected_monster_ = 0;
    touch();
}

void GameState::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    self_ = Self{};
    monsters_.clear();
    players_.clear();
    items_.clear();
    party_.clear();
    chat_.clear();
    selected_monster_ = 0;
    last_update_ms_ = 0;
}

// -------------------------------------------------------------------- reads

Self GameState::self() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return self_;
}

std::vector<Monster> GameState::monsters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Monster> out;
    out.reserve(monsters_.size());
    for (const auto& [id, m] : monsters_) {
        if (!m.dead) out.push_back(m);
    }
    return out;
}

std::vector<PlayerEntity> GameState::players() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PlayerEntity> out;
    out.reserve(players_.size());
    for (const auto& [id, p] : players_) out.push_back(p);
    return out;
}

std::vector<DroppedItem> GameState::items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DroppedItem> out;
    out.reserve(items_.size());
    for (const auto& [id, d] : items_) out.push_back(d);
    return out;
}

std::vector<PartyMember> GameState::party() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return party_;
}

std::int32_t GameState::selected_monster() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return selected_monster_;
}

std::optional<Monster> GameState::monster(std::int32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = monsters_.find(id);
    if (it == monsters_.end()) return std::nullopt;
    return it->second;
}

std::size_t GameState::monster_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return monsters_.size();
}

std::vector<GameState::ChatLine> GameState::recent_chat(std::size_t max) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (chat_.size() <= max) return chat_;
    return { chat_.end() - static_cast<std::ptrdiff_t>(max), chat_.end() };
}

std::uint64_t GameState::last_update_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_update_ms_;
}

} // namespace gobot
