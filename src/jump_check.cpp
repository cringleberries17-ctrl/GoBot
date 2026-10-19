#include "gobot/jump_check.hpp"
#include "gobot/common.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace gobot {
namespace {

constexpr std::uint32_t kSpace = 0x20;      // VK_SPACE: the game's attack

/// How long another job's key is waited for before the check gives up: one
/// key press of anybody's is over well inside it.
constexpr std::uint64_t kTokenWaitMs = 500;

} // namespace

std::uint64_t JumpCheck::clock_now() const {
    return clock_.now_ms ? clock_.now_ms() : now_ms();
}

void JumpCheck::pause(std::uint32_t ms) const {
    if (clock_.sleep_ms) clock_.sleep_ms(ms);
    else                 std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool JumpCheck::look(int& y) const {
    if (!eyes_.player) return false;
    Nameplate me;
    bool fresh = false;
    // Only a plate seen just now: the detector remembers where the character
    // was for a moment after it stops seeing it, which is no reading at all.
    if (!eyes_.player(me, &fresh) || !fresh) return false;
    y = me.y();
    return true;
}

JumpCheck::Result JumpCheck::run(std::uint32_t jump_key, const std::atomic<bool>* stop) {
    Result r;
    const auto stopped = [stop] { return stop != nullptr && stop->load(); };

    if (jump_key == 0) {
        r.outcome = Outcome::NoKey;
        return r;
    }
    // A hold refuses every key, and whatever holds it is at work on the game:
    // nothing is pressed, nor is the game pulled to the front under it.
    if (coord_.critical_active()) {
        r.outcome = Outcome::Held;
        r.hold = coord_.critical_reason();
        return r;
    }
    // The game in front first: keys only reach it there, and the plate is
    // read off the screen, where anything over the game is read instead.
    if (!hands_.focus || !hands_.focus()) {
        r.outcome = Outcome::NotInFront;
        return r;
    }

    // Where the plate stands: the middle of a few fresh reads.
    std::vector<int> before;
    const std::uint64_t base_until = clock_now() + kBaselineWaitMs;
    while (static_cast<int>(before.size()) < kBaselineReads && clock_now() < base_until &&
           !stopped()) {
        int y = 0;
        if (look(y)) before.push_back(y);
        if (static_cast<int>(before.size()) < kBaselineReads) pause(kLookEveryMs);
    }
    const bool have_base = static_cast<int>(before.size()) >= kBaselineReads;
    int base = 0;
    if (have_base) {
        std::sort(before.begin(), before.end());
        base = before[before.size() / 2];
    }

    // Exactly one press, under the token like every other key -- never over
    // a hold, nor on top of another job's key, nor after a stop.
    const std::uint64_t token_until = clock_now() + kTokenWaitMs;
    for (;;) {
        if (stopped()) {
            r.outcome = Outcome::Stopped;
            return r;
        }
        if (coord_.critical_active()) {
            r.outcome = Outcome::Held;
            r.hold = coord_.critical_reason();
            return r;
        }
        auto token = coord_.acquire("jump-test", Priority::Combat);
        if (token) {
            // The game in front the moment before the press -- the player
            // may have clicked another window while the plate was read -- so
            // that "not in front" means nothing was sent: InputManager sends
            // nothing to a game behind another window.
            const bool in_front = hands_.focused ? hands_.focused() : (hands_.focus && hands_.focus());
            if (!in_front) {
                r.outcome = Outcome::NotInFront;
                return r;
            }
            r.pressed = hands_.key && hands_.key(jump_key, kHoldMs);
            break;
        }
        if (clock_now() >= token_until) {
            r.outcome = Outcome::Busy;
            return r;
        }
        pause(20);
    }
    // In front a moment ago, and the press did not come back as gone out:
    // focus went during it, perhaps with the key already down. It may have
    // reached the game, so it is not "nothing pressed" -- and nothing was
    // checked.
    if (!r.pressed) {
        r.outcome = Outcome::Unchecked;
        r.pressed = true;
        return r;
    }

    // Up and down again: the highest the plate went (screen y grows downward).
    int top = base;
    const std::uint64_t watch_until = clock_now() + kWatchMs;
    while (clock_now() < watch_until && !stopped()) {
        pause(kLookEveryMs);
        int y = 0;
        if (!look(y)) continue;
        ++r.reads;
        top = std::min(top, y);
    }
    if (!have_base || r.reads == 0) {
        r.outcome = Outcome::NoPlate;
        return r;
    }
    r.rose_px = std::max(0, base - top);
    if (r.rose_px >= kJumpedPx)
        r.outcome = Outcome::Jumped;
    else if (r.rose_px < kStillPx && r.reads >= kStillReads)
        r.outcome = Outcome::NoJump;
    else
        r.outcome = Outcome::Unsure;
    return r;
}

const char* JumpCheck::outcome_name(Outcome o) {
    switch (o) {
        case Outcome::Jumped:     return "jumped";
        case Outcome::NoJump:     return "no jump seen";
        case Outcome::Unsure:     return "not sure";
        case Outcome::NoPlate:    return "character not seen";
        case Outcome::Held:       return "held";
        case Outcome::Busy:       return "keys busy";
        case Outcome::NotInFront: return "game not in front";
        case Outcome::Stopped:    return "stopped";
        case Outcome::Unchecked:  return "not checked: the game lost focus during the press";
        case Outcome::NoKey:      break;
    }
    return "no jump key";
}

std::string JumpCheck::said_vi(const Result& r, const std::string& key) {
    switch (r.outcome) {
        case Outcome::Jumped:
            return "Đã bấm " + key + " một lần — nhân vật đã nhảy (cao khoảng " +
                   std::to_string(r.rose_px) + " px).";
        case Outcome::NoJump:
            return "Đã bấm " + key + " một lần nhưng không thấy nhân vật nhảy trên màn hình. Hãy "
                   "nhìn trong game và kiểm tra phím nhảy trong cài đặt phím của game.";
        case Outcome::Unsure:
            return "Đã bấm " + key + " một lần, nhân vật nhích lên khoảng " +
                   std::to_string(r.rose_px) + " px — chưa chắc là nhảy. Hãy nhìn trong game.";
        case Outcome::NoPlate:
            return "Đã bấm " + key + " một lần. Không thấy tên nhân vật nên không kiểm tra được — "
                   "hãy nhìn trong game.";
        case Outcome::Held:
            return "Bot đang tạm dừng (" + (r.hold.empty() ? std::string("đang bận") : r.hold) +
                   ") — thử lại sau.";
        case Outcome::Busy:
            return "Chưa bấm: bot đang bấm phím khác. Thử lại sau.";
        case Outcome::NotInFront:
            return "Chưa bấm: game không ở phía trước. Bấm vào cửa sổ game rồi thử lại.";
        case Outcome::Stopped:
            return "Đã dừng, chưa bấm phím nào.";
        case Outcome::Unchecked:
            return "Có thể đã bấm " + key + ": game không còn ở phía trước ngay lúc bấm nên không "
                   "kiểm tra được. Hãy nhìn trong game rồi thử lại.";
        case Outcome::NoKey:
            break;
    }
    return "Chưa đặt phím nhảy.";
}

JumpCheck::KeyAdvice JumpCheck::advice(std::uint32_t jump, std::uint32_t attack_key,
                                       const std::vector<NamedKey>& others) {
    KeyAdvice a;
    if (jump == 0) {
        a.kind = Advice::NotSet;
        return a;
    }
    if (attack_key != 0 && jump == attack_key) {
        a.kind = Advice::Conflict;
        a.what_vi = "đánh";
        return a;
    }
    if (const NamedKey* k = key_taken(jump, others)) {
        a.kind = Advice::Conflict;
        a.what_vi = k->what_vi;
        return a;
    }
    // GoC's own key map has Space as "Tấn công": pressed to jump, it attacks.
    if (jump == kSpace) a.kind = Advice::SpaceIsAttack;
    return a;
}

} // namespace gobot
