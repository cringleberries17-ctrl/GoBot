#pragma once

#include "gobot/screen_capture.hpp"

#include <string>

namespace gobot {

/// Whether the character is in the world, and the programs around the game.
///
/// How it tells the first: the HUD. A character in the world has a health bar
/// and a mana bar, drawn as a matched pair at the top of the window, and
/// nothing else the client ever shows looks like that -- not the launcher, not
/// the channel list, not the character select, not a disconnect dialog, not a
/// loading screen. Asking "are both bars there?" is therefore the whole
/// question, and it needs no template, no packet and no knowledge of which
/// dialog the client happens to be showing. That last part is what makes it
/// worth preferring to recognising dialogs: a dialog this has never seen still
/// reads as "not in the world".
class SessionManager {
public:
    SessionManager() = delete;

    /// Decide what a single frame shows: true for the HUD's two bars.
    static bool hud_present(const Frame& f);

    /// Whether a process of this name is running. Name only, no path, case
    /// insensitive -- "VTCZone.exe". Not for the game: plenty of programs are
    /// called Game.exe, and `game_running` (gobot/game_install.hpp) asks
    /// where one runs from.
    static bool process_running(const std::string& exe_name);

    /// Start a program and return whether it started. No console window, and
    /// this does not wait for it. Refused under the "no real input" switch.
    static bool start_program(const std::string& path);
};

} // namespace gobot
