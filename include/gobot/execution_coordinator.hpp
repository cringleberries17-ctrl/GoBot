#pragma once

#include "gobot/command.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gobot {

/// The keys a bag job's key window lets through (2026.11.10, the owner: "Khi
/// dọn rác thì cần ra các skill thuộc phím chữ cái", "cả khi mua HP và MP cũng
/// cần ra skill", "có tính phím space"; 2026.11.11: "Home End PgUp PgDn Ins
/// Del"): the macro's own keys on a letter, Space or one of those six, each
/// once -- skills, buffs and potions alike -- and those of them a potion is
/// on, after whose key-down the bag may change. Built from the macro's keys in
/// memory (MacroEngine::pass_keys).
struct PassKeys {
    std::vector<std::uint32_t> keys;
    std::vector<std::uint32_t> potions;     ///< of `keys`, those a potion is on
    /// Of `potions`, those an HP potion is on (2026.11.11, the owner: "Mua
    /// luôn, bơm xen"): HP is drunk between the job's own clicks on them.
    std::vector<std::uint32_t> hp_potions;
    /// The highest mark of the macro's HP potions that are on, on whatever
    /// key -- let through or not; 0 for none.
    int hp_mark = 0;
    /// Every HP potion the macro may choose while the job runs on -- each on,
    /// with its mark at half `hp_mark` or over -- among `hp_potions` (the
    /// review's HP-1): the macro presses only the tier the reading is under,
    /// so one of them on a digit would leave HP undrunk from its mark down.
    /// Only then do the job's HP gates go by `hp_mark` (bag::hp_gates).
    bool hp_all_through = false;
    bool any() const { return !keys.empty(); }
    bool has(std::uint32_t vk) const;
    bool potion(std::uint32_t vk) const;
};

/// Whether a key may ever go through a key window: a letter A to Z, Space, or
/// (2026.11.11, the owner: "Home End PgUp PgDn Ins Del") exactly PgUp 0x21,
/// PgDn 0x22, End 0x23, Home 0x24, Insert 0x2D and Delete 0x2E -- by its whole
/// code, nothing with more folded into it, and never a range: the arrows
/// 0x25-0x28 (0x28 is ↓) and 0x29-0x2C lie between them. Never a digit (a
/// number in a quantity box; the owner: "Không cho phím số"), the number pad,
/// Enter or Esc (they answer a dialog), Tab, Backspace, an arrow, a modifier,
/// an F-key or an OEM key.
bool pass_key_allowed(std::uint32_t vk);
/// Of those, the six that are no letter or Space -- Home, End, PgUp, PgDn,
/// Insert, Delete: what the game does with them on the bag or the shop is not
/// known, so a bag job lets the game settle a moment after one went through
/// (ExecutionCoordinator::pass_nav_ms, BagCleaner::kNavSettleMs).
bool pass_key_nav(std::uint32_t vk);

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

    /// Take the token only when nobody holds it: no holder taken over,
    /// however low, and none reclaimed, however old -- one taken over goes on
    /// pressing. Refused below Critical in critical mode, as request() is.
    /// For the drop's Ctrl + ↓ (InputManager::drop_ctrl_down): held at
    /// Combat, nothing but a Critical hold can take it while Ctrl is down.
    bool request_free(const std::string& owner, Priority priority);

    /// Release the token. A mismatched owner is ignored, so a late release
    /// from a pre-empted subsystem can't steal the token from its successor.
    void release(const std::string& owner);

    Priority    current_priority() const { return priority_.load(); }
    std::string current_owner() const;
    bool        held() const { return held_.load(); }

    /// Critical mode locks everything below Priority::Critical out entirely —
    /// used while a blocking dialog is on screen and stray keys would be
    /// typed into it. A hold taken afresh, another's taken in its place, or
    /// one ended takes any key window with it (disarm_pass_keys).
    void engage_critical(const std::string& reason);
    void disengage_critical();
    bool critical_active() const { return critical_.load(); }
    std::string critical_reason() const;

    // ---- a bag job's key window (2026.11.10) ----
    //
    // A hold refuses every key below Critical, so a Dọn rác pass or a potion
    // shop round held every skill, potion and buff off for its seconds. The
    // owner asked for the macro's letter keys and Space to go on meanwhile --
    // and Home, End, PgUp, PgDn, Insert and Delete (2026.11.11) -- never during
    // a charm run. A window of its own, never keyed on the hold's
    // reason: the job arms it with its set for its hold, opens it on a look
    // with nothing up, and shuts it -- the keys let through seen up -- before
    // each click and key of its own. request() is untouched: under the hold
    // the hunter, a walk, the drop and the jumps stand as before.

    /// The keys `keys` (those pass_key_allowed lets) for this hold, the window
    /// shut: false, nothing armed, with no hold on or no such key in `keys`.
    bool arm_pass_keys(const PassKeys& keys);
    /// The window open: false when nothing is armed for this hold -- a stop
    /// took it away, or the hold was ended or taken by another.
    bool open_pass_keys();
    /// Shut: no key goes through from now on, and each that went is waited
    /// for, `wait_ms` at most, to be up -- its press over, its token back.
    /// False when one still is.
    bool close_pass_keys(std::uint32_t wait_ms);
    /// Shut, and the keys forgotten, with no wait: a job's end, a stop, F12.
    void disarm_pass_keys();
    bool pass_keys_armed() const;
    bool pass_keys_open() const;
    /// Whether `vk` is one the window armed for this hold lets through when
    /// it is open, shut or not now: a hold on, armed, `vk` in its set and
    /// allowed (2026.11.11: what a bag job's potion break asks of the potion
    /// due -- one the window never lets through is not waited for).
    bool pass_key_armed(std::uint32_t vk) const;
    /// Whether `vk` goes through now: a hold on, a window armed for it and
    /// open, `vk` in its set and allowed (pass_key_allowed).
    bool lets_through(std::uint32_t vk) const;
    /// How request_key came out. Busy: another holds the token. Shut: a
    /// window armed for this hold, but shut, or `vk` not one it lets through.
    /// NoWindow: no hold, or none armed -- the hold refuses every key.
    enum class KeyWay { Taken, Busy, Shut, NoWindow };
    /// The token at Combat for `owner`'s key `vk`, under the hold, when
    /// lets_through(vk) and nobody holds it: what the macro asks once
    /// request() has refused it. Never reclaims or takes over.
    KeyWay request_key(const std::string& owner, std::uint32_t vk);
    /// When a potion key let through last went down, on now_ms(); 0: none
    /// since the window was armed.
    std::uint64_t pass_potion_ms() const;
    /// When a key let through that is no letter or Space (pass_key_nav) last
    /// came up -- its token given back, its key-up sent -- on now_ms(); 0:
    /// none since the window was armed (2026.11.11).
    std::uint64_t pass_nav_ms() const;
    /// Since it was armed, for the job's log, each key by its name
    /// (InputManager::key_name): "Z 5, X 4, PgUp 1 through, 3 times open
    /// (1840 ms)", or "nothing through, ...".
    std::string pass_keys_line() const;

    /// RAII holder. Check `ok()` before acting.
    class Token {
    public:
        Token(ExecutionCoordinator& c, std::string owner, Priority p)
            : coord_(&c), owner_(std::move(owner)) {
            ok_ = coord_->request(owner_, p);
        }
        /// Taken by request_free: only when nobody holds the token.
        struct OnlyFree {};
        Token(ExecutionCoordinator& c, std::string owner, Priority p, OnlyFree)
            : coord_(&c), owner_(std::move(owner)) {
            ok_ = coord_->request_free(owner_, p);
        }
        /// Taken by request_key: key `vk` through a bag job's key window,
        /// `way` told how that came out.
        struct ByKey {};
        Token(ExecutionCoordinator& c, std::string owner, std::uint32_t vk, KeyWay* way, ByKey)
            : coord_(&c), owner_(std::move(owner)) {
            const KeyWay w = coord_->request_key(owner_, vk);
            if (way != nullptr) *way = w;
            ok_ = w == KeyWay::Taken;
        }
        ~Token() { if (ok_) coord_->release(owner_); }

        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        Token(Token&& o) noexcept : coord_(o.coord_), owner_(std::move(o.owner_)), ok_(o.ok_) { o.ok_ = false; }
        Token& operator=(Token&& o) noexcept {
            if (this != &o) {
                if (ok_) coord_->release(owner_);
                coord_ = o.coord_;
                owner_ = std::move(o.owner_);
                ok_ = o.ok_;
                o.ok_ = false;
            }
            return *this;
        }

        bool ok() const { return ok_; }
        explicit operator bool() const { return ok_; }

    private:
        ExecutionCoordinator* coord_;
        std::string           owner_;
        bool                  ok_ = false;
    };

    Token acquire(const std::string& owner, Priority p) { return Token(*this, owner, p); }
    Token acquire_free(const std::string& owner, Priority p) { return Token(*this, owner, p, Token::OnlyFree{}); }
    Token acquire_key(const std::string& owner, std::uint32_t vk, KeyWay* way = nullptr) {
        return Token(*this, owner, vk, way, Token::ByKey{});
    }

