#include "gobot/execution_coordinator.hpp"
#include "gobot/common.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace gobot {

bool PassKeys::has(std::uint32_t vk) const { return std::find(keys.begin(), keys.end(), vk) != keys.end(); }

bool PassKeys::potion(std::uint32_t vk) const {
    return std::find(potions.begin(), potions.end(), vk) != potions.end();
}

bool pass_key_allowed(std::uint32_t vk) {
    // 'A'..'Z' are VK 0x41..0x5A and Space 0x20, with nothing above them; and
    // the six by their own codes.
    return (vk >= 'A' && vk <= 'Z') || vk == 0x20 || pass_key_nav(vk);
}

bool pass_key_nav(std::uint32_t vk) {
    // PgUp 0x21, PgDn 0x22, End 0x23, Home 0x24, Insert 0x2D, Delete 0x2E --
    // each by its code: 0x25-0x28 between them are the arrows, ↓ among them,
    // and 0x29-0x2C Select, Print, Execute and Print Screen.
    return vk == 0x21 || vk == 0x22 || vk == 0x23 || vk == 0x24 || vk == 0x2D || vk == 0x2E;
}

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
        // One of the six let through, taken over: its press is over as far as
        // the window is concerned, and a job settles from now.
        if (through_ && pass_key_nav(through_vk_)) pass_nav_ms_ = std::max<std::uint64_t>(now_ms(), 1);
    }

    held_.store(true);
    priority_.store(priority);
    owner_ = owner;
    acquired_ms_ = now_ms();
    through_ = false;
    return true;
}

bool ExecutionCoordinator::request_free(const std::string& owner, Priority priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (critical_.load() && priority < Priority::Critical) return false;
    // Whoever holds it, at whatever priority and for however long: a holder
    // taken over is not stopped, and its key would go out inside the drop's
    // Ctrl.
    if (held_.load()) return false;
    held_.store(true);
    priority_.store(priority);
    owner_ = owner;
    acquired_ms_ = now_ms();
    through_ = false;
    return true;
}

ExecutionCoordinator::KeyWay ExecutionCoordinator::request_key(const std::string& owner, std::uint32_t vk) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!critical_.load() || !pass_armed_) return KeyWay::NoWindow;
    if (!pass_open_ || !pass_keys_.has(vk) || !pass_key_allowed(vk)) return KeyWay::Shut;
    // Nobody's taken over, however old: the one holding it is pressing.
    if (held_.load()) return KeyWay::Busy;
    held_.store(true);
    priority_.store(Priority::Combat);
    owner_ = owner;
    acquired_ms_ = now_ms();
    through_ = true;
    through_vk_ = vk;
    if (pass_keys_.potion(vk)) pass_potion_ms_ = std::max<std::uint64_t>(acquired_ms_, 1);
    const auto it = std::find_if(pass_taken_.begin(), pass_taken_.end(),
                                 [vk](const std::pair<std::uint32_t, int>& t) { return t.first == vk; });
    if (it != pass_taken_.end()) ++it->second;
    else pass_taken_.emplace_back(vk, 1);
    return KeyWay::Taken;
}

bool ExecutionCoordinator::arm_pass_keys(const PassKeys& keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    disarm_locked();
    // What went through is counted afresh for each job.
    pass_taken_.clear();
    pass_openings_ = 0;
    pass_open_total_ms_ = 0;
    pass_potion_ms_ = 0;
    pass_nav_ms_ = 0;
    if (!critical_.load()) return false;
    // Only what may ever go through, each once: the rest is never asked about.
    PassKeys kept;
    for (const std::uint32_t vk : keys.keys) {
        if (!pass_key_allowed(vk) || kept.has(vk)) continue;
        kept.keys.push_back(vk);
        if (keys.potion(vk)) kept.potions.push_back(vk);
    }
    if (!kept.any()) return false;
    pass_keys_ = std::move(kept);
    pass_armed_ = true;
    return true;
}

bool ExecutionCoordinator::open_pass_keys() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!critical_.load() || !pass_armed_) return false;
    if (!pass_open_) {
        pass_open_ = true;
        pass_opened_ms_ = now_ms();
        ++pass_openings_;
    }
    return true;
}

