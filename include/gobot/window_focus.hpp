#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gobot {

enum class FocusState { Unknown, Focused, Unfocused };

/// Polls the foreground window and reports transitions for the target HWND.
///
/// Debounced: alt-tabbing through windows produces a burst of transitions, and
/// we only want to suspend the bot when focus has genuinely settled elsewhere.
class WindowFocusMonitor {
public:
    using Listener = std::function<void(FocusState)>;

    static WindowFocusMonitor& instance();

    void start(void* target_hwnd);
    void stop();
    bool running() const { return running_.load(); }

    void set_target(void* hwnd);

    FocusState state() const { return state_.load(); }
    bool       focused() const { return state_.load() == FocusState::Focused; }

    /// Returns a token for remove_listener.
    int  add_listener(Listener cb);
    void remove_listener(int token);

    std::string foreground_title() const;

    void set_poll_interval_ms(int ms) { poll_ms_ = ms; }
    void set_debounce_ms(int ms)      { debounce_ms_ = ms; }

private:
    WindowFocusMonitor() = default;
    ~WindowFocusMonitor();

    void monitor_loop();
    void emit(FocusState s);

    std::atomic<void*>      target_{nullptr};
    std::atomic<FocusState> state_{FocusState::Unknown};
    std::atomic<bool>       running_{false};
    std::thread             thread_;

    std::mutex                            listener_mutex_;
    std::vector<std::pair<int, Listener>> listeners_;
    int                                   next_token_ = 1;

    int poll_ms_ = 100;
    int debounce_ms_ = 250;
};

} // namespace gobot
