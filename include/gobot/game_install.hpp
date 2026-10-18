#pragma once

#include "gobot/input_manager.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gobot {

/// A program running on this machine, as far as can be told from outside it.
struct RunningProgram {
    unsigned long pid = 0;
    std::string   name;     ///< its file name, "Game.exe"
    /// The full path of the file it was started from, UTF-8. Empty when that
    /// cannot be read: a program running with more rights than this one.
    std::string   path;
};

/// The one rule for whether a program is the game: by the full path of the
/// file it runs from, never by its name.
///
/// Game.exe is what a great many programs are called. Everything that ended
/// the game, followed it, brought it forward or waited for it took any of
/// them for it, so ending a crashed client could end somebody's other game,
/// and the keeper could follow one. The game is the program at a path the
/// game is known to be installed at:
///
///   - where its installer put it, as Windows' list of installed programs
///     records it: the DisplayIcon of "Ghost Online VTC", the same record
///     the game's map files are found by -- while the program is there;
///   - the usual place, C:\VTCZone\Games\Ghost_Online_PC_VTC\Game.exe,
///     whenever the game is there, record or not: a record left behind by a
///     folder deleted or moved since must not hide it;
///   - and wherever the program behind the game's own window was started
///     from -- class GhostSoul, titled GhostOnlineVTC, which nothing else
///     draws -- remembered for the rest of the session, so that a copy
///     installed somewhere else is still the game once its window has gone.
///
/// Two paths are compared as text first, and then, when only the folders
/// they go through differ, as files on disk: a game reached through a
/// junction or a subst drive runs, as Windows tells it, from where the file
/// really is, which is not the path the record names.
///
/// A program whose path cannot be read is not the game. GoBot runs as
/// administrator, and that reads the game's path: XIGNCODE3 takes away the
/// right to read the game's memory, not the right to ask where it runs from
/// (docs/overhaul.md), and GoBot's log has named the game's program, read off
/// that path, every time it found the game's window.
class GameInstall {
public:
    /// What puts a path into the one form two paths are compared in.
    struct Drives {
        /// Each drive and the device behind it, as Windows names them:
        /// {"C:", "\\Device\\HarddiskVolume3"}. A path in the kernel's form,
        /// which starts with the device, is read as the drive's.
        std::vector<std::pair<std::string, std::string>> devices;

        /// The long form of a path with 8.3 short names in it -- GHOST_~1
        /// for Ghost_Online_PC_VTC -- or "" when it cannot be had. Windows
        /// keeps a program's path the way it was started, and one started by
        /// its short name runs from a path that does not look like its own.
        std::function<std::string(const std::string&)> long_name;

        /// Which file or folder a path in the one form names on disk, as
        /// Windows tells them apart -- its volume and its number there -- or
        /// "" when it cannot be opened. How a path through a junction or a
        /// subst drive is known for the file it leads to. Never asked across
        /// the network -- a share, or a drive mapped to one: a server that
        /// does not answer would hold every look up.
        std::function<std::string(const std::string&)> file_id;

        /// This machine's.
        static Drives here();
    };

    /// `path` in the one form: unquoted, backslashes only and one at a time,
    /// no `.` or `..`, without the prefixes Windows puts on a long path or a
    /// share, the kernel's device as the drive it is, and long names for
    /// short ones. Case is left as it came; `same_file` ignores it.
    static std::string plain(const std::string& path, const Drives& drives);

    /// Whether two paths name the same file, whatever form each came in and
    /// whatever the case. Two that differ as text are asked of the disk
    /// (`Drives::file_id`) when they end in the same name -- a junction or a
    /// subst drive changes the folders on the way, never the program's own
    /// name -- and are different files when either cannot be opened. An
    /// empty path names nothing.
    static bool same_file(const std::string& a, const std::string& b, const Drives& drives);

    /// The game's program, out of what a DisplayIcon says: a path, perhaps
    /// quoted, perhaps with ",0" after it for which icon. When it names
    /// something other than a program -- an icon file -- the Game.exe beside
    /// it. Empty when it says nothing.
    static std::string program_in_icon(const std::string& icon);

    /// The full path of the game's program in each place it is known to be.
    /// Usually one.
    std::vector<std::string> programs;

    /// Where the game is installed on this machine, as far as is known now.
    static GameInstall here();

    /// What `here` makes of what it finds: the programs the installer's
    /// records name, each only when `there` says it is on disk; the usual
    /// place whenever `there` says the game is in it; and the programs seen
    /// behind the game's window, which need no asking. Apart from `here` so
    /// that a test can say what is on its pretend disk.
    static GameInstall found(const std::vector<std::string>& records,
                             const std::vector<std::string>& seen_behind_window,
                             const std::function<bool(const std::string&)>& there,
                             const Drives& drives);

    /// Whether a program running from `path` is the game.
    bool is_game(const std::string& path, const Drives& drives) const;
    bool is_game(const RunningProgram& program, const Drives& drives) const {
        return is_game(program.path, drives);
    }

    /// Whether a program running from `path` sits beside the game -- in the
    /// very folder of one of `programs` -- the way its launcher,
    /// GhostOnlineVTC.exe, does. A program of the launcher's name anywhere
    /// else is somebody else's, and is neither asked to close nor ended.
    bool beside_the_game(const std::string& path, const Drives& drives) const;

    /// Of `running`, the programs that are the game.
    std::vector<RunningProgram> games_among(const std::vector<RunningProgram>& running,
                                            const Drives& drives) const;

    /// Whether `name` is the file name of the game's program, whatever the
    /// case -- whether a program asked for by that name has to be the game.
    /// Game.exe always is, known install or not.
    bool names_the_game(const std::string& name) const;

private:
    void add(const std::string& program, const Drives& drives);
};

/// The full path of the file process `pid` was started from, UTF-8, or ""
/// when it cannot be read.
std::string program_path(unsigned long pid);

/// The programs running now that are called `name`, whatever the case, each
/// with its path when that can be read.
std::vector<RunningProgram> programs_called(const std::string& name);

/// Of the programs running now that are called `name`, the ones that are the
/// game.
std::vector<RunningProgram> game_programs(const std::string& name, const GameInstall& install);

/// Whether the game is running now: a program called `name` that is the game.
bool game_running(const std::string& name, const GameInstall& install);

/// The game's windows: the windows of the programs called `name` that are
/// the game, as InputManager::windows_of finds them -- visible, any size.
std::vector<InputManager::WindowInfo> game_windows(const std::string& name,
                                                   const GameInstall& install);

/// End the program numbered `pid` if it is the game. Asked again of the very
/// handle that ends it, so that a program which has ended since it was looked
/// at, its number taken by another, is never ended in its place. Refused
/// under the "no real input" switch.
bool end_game_program(unsigned long pid, const GameInstall& install);

/// End every program called `name` that is the game, and no other: how many
/// were ended. Refused under the "no real input" switch.
int end_game(const std::string& name, const GameInstall& install);

/// End every program called `name` that sits beside the game -- the game's
/// launcher -- and no other: how many were ended. Each is asked again of the
/// very handle that ends it, as the game is; one anywhere else is left
/// running and said so. Refused under the "no real input" switch.
int end_beside_the_game(const std::string& name, const GameInstall& install);

} // namespace gobot
