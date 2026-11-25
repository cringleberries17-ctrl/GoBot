#pragma once

namespace gobot {

/// How much a hold of the keys matters (ExecutionCoordinator): a higher one
/// takes the token from a lower, an equal one waits its turn.
enum class Priority : int {
    Idle     = 0,
    Movement = 10,
    Combat   = 20,
    Buff     = 30,
    Loot     = 15,
    Heal     = 80,
    Critical = 100,     ///< a dialog on screen -- pre-empts everything
};

} // namespace gobot
