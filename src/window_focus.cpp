#include "gobot/window_focus.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>

#include <windows.h>

namespace gobot {

WindowFocusMonitor& WindowFocusMonitor::instance() {
    static WindowFocusMonitor inst;
    return inst;
}

WindowFocusMonitor::~WindowFocusMonitor() { stop(); }

void WindowFocusMonitor::start(void* target_hwnd) {
    if (running_.exchange(true)) {
        set_target(target_hwnd);
        return;
    }
    target_.store(target_hwnd);
    state_.store(FocusState::Unknown);
    thread_ = std::thread(&WindowFocusMonitor::monitor_loop, this);
    GB_DEBUG("WindowFocusMonitor started");
}

void WindowFocusMonitor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    GB_DEBUG("WindowFocusMonitor stopped");
}

void WindowFocusMonitor::set_target(void* hwnd) {
    target_.store(hwnd);
    state_.store(FocusState::Unknown);   // force a fresh transition
}

int WindowFocusMonitor::add_listener(Listener cb) {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    const int token = next_token_++;
    listeners_.emplace_back(token, std::move(cb));
    return token;
}

void WindowFocusMonitor::remove_listener(int token) {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
                       [token](const auto& p) { return p.first == token; }),
        listeners_.end());
}

std::string WindowFocusMonitor::foreground_title() const {
    HWND fg = GetForegroundWindow();
    if (!fg) return {};
    char buf[512]{};
    GetWindowTextA(fg, buf, sizeof(buf) - 1);
    return buf;
}

void WindowFocusMonitor::emit(FocusState s) {
    std::vector<Listener> snapshot;
    {
        std::lock_guard<std::mutex> lock(listener_mutex_);
        snapshot.reserve(listeners_.size());
        for (const auto& [token, cb] : listeners_) snapshot.push_back(cb);
    }
    // Callbacks run outside the lock: a listener that adds or removes another
    // listener would otherwise deadlock.
    for (const auto& cb : snapshot) cb(s);
}

void WindowFocusMonitor::monitor_loop() {
    FocusState    candidate = FocusState::Unknown;
    std::uint64_t candidate_since = 0;

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));

        void* target = target_.load();
        if (!target) continue;

        HWND hwnd = static_cast<HWND>(target);
        if (!IsWindow(hwnd)) {
            if (state_.load() != FocusState::Unfocused) {
                GB_WARN("Target window vanished");
                state_.store(FocusState::Unfocused);
                emit(FocusState::Unfocused);
            }
            continue;
        }

        const FocusState observed =
            (GetForegroundWindow() == hwnd) ? FocusState::Focused : FocusState::Unfocused;

        const std::uint64_t now = now_ms();

        if (observed != candidate) {
            candidate = observed;
            candidate_since = now;
            continue;
        }

        // Alt-tabbing walks focus through intermediate windows; only report a
        // transition once the new state has held for the debounce window.
        if (observed != state_.load() && now - candidate_since >= static_cast<std::uint64_t>(debounce_ms_)) {
            state_.store(observed);
            GB_DEBUG("Focus -> %s", observed == FocusState::Focused ? "focused" : "unfocused");
            emit(observed);
        }
    }
}

} // namespace gobot
