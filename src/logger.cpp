#include "gobot/logger.hpp"
#include "gobot/common.hpp"

#include <chrono>
#include <ctime>

namespace gobot {
namespace {

const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

std::string wall_clock() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t   = system_clock::to_time_t(now);
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm{};
    localtime_s(&tm, &t);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

} // namespace

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool Logger::set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (path.empty()) return true;

    file_ = std::fopen(path.c_str(), "a");
    if (!file_) return false;

    std::fprintf(file_, "\n=== GoBot session start %s ===\n", wall_clock().c_str());
    std::fflush(file_);
    return true;
}

void Logger::write(LogLevel lvl, const std::string& msg) {
    if (lvl < level_) return;

    const std::string line = wall_clock() + " [" + level_tag(lvl) + "] " + msg;

    std::lock_guard<std::mutex> lock(mutex_);
    std::fprintf(stderr, "%s\n", line.c_str());
    if (lvl >= LogLevel::Warn) std::fflush(stderr);

    if (file_) {
        std::fprintf(file_, "%s\n", line.c_str());
        std::fflush(file_);
    }

    if (ring_capacity_ > 0) {
        ring_.push_back({ lvl, line });
        while (ring_.size() > ring_capacity_) ring_.pop_front();
    }
}

void Logger::set_ring_capacity(std::size_t max) {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_capacity_ = max;
    while (ring_.size() > ring_capacity_) ring_.pop_front();
    if (max == 0) ring_.clear();
}

std::vector<LogLine> Logger::ring() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return { ring_.begin(), ring_.end() };
}

void Logger::clear_ring() {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_.clear();
}

const char* to_string(LogLevel l) { return level_tag(l); }

} // namespace gobot
