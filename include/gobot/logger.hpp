#pragma once

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gobot {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

const char* to_string(LogLevel l);

struct LogLine {
    LogLevel    level = LogLevel::Info;
    std::string text;       ///< timestamp + level tag already prefixed
};

/// Process-wide logger. Writes to stderr and, optionally, to a file.
/// Every sink is guarded by one mutex so worker threads can log freely.
class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel lvl) { level_ = lvl; }
    LogLevel level() const { return level_; }

    /// Mirror output into `path`. Empty string detaches the file sink.
    ///
    /// `fresh` starts the file anew: what was in it is moved to old_name(),
    /// in place of the one before, so the file holds this session and the
    /// .old one the last -- a log sent with a problem report then carries
    /// those two, not weeks of names, chat lines and paths. A file that cannot
    /// be moved (another copy of GoBot has it open) is added to, as without.
    bool set_file(const std::string& path, bool fresh = false);

    /// Where `fresh` keeps the session before: "gobot_gui.log" becomes
    /// "gobot_gui.old.log", and a name with no extension gains ".old".
    static std::string old_name(const std::string& path);

    void write(LogLevel lvl, const std::string& msg);

    /// Retain the last `max` lines in memory for the GUI's log panel. Zero
    /// disables retention. Console and file sinks are unaffected.
    void set_ring_capacity(std::size_t max);

    /// Snapshot of retained lines, oldest first.
    std::vector<LogLine> ring() const;
    void clear_ring();

    template <typename... Args>
    void logf(LogLevel lvl, const char* fmt, Args&&... args) {
        if (lvl < level_) return;
        char buf[2048];
        std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
        write(lvl, buf);
    }

private:
    Logger() = default;
    ~Logger();

    LogLevel            level_ = LogLevel::Info;
    std::FILE*          file_  = nullptr;
    mutable std::mutex  mutex_;

    std::deque<LogLine> ring_;
    std::size_t         ring_capacity_ = 0;
};

} // namespace gobot

// Printf-style macros. The variadic tail is optional, hence __VA_ARGS__ with
// the MSVC-friendly comma-elision idiom (/Zc:preprocessor not required).
#define GB_TRACE(...) ::gobot::Logger::instance().logf(::gobot::LogLevel::Trace, __VA_ARGS__)
#define GB_DEBUG(...) ::gobot::Logger::instance().logf(::gobot::LogLevel::Debug, __VA_ARGS__)
#define GB_INFO(...)  ::gobot::Logger::instance().logf(::gobot::LogLevel::Info,  __VA_ARGS__)
#define GB_WARN(...)  ::gobot::Logger::instance().logf(::gobot::LogLevel::Warn,  __VA_ARGS__)
#define GB_ERROR(...) ::gobot::Logger::instance().logf(::gobot::LogLevel::Error, __VA_ARGS__)
