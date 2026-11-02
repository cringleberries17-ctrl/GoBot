#pragma once

#include <cstdint>

namespace gobot {

/// When to bring the game back to the front while the bot runs: "Luôn đưa
/// game lên trước khi bot đang chạy" on the simple screen, refocus in [Simple]
/// in app.ini.
///
/// The keys only land while the game is in front -- the macro, the hunter
/// and the packet bot refuse every key with anything else there, and the
/// chat, the quest and the bag wait for it -- so a window that takes the
/// focus during a run stops the bot as surely as DỪNG does, and says nothing.
/// The player asked for the game back at once, whatever took it, GoBot's own
/// window included (2026-10-27); it used to wait for nobody to have touched
/// the mouse or keyboard for 30 s. So: as soon as it is found behind, and
/// again every kEveryMs while it stays there.
///
/// "As soon as" is after the click or the key that took it, never in the
/// middle of one: not while a mouse button is held, and not until the mouse
/// and keyboard have been still for kSettleMs. GoBot's own window shows why.
/// Taken from it in the frame its button comes up, ImGui hears the release
/// and the focus going in one batch, clears the mouse, and DỪNG is never
/// pressed. kSettleMs is a few frames; past kSettleMaxMs of waiting for the
/// stillness -- something nudging the input without a pause -- it goes ahead
/// with no button held.
///
/// One of GoBot's key boxes waiting for a key, with GoBot in front, is a
/// click not finished either: the key it waits for has to reach GoBot, and
/// while it waits F10 and F11 are not heard (hotkeys.cpp). Held off for
/// kListenMaxMs at most -- nothing is pressed in the game meanwhile, so the
/// caller lets HP under the potion's mark, or 50%, end it sooner -- and the
/// box gives up its wait once the game is in front, however it got there
/// (GuiApp::keep_game_in_front). The key the box takes is held down for the
/// keeper until it reads up, and the stillness is waited for after that: the
/// game must not get it down.
///
/// Windows can refuse: a UAC prompt, the lock screen. After
/// kRefusalsBeforeBackoff refusals in a row only every kBackoffMs, as each
/// try holds the window that asks for up to about 150 ms: the look at
/// whether the program in front answers, and the 50 ms of
/// InputManager::focus_window it asks for. A game or a program in front
/// that has stopped answering is not asked at all (skipped): that is no
/// refusal of Windows', and the looks keep their pace.
class FrontKeeper {
public:
    static constexpr std::uint64_t kEveryMs = 700;
    static constexpr std::uint64_t kBackoffMs = 3000;
    static constexpr int           kRefusalsBeforeBackoff = 3;
    static constexpr std::uint64_t kSettleMs = 200;
    static constexpr std::uint64_t kSettleMaxMs = 3000;
    static constexpr std::uint64_t kListenMaxMs = 10'000;

    /// What one look sees.
    struct Look {
        bool on = false;            ///< the option is ticked
        bool running = false;       ///< the bot runs: anything DỪNG LẠI stops, and the packet bot
        bool window = false;        ///< there is a game window
        bool in_front = false;      ///< and it is the window in front
        bool mouse_down = false;    ///< a mouse button is held, wherever
        bool key_down = false;      ///< a key a key box has just taken is still down: its key-up is waited for
        bool listening = false;     ///< a key box of GoBot's waits for a key, GoBot in front
        bool crossing = false;      ///< the keeper is logging back in: its windows are its own to bring forward
        /// How long ago the mouse or keyboard was last used, anywhere.
        std::uint64_t input_age_ms = UINT64_MAX;
    };

    /// Whether to try bringing the game to the front at `now_ms`. When it
    /// says yes, tell tried() how it went.
    bool due(const Look& l, std::uint64_t now_ms) {
        if (!l.on || !l.running || !l.window || l.in_front || l.crossing) {
            refusals_ = 0;
            settling_ms_ = 0;
            listening_ms_ = 0;
            return false;
        }
        // A button held is a click or a drag under way, however long: the
        // wait for stillness starts over once it comes up. So is a key just
        // taken by a key box: a key held past the settle time sends no input
        // of its own until its auto-repeat, and the game would get it down.
        if (l.mouse_down || l.key_down) {
            settling_ms_ = 0;
            return false;
        }
        if (!l.listening) {
            listening_ms_ = 0;
        } else {
            if (listening_ms_ == 0) listening_ms_ = now_ms;
            if (now_ms - listening_ms_ < kListenMaxMs) {
                settling_ms_ = 0;
                return false;
            }
        }
        if (settling_ms_ == 0) settling_ms_ = now_ms;
        if (l.input_age_ms < kSettleMs && now_ms - settling_ms_ < kSettleMaxMs) return false;
        const std::uint64_t gap = refusals_ >= kRefusalsBeforeBackoff ? kBackoffMs : kEveryMs;
        return tried_ms_ == 0 || now_ms - tried_ms_ >= gap;
    }

    void tried(bool brought, std::uint64_t now_ms) {
        tried_ms_ = now_ms;
        refusals_ = brought ? 0 : refusals_ + 1;
    }

    /// Due, and not tried: a window was not answering. Not counted as a
    /// refusal; the next look comes at the pace the tries had.
    void skipped(std::uint64_t now_ms) { tried_ms_ = now_ms; }

    /// Refusals in a row since it was last brought forward or found in front.
    int refusals() const { return refusals_; }

private:
    std::uint64_t tried_ms_ = 0;
    std::uint64_t settling_ms_ = 0;    ///< since when it has waited behind, no button held; 0 when not
    std::uint64_t listening_ms_ = 0;   ///< since when a key box has waited, GoBot in front; 0 when not
    int           refusals_ = 0;
};

} // namespace gobot
