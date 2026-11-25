#pragma once

namespace gobot {

/// The character's HP and MP from the packets, in percent, as
/// MacroEngine::set_vitals takes them; -1 for no reading.
struct SelfVitals {
    float hp = -1.0f;
    float mp = -1.0f;
};

} // namespace gobot
