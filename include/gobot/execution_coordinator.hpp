#pragma once

#include "gobot/command.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace gobot {

/// Arbitrates between subsystems that all want to press keys.
///
/// Combat, healing, buffing, looting and recovery run on separate threads and
/// will happily fight over the keyboard. Every one of them must hold this
/// token before sending input. Higher priority pre-empts lower; equal priority
/// is first-come. Without this the bot walks left and right in the same tick.
class ExecutionCoordinator {
public:
    /// Try to take the token for `owner` at `priority`.
    /// Non-blocking: returns false if something of equal-or-higher priority
    /// holds it. Callers back off and retry on their next tick.
    bool request(const std::string& owner, Priority priority);

    /// Blocking variant, capped at `timeout_ms`.
    bool request_wait(const std::string& owner, Priority priority, std::uint32_t timeout_ms);

    /// Release the token. A mismatched owner is ignored, so a late release
    /// from a pre-empted subsystem can't steal the token from its successor.
    void release(const std::string& owner);

    Priority    current_priority() const { return priority_.load(); }
    std::string current_owner() const;
    bool        held() const { return held_.load(); }

    /// Critical mode locks everything below Priority::Critical out entirely —
    /// used while a blocking dialog is on screen and stray keys would be
    /// typed into it.
    void engage_critical(const std::string& reason);
    void disengage_critical();
    bool critical_active() const { return critical_.load(); }
    std::string critical_reason() const;

    /// RAII holder. Check `ok()` before acting.
    class Token {
    public:
        Token(ExecutionCoordinator& c, std::string owner, Priority p)
            : coord_(&c), owner_(std::move(owner)) {
            ok_ = coord_->request(owner_, p);
        }
        ~Token() { if (ok_) coord_->release(owner_); }

        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        Token(Token&& o) noexcept : coord_(o.coord_), owner_(std::move(o.owner_)), ok_(o.ok_) { o.ok_ = false; }

        bool ok() const { return ok_; }
        explicit operator bool() const { return ok_; }

    private:
        ExecutionCoordinator* coord_;
        std::string           owner_;
        bool                  ok_ = false;
    };

    Token acquire(const std::string& owner, Priority p) { return Token(*this, owner, p); }

private:
    mutable std::mutex     mutex_;
    std::atomic<bool>      held_{false};
    std::atomic<Priority>  priority_{Priority::Idle};
    std::string            owner_;
    std::uint64_t          acquired_ms_ = 0;

    std::atomic<bool>      critical_{false};
    std::string            critical_reason_;

    /// A subsystem that dies holding the token would deadlock the bot, so a
    /// hold older than this is forcibly reclaimed.
    static constexpr std::uint64_t kMaxHoldMs = 30'000;
};

} // namespace gobot
