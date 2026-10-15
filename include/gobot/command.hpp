#pragma once

#include "gobot/game_state.hpp"

#include <cstdint>
#include <string>

namespace gobot {

/// The complete vocabulary the brain uses to drive the character.
///
/// Keeping this an explicit enum rather than free-form callbacks means the
/// executor can reason about a command before running it — dedupe, prioritise,
/// drop stale movement when a newer one arrives — and every action the bot can
/// take is enumerable in one place.
enum class CommandType {
    None,
    MoveLeft,
    MoveRight,
    StopMove,
    Jump,
    DoubleJump,
    JumpDown,
    FaceLeft,
    FaceRight,
    UseSkill,
    NormalAttack,
    StartAttack,
    StopAttack,
    UseBuff,
    UseItem,
    Loot,
    TargetMonster,
    PathfindTo,
    StopPathfind,
    Chat,
    KeyDown,
    KeyUp,
    KeyPress,
    Wait,
};

const char* to_string(CommandType t);

enum class Priority : int {
    Idle     = 0,
    Movement = 10,
    Combat   = 20,
    Buff     = 30,
    Loot     = 15,
    Heal     = 80,
    Critical = 100,     ///< death recovery — pre-empts everything
};

struct Command {
    CommandType   type = CommandType::None;
    Priority      priority = Priority::Idle;

    std::uint32_t keycode = 0;        ///< UseSkill / UseBuff / Key*
    std::int32_t  target_id = 0;      ///< TargetMonster
    Vec2          position;           ///< PathfindTo
    std::uint32_t duration_ms = 0;    ///< Wait / KeyDown hold
    std::string   text;               ///< Chat
    int           slot = -1;          ///< originating skill/buff slot
    std::uint64_t issued_ms = 0;
    /// Drop the command if it is still queued this long after issue. Stale
    /// movement toward a monster that has since died is worse than no move.
    std::uint32_t ttl_ms = 0;

    static Command move_left(Priority p = Priority::Movement);
    static Command move_right(Priority p = Priority::Movement);
    static Command stop_move(Priority p = Priority::Movement);
    static Command jump(Priority p = Priority::Movement);
    static Command skill(std::uint32_t vk, int slot, Priority p = Priority::Combat);
    static Command buff(std::uint32_t vk, int slot);
    static Command heal(std::uint32_t vk);
    static Command face(bool left);
    static Command pathfind(Vec2 pos);
    static Command wait(std::uint32_t ms);
    static Command chat(const std::string& text);

    bool expired(std::uint64_t now_ms) const {
        return ttl_ms != 0 && now_ms > issued_ms + ttl_ms;
    }
};

} // namespace gobot
