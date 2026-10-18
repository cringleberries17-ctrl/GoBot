#pragma once

#include "gobot/config.hpp"

#include <string>
#include <vector>

namespace gobot {

/// Notices that the game has stopped being a game, and says why.
///
/// `SessionManager` already answers "is the character in the world?" by
/// looking at the HUD, and that is the right question for a session that is
/// merely interrupted -- a loading screen, a full-screen dialog, a death. It
/// is the wrong question for a client that has fallen over, because all three
/// of those look identical to it: no HUD. An unattended run then sits waiting
/// out a grace period for a window that is never coming back.
///
/// This asks the other question. Three things can have happened:
///
///   Gone      the process is not running. Watched live: left alone at the
///             character select, this client closes itself after about two
///             minutes, with no dialog and nothing written to its log.
///   Hung      the window is there and the program has stopped answering.
///             Windows knows this and will say so; nothing else will, because
///             a frozen client keeps drawing its last frame.
///   ErrorBox  a message box is up. That is the one that carries the error
///             code, and the code is read out of the dialog rather than
///             matched against a picture of it -- a code nobody has seen
///             before still reads correctly, and there are a lot of them.
///
/// Reading the box instead of recognising it is the point. A crash dialog is
/// an ordinary Win32 dialog: its text lives in child controls and Windows will
/// hand it over for the asking. Matching a template would mean having a
/// picture of every error this game can produce, and would still say nothing
/// about which one it was.
class CrashWatch {
public:
    enum class Trouble {
        None,
        Gone,       ///< the process has exited
        Hung,       ///< the window is not answering
        ErrorBox,   ///< a message box is up, probably with a code in it
        /// The client runs and answers, but nobody has been in the world for
        /// minutes. Never one of look()'s: the keeper's watchdog, which looks
        /// at the screen, says so (gobot/stall_watch.hpp).
        Stalled,
    };
    static const char* trouble_name(Trouble t);

    struct Look {
        Trouble     what = Trouble::None;
        std::string detail;         ///< what to tell a person
        std::string code;           ///< the error code, when the text had one
        void*       dialog = nullptr;  ///< the box to dismiss, when there is one
        int         minutes = 0;    ///< Stalled: how long nobody was in the world
        bool        bad() const { return what != Trouble::None; }
    };

    /// The Vietnamese for what happened, for the simple screen: "Game đã
    /// tắt", "Game bị treo", "Game hiện bảng lỗi", "Game không vào được thế
    /// giới 5 phút".
    static std::string trouble_vi(const Look& seen);

    struct Settings {
        /// The game, and the launcher that starts it. Both are ended on the
        /// way to logging back in: leaving the launcher behind means the next
        /// crossing finds a window it did not put there.
        std::string game_process = "Game.exe";
        std::string launcher_process = "GhostOnlineVTC.exe";

        /// Windows' own crash reporter. It puts up "Game.exe has stopped
        /// working" on behalf of a program that can no longer speak for
        /// itself, so a crash can leave no dialog of the game's at all.
        std::string reporter_process = "WerFault.exe";

        /// How long a window may stay unresponsive before it counts as hung.
        ///
        /// Not instant, because it should not be. A client loading a map, or
        /// one on a machine that is swapping, stops pumping messages for a few
        /// seconds at a time while nothing is wrong, and restarting the game
        /// every time that happens is worse than the problem.
        int hung_grace_s = 20;

        /// Give up after this many restarts. Beyond it, a person is needed.
        ///
        /// A client that crashes once is a crash. A client that crashes five
        /// times in a row is something that restarting will not fix -- a bad
        /// patch, a full disk, an account that has been locked -- and a loop
        /// that keeps reconnecting to a server refusing it is indistinguishable
        /// from an attack on that server.
        int max_restarts = 5;

        /// Wait this long after ending the client before starting again, so
        /// the server sees the session close rather than two of them at once.
        int cooldown_s = 15;

        void load(const IniFile& ini);
        void save(IniFile& ini) const;
    };

    explicit CrashWatch(Settings s = {});

    void set_settings(const Settings& s) { settings_ = s; }
    Settings settings() const { return settings_; }

    /// One look. Cheap: no capture, no template, just Win32 asking after a
    /// handful of windows.
    Look look();

    /// End the client, and the launcher with it.
    ///
    /// Dismisses the error box first if there is one, because a process with a
    /// modal dialog up will not exit on its own, and a crash reporter left
    /// running holds the executable open for the next start.
    bool shut_down(const Look& seen);

    /// What a keeper does about one look -- the GUI's and the command line's
    /// alike, so the two cannot drift apart.
    enum class Move {
        Watch,      ///< the client is fine: keep watching
        Follow,     ///< fine, and a start was waiting on it: follow the game, start what was asked for
        Start,      ///< gone, and never up since the keeper began: cross in -- a start, not a restart
        Restart,    ///< it fell over: end what is left of it, cool down, cross back in
    };
    /// `seen_running`: the client has been up since the keeper began.
    /// `start_pending`: the keeper was started to open the game -- BẮT ĐẦU
    /// with the game closed -- and what was asked for has not started yet. A
    /// game that came up by itself in the meantime, by hand or by another
    /// tool, is then followed rather than crossed into a second time.
    static Move next_move(const Look& seen, bool seen_running, bool start_pending);

    /// Pull an error code out of a dialog's text.
    ///
    /// Recognises the shapes these actually come in: a hex code (0x8007000E),
    /// a number after the word for "error" in English or Vietnamese ("Error
    /// 134", "Lỗi 134", "mã lỗi: 134"), and a bare parenthesised number at the
    /// end of a line. Returns "" when the text has nothing that looks like
    /// one, which is not a failure -- plenty of dialogs carry no code.
    static std::string code_in(const std::string& text);

    /// Whether this window is a message box rather than an ordinary window.
    static bool looks_like_a_box(const std::string& window_class,
                                 const std::string& title,
                                 const std::string& text);

private:
    Settings settings_;

    /// When the window was first seen not answering. Zero when it is fine.
    long long hung_since_ms_ = 0;
};

} // namespace gobot