bool ExecutionCoordinator::close_pass_keys(std::uint32_t wait_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pass_open_) pass_open_total_ms_ += now_ms() - pass_opened_ms_;
        pass_open_ = false;
    }
    // From here no key goes through; the one that went is let finish.
    const std::uint64_t until = now_ms() + wait_ms;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!through_ || !held_.load()) return true;
        }
        if (now_ms() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ExecutionCoordinator::disarm_pass_keys() {
    std::lock_guard<std::mutex> lock(mutex_);
    disarm_locked();
}

void ExecutionCoordinator::disarm_locked() {
    if (pass_open_) pass_open_total_ms_ += now_ms() - pass_opened_ms_;
    pass_open_ = false;
    pass_armed_ = false;
    pass_keys_ = PassKeys{};
}

bool ExecutionCoordinator::pass_keys_armed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pass_armed_ && critical_.load();
}

bool ExecutionCoordinator::pass_keys_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pass_armed_ && pass_open_ && critical_.load();
}

bool ExecutionCoordinator::pass_key_armed(std::uint32_t vk) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return critical_.load() && pass_armed_ && pass_keys_.has(vk) && pass_key_allowed(vk);
}

bool ExecutionCoordinator::lets_through(std::uint32_t vk) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return critical_.load() && pass_armed_ && pass_open_ && pass_keys_.has(vk) && pass_key_allowed(vk);
}

std::uint64_t ExecutionCoordinator::pass_potion_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pass_potion_ms_;
}

std::uint64_t ExecutionCoordinator::pass_nav_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pass_nav_ms_;
}

std::string ExecutionCoordinator::pass_keys_line() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string keys;
    // By name: Home by its code's character would be '$', PgUp '!'.
    for (const auto& t : pass_taken_)
        keys += (keys.empty() ? "" : ", ") + InputManager::key_name(t.first) + " " + std::to_string(t.second);
    const std::uint64_t open_ms = pass_open_total_ms_ + (pass_open_ ? now_ms() - pass_opened_ms_ : 0);
    return (keys.empty() ? std::string("nothing") : keys) + " through, " + std::to_string(pass_openings_) +
           (pass_openings_ == 1 ? " time open (" : " times open (") + std::to_string(open_ms) + " ms)";
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

    // One of the six let through, its key-up sent: when the job's settle
    // after it begins (pass_nav_ms).
    if (through_ && pass_key_nav(through_vk_)) pass_nav_ms_ = std::max<std::uint64_t>(now_ms(), 1);
    held_.store(false);
    priority_.store(Priority::Idle);
    owner_.clear();
    through_ = false;
}

std::string ExecutionCoordinator::current_owner() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owner_;
}

void ExecutionCoordinator::engage_critical(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A hold taken afresh, or another's in the place of one, starts with
        // no key window: one armed for the last is never this one's.
        if (!critical_.load() || critical_reason_ != reason) disarm_locked();
        critical_reason_ = reason;
    }
    if (!critical_.exchange(true))
        GB_INFO("Critical mode engaged: %s", reason.c_str());
}

void ExecutionCoordinator::disengage_critical() {
    std::lock_guard<std::mutex> lock(mutex_);
    // The window goes with the hold it was armed for.
    disarm_locked();
    if (critical_.exchange(false)) {
        GB_INFO("Critical mode released (was: %s)", critical_reason_.c_str());
        critical_reason_.clear();
    }
}

PassKeysScope::PassKeysScope(ExecutionCoordinator& c, const PassKeys& keys, const char* job)
    : coord_(c), job_(job != nullptr ? job : "job") {
    armed_ = keys.any() && coord_.arm_pass_keys(keys);
}

PassKeysScope::~PassKeysScope() {
    if (!armed_) return;
    const bool up = coord_.close_pass_keys(kUpWaitMs);
    GB_INFO("Trash: %s's key window: %s%s", job_, coord_.pass_keys_line().c_str(),
            up ? "" : " -- a key let through was still down as it ended");
    coord_.disarm_pass_keys();
}

std::string ExecutionCoordinator::critical_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return critical_reason_;
}

} // namespace gobot
