#include "gobot/execution_coordinator.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <chrono>
#include <thread>

namespace gobot {

bool ExecutionCoordinator::request(const std::string& owner, Priority priority) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Critical mode: only Critical-priority work may act.
    if (critical_.load() && priority < Priority::Critical) return false;

    if (held_.load()) {
        // Reclaim a token whose holder appears to have died or hung. Without
        // this one stuck thread silences the whole bot permanently.
        if (now_ms() - acquired_ms_ > kMaxHoldMs) {
            GB_WARN("ExecutionCoordinator: reclaiming stale token from '%s' (held %llums)",
                    owner_.c_str(),
                    static_cast<unsigned long long>(now_ms() - acquired_ms_));
        } else if (priority <= priority_.load()) {
            return false;
        }
        // else: pre-empting a strictly lower priority holder.
    }

    held_.store(true);
    priority_.store(priority);
    owner_ = owner;
    acquired_ms_ = now_ms();
    return true;
}

bool ExecutionCoordinator::request_wait(const std::string& owner, Priority priority,
                                        std::uint32_t timeout_ms) {
    const std::uint64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        if (request(owner, priority)) return true;
        if (now_ms() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ExecutionCoordinator::release(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A pre-empted subsystem may release late. Ignoring the mismatch keeps it
    // from yanking the token out from under whoever pre-empted it.
    if (!held_.load() || owner_ != owner) return;

    held_.store(false);
    priority_.store(Priority::Idle);
    owner_.clear();
}

std::string ExecutionCoordinator::current_owner() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owner_;
}

void ExecutionCoordinator::engage_critical(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        critical_reason_ = reason;
    }
    if (!critical_.exchange(true))
        GB_INFO("Critical mode engaged: %s", reason.c_str());
}

void ExecutionCoordinator::disengage_critical() {
    if (critical_.exchange(false)) {
        std::lock_guard<std::mutex> lock(mutex_);
        GB_INFO("Critical mode released (was: %s)", critical_reason_.c_str());
        critical_reason_.clear();
    }
}

std::string ExecutionCoordinator::critical_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return critical_reason_;
}

} // namespace gobot
