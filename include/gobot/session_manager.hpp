#pragma once

#include "gobot/config.hpp"
#include "gobot/screen_capture.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gobot {

/// Keeps an unattended session alive, and ends one deliberately.
///
/// Everything else in this project assumes somebody is sitting there: the
/// character is in the world, the window is up, the macro is running. A
/// session left overnight meets none of those assumptions. The client is
/// disconnected and shows a dialog; or it has exited and there is no window at
/// all; or it is back at the channel list waiting to be told where to go. In
/// every one of those the macro carries on pressing keys at nothing, and
/// whoever comes back in the morning finds eight hours of that.
///
/// What this does is notice. It watches for the one thing that distinguishes
/// being in the world from every other state, and says so.
///
/// How it tells: the HUD. A character in the world has a health bar and a mana
/// bar, drawn as a matched pair at the top of the window, and nothing else the
/// client ever shows looks like that -- not the launcher, not the channel
/// list, not the character select, not a disconnect dialog, not a loading
/// screen. Asking "are both bars there?" is therefore the whole question, it
/// costs one capture every couple of seconds, and it needs no template, no
/// packet and no knowledge of which dialog the client happens to be showing.
/// That last part is what makes it worth preferring to recognising dialogs:
/// a dialog this has never seen still reads as "not in the world".
class SessionManager {
public:
    enum class State {
        Unknown,        ///< not looked yet
        NoWindow,       ///< the game is not running, or has no window
        NotPlaying,     ///< a window, but no HUD: loading, a menu, a dialog
        Playing,        ///< in the world, HUD readable
    };
    static const char* state_name(State s);

    enum class Event {
        StateChanged,
        SessionLost,      ///< was playing, and has not been for the grace period
        SessionRegained,
        NeedsAPerson,     ///< cannot be recovered from here -- say so and stop
        StoppingOnTimer,
        ShuttingDown,
    };
    static const char* event_name(Event e);

    struct Settings {
        /// How often to look.
        int poll_ms = 2000;

        /// How long the HUD may be missing before the session counts as lost.
        ///
        /// Generous on purpose. Changing map, dying, reviving and opening a
        /// full-screen dialog all hide the HUD for a few seconds while nothing
        /// is actually wrong, and treating those as a disconnect would mean a
        /// bot that restarts itself every time it walks through a door.
        int lost_grace_s = 25;

        /// Stop the run after this many minutes. 0 means never.
        int stop_after_minutes = 0;

        /// Shut the machine down once the run has stopped.
        ///
        /// Off, and stays off unless somebody writes it down. The countdown is
        /// announced and can be called off from a console with `shutdown /a`,
        /// which is the only reason this is allowed to exist at all.
        bool shutdown_when_done = false;
        int  shutdown_delay_s = 60;

        void load(const IniFile& ini);
        void save(IniFile& ini) const;
    };

    SessionManager();
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    void set_settings(const Settings& s);
    Settings settings() const;

    /// The window to watch. Re-findable: when the client exits and is started
    /// again the handle changes, so this is looked up again rather than held.
    void set_window_hints(const std::string& title, const std::string& window_class,
                          const std::string& process_name);

    /// Called from the watcher thread whenever something happens. Keep it
    /// quick and do not call back into this object from it.
    void on_event(std::function<void(Event, const std::string&)> fn);

    void start();
    void stop();
    bool running() const { return running_.load(); }

    State state() const { return state_.load(); }

    /// Whether the macro should be pressing keys right now. False whenever the
    /// character is not in the world, and false once the run has stopped.
    bool should_act() const { return state_.load() == State::Playing && !finished_.load(); }

    struct Stats {
        std::uint64_t looks = 0;
        std::uint64_t captures_failed = 0;
        std::uint64_t sessions_lost = 0;
        std::uint64_t seconds_playing = 0;
        std::uint64_t seconds_watching = 0;
    };
    Stats stats() const;

    /// Decide what a single frame shows, without any of the threading. This is
    /// the whole of the judgement and is exposed so it can be tested against a
    /// picture instead of against a running game.
    static bool hud_present(const Frame& f);

    // ---- processes ----

    /// Whether a process of this name is running. Name only, no path, case
    /// insensitive -- "VTCZone.exe". Not for the game: plenty of programs are
    /// called Game.exe, and `game_running` (gobot/game_install.hpp) asks
    /// where one runs from.
    static bool process_running(const std::string& exe_name);

    /// Start a program and return whether it started. No console window, and
    /// this does not wait for it. Refused under the "no real input" switch.
    static bool start_program(const std::string& path);

    /// Ask the machine to shut down after a delay, announced to the user.
    ///
    /// Done by handing the request to Windows' own `shutdown` command rather
    /// than calling the API directly, for one reason: that gives a visible
    /// countdown the person can call off with `shutdown /a`. An unattended bot
    /// turning a machine off is the single most annoying thing in this file if
    /// it is ever wrong, so it had better be interruptible. Refused under the
    /// "no real input" switch.
    static bool request_shutdown(int delay_s, const std::string& why);

private:
    void loop();
    void fire(Event e, const std::string& detail);

    std::atomic<bool>  running_{false};
    std::atomic<bool>  finished_{false};
    std::atomic<State> state_{State::Unknown};
    std::thread        thread_;

    mutable std::mutex mutex_;
    Settings           settings_;
    std::string        title_, class_, process_;
    std::function<void(Event, const std::string&)> callback_;

    mutable std::mutex stats_mutex_;
    Stats              stats_;
};

} // namespace gobot
