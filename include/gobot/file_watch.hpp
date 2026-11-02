#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace gobot {

/// When the file at `path` (UTF-8) was last written, as Windows counts it:
/// 100 ns steps since 1601. 0 when there is no such file, or it cannot be
/// asked.
std::uint64_t file_write_stamp(const std::string& path);

/// When a panel that is drawn every frame should read its config file again.
///
/// The tabs that show what a file says -- configs/login.ini on Tự đăng nhập,
/// party.ini, app.ini and login.ini on the full screen's party and keeper
/// panels -- parsed it on every frame they were open: up to sixty file opens
/// a second, and inside a VMware guest every one of them costs. A copy read
/// once would go stale, though: gobot_cli, the simple screen's other tabs and
/// a person with a text editor write these files too. So the file is read
/// again only when its last-write time has moved, and that time is asked for
/// no more than once per `gap_ms` -- 1000 by default, a new value; the
/// routines panel's own check (GuiApp::draw_routines_panel) runs every 250.
/// A hand edit shows up at most that late. What a tab writes itself, it
/// forgets at once (forget), and the next frame reads the file back.
///
/// No clock and no file of its own: `due` is handed the time and a way to ask
/// for the stamp, so the tests drive it with theirs.
class FileWatch {
public:
    static constexpr std::uint64_t kGapMs = 1000;

    FileWatch() = default;
    explicit FileWatch(std::uint64_t gap_ms) : gap_ms_(gap_ms) {}

    /// Whether to read the file now, at `now_ms`: the first time, after
    /// forget(), and when `stamp` -- asked at most once per gap -- says
    /// something other than at the last read.
    bool due(std::uint64_t now_ms, const std::function<std::uint64_t()>& stamp);

    /// The same for the file at `path`, on now_ms()'s clock.
    bool due(const std::string& path);

    /// What was read is out of date -- the tab has just written the file --
    /// so the next due() says yes, whatever the time.
    void forget() { read_ = false; }

    /// The read came to nothing -- a file a virus scan or a sync is holding
    /// for a moment will not open -- so read it again at the next look, a gap
    /// from now, even though its time has not moved. Every frame, as before,
    /// would be sixty failed opens a second for as long as the file is held.
    void retry() { retry_ = true; }

private:
    std::uint64_t gap_ms_ = kGapMs;
    std::uint64_t stamp_ = 0;       ///< the file's time when it was last read
    std::uint64_t checked_ms_ = 0;   ///< when the time was last asked
    bool          read_ = false;     ///< something has been read, and not forgotten
    bool          retry_ = false;
};

} // namespace gobot