private:
    /// disarm_pass_keys, with mutex_ held.
    void disarm_locked();

    mutable std::mutex     mutex_;
    std::atomic<bool>      held_{false};
    std::atomic<Priority>  priority_{Priority::Idle};
    std::string            owner_;
    std::uint64_t          acquired_ms_ = 0;

    std::atomic<bool>      critical_{false};
    std::string            critical_reason_;

    // The key window, under mutex_: armed for the hold on now, open, its
    // keys; the token's holder came through it, and with which key; the last
    // potion through; the last of the six no letter came up; and what went
    // through since it was armed, for pass_keys_line.
    bool                   pass_armed_ = false;
    bool                   pass_open_ = false;
    PassKeys               pass_keys_;
    bool                   through_ = false;
    std::uint32_t          through_vk_ = 0;
    std::uint64_t          pass_potion_ms_ = 0;
    std::uint64_t          pass_nav_ms_ = 0;
    std::uint64_t          pass_opened_ms_ = 0;
    std::uint64_t          pass_open_total_ms_ = 0;
    int                    pass_openings_ = 0;
    std::vector<std::pair<std::uint32_t, int>> pass_taken_;

    /// A subsystem that dies holding the token would deadlock the bot, so a
    /// hold older than this is forcibly reclaimed.
    static constexpr std::uint64_t kMaxHoldMs = 30'000;
};

/// A bag job's key window for as long as it lives (2026.11.10): armed with
/// `keys` as it begins -- when there are any, under the job's hold -- and
/// however the job ends, shut with the keys let through waited for up to
/// kUpWaitMs, its line said in the log, and disarmed: never left open past
/// the job, nor for whatever holds the keys after it.
class PassKeysScope {
public:
    PassKeysScope(ExecutionCoordinator& c, const PassKeys& keys, const char* job);
    ~PassKeysScope();
    PassKeysScope(const PassKeysScope&) = delete;
    PassKeysScope& operator=(const PassKeysScope&) = delete;
    bool armed() const { return armed_; }

    static constexpr std::uint32_t kUpWaitMs = 600;

private:
    ExecutionCoordinator& coord_;
    const char*           job_;
    bool                  armed_ = false;
};

} // namespace gobot
