#include "gobot/jump_check.hpp"
#include "gobot/common.hpp"
#include "gobot/input_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
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
    // ↓ would not go out anyway (InputManager::never_pressed), and a press
    // refused would read as one that may have gone out.
    if (InputManager::never_pressed(jump_key)) {
        r.outcome = Outcome::NeverPressed;
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
    std::optional<std::uint64_t> takeoffs_before;
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
            // Your take-offs so far, the moment before the press, when the
            // packets can tell: a jump of the player's own a moment earlier
            // is not counted as this press's.
            if (eyes_.takeoffs) takeoffs_before = eyes_.takeoffs();
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
    bool cut = false;
    while (clock_now() < watch_until) {
        if (stopped()) {
            cut = true;
            break;
        }
        pause(kLookEveryMs);
        int y = 0;
        if (!look(y)) continue;
        ++r.reads;
        top = std::min(top, y);
    }
    // Your client's own take-offs since the press, when the packets could
    // tell before it and still can. A count lower than before is a capture
    // started afresh meanwhile: nothing to go by, and the plate decides. A
    // watch cut short by a stop goes by them only for a take-off already
    // counted: the frame of one may not have come yet, and none counted is no
    // proof of no jump.
    if (takeoffs_before && eyes_.takeoffs) {
        const std::optional<std::uint64_t> after = eyes_.takeoffs();
        if (after && *after >= *takeoffs_before && (!cut || *after > *takeoffs_before)) {
            r.by_packets = true;
            r.takeoffs = static_cast<int>(std::min<std::uint64_t>(*after - *takeoffs_before, 1000));
        }
    }
    if (!have_base || r.reads == 0) {
        if (r.by_packets) {
            r.outcome = r.takeoffs >= 1 ? Outcome::Jumped : Outcome::NoJump;
            return r;
        }
        r.outcome = Outcome::NoPlate;
        return r;
    }
    r.rose_px = std::max(0, base - top);
    // The packets decide whatever the plate did: on a map where the camera
    // follows the jump, the plate hardly moves.
    if (r.by_packets)
        r.outcome = r.takeoffs >= 1 ? Outcome::Jumped : Outcome::NoJump;
    else if (r.rose_px >= kJumpedPx)
        r.outcome = Outcome::Jumped;
    else if (r.rose_px < kStillPx && r.reads >= kStillReads)
        r.outcome = Outcome::NoJump;
    else
        r.outcome = Outcome::Unsure;
    return r;
}

// --------------------------------------------- "Thử ↑", "Thử ↑↑", "Thử Ctrl + ↑"

JumpCheck::Flight JumpCheck::flight_of(const std::vector<PacketWorld::OwnTrace>& reports) {
    Flight f;
    // Up and away: in the air (any state but 0) and rising. A walk-off is
    // state 1 too, at a speed of 0 or down: no push of anyone's.
    const auto pushes = [](const PacketWorld::OwnTrace& r) {
        return r.kind == OwnReport::Air && r.state != 0 && r.vy < 0.0f;
    };
    std::optional<PacketWorld::OwnTrace> walk;     // the first walk's report after the take-off
    for (const PacketWorld::OwnTrace& r : reports) {
        if (!f.took_off) {
            if (pushes(r)) {
                f.took_off = true;
                f.off = r;
                f.push = r;
            }
            continue;
        }
        if (r.kind == OwnReport::Air) {
            if (r.state == 0) {
                f.landed = true;
                f.land = r;
                break;
            }
            if (pushes(r)) f.push = r;
            continue;
        }
        if (r.kind != OwnReport::Cast && !walk) walk = r;
    }
    if (!f.landed && walk) {
        f.landed = true;
        f.land = *walk;
    }
    return f;
}

JumpCheck::KindResult JumpCheck::run_kind(JumpKind kind, const std::atomic<bool>* stop) {
    KindResult r;
    r.kind = kind;
    const auto stopped = [stop] { return stop != nullptr && stop->load(); };
    const auto ends = [&r](KindOutcome o) {
        r.outcome = o;
        return r;
    };

    // A hold refuses every key: nothing pressed, and nothing asked.
    if (coord_.critical_active()) {
        r.hold = coord_.critical_reason();
        return ends(KindOutcome::Held);
    }
    // The map before anything else: a jump straight up under a floor may
    // land on it -- Ctrl + ↑ goes some 430 px up. No gate is asked of: on one
    // Ctrl + ↑ does not change the map (the owner, 2026-11-05).
    if (eyes_.floor_above) {
        if (const std::optional<int> above = eyes_.floor_above()) {
            r.floor_above_px = *above;
            return ends(KindOutcome::FloorAbove);
        }
    }
    // Only the packets tell a high jump or a second one from a plain jump.
    if (!eyes_.air || !eyes_.air()) return ends(KindOutcome::NoPackets);
    if (!hands_.jump) return ends(KindOutcome::NotSent);
    if (!hands_.focus || !hands_.focus()) return ends(KindOutcome::NotInFront);

    // Exactly one jump, under the token taken only from nobody -- as the
    // drop takes it: no key of anyone's between Ctrl's down and its up.
    const std::uint64_t token_until = clock_now() + kTokenWaitMs;
    std::optional<PacketWorld::AirCounts> before;
    Press press;
    for (;;) {
        if (stopped()) return ends(KindOutcome::Stopped);
        if (coord_.critical_active()) {
            r.hold = coord_.critical_reason();
            return ends(KindOutcome::Held);
        }
        auto token = coord_.acquire_free("jump-test", Priority::Combat);
        if (token) {
            const bool in_front = hands_.focused ? hands_.focused() : (hands_.focus && hands_.focus());
            if (!in_front) return ends(KindOutcome::NotInFront);
            // The counts the moment before: a jump of the player's own a
            // moment earlier is not this press's.
            before = eyes_.air();
            if (!before) return ends(KindOutcome::NoPackets);
            // Asked through the press: a stop, a hold or the game sent
            // behind cuts it short.
            const auto go_on = [this, &stopped] {
                return !stopped() && !coord_.critical_active() && (!hands_.focused || hands_.focused());
            };
            press = hands_.jump(kind, go_on);
            break;
        }
        if (clock_now() >= token_until) return ends(KindOutcome::Busy);
        pause(20);
    }
    r.pressed = press.went;
    r.held_ms = press.held_ms;
    r.held_long = press.held_ms > InputManager::kJumpHeldLongMs;
    if (!press.went) return ends(KindOutcome::NotSent);
    const std::uint64_t from = press.down_ms != 0 ? press.down_ms : clock_now();

    // The flight, until its landing or kKindWatchMs.
    const std::uint64_t watch_until = clock_now() + kKindWatchMs;
    bool cut = false;
    while (clock_now() < watch_until) {
        if (stopped()) {
            cut = true;
            break;
        }
        pause(kLookEveryMs);
        if (eyes_.own_since && flight_of(eyes_.own_since(from)).landed) break;
    }
    const Flight f = eyes_.own_since ? flight_of(eyes_.own_since(from)) : Flight{};
    if (f.took_off) {
        r.takeoff_ms = f.off.ms >= from ? static_cast<int>(f.off.ms - from) : 0;
        if (f.landed && f.land.ms >= f.off.ms) {
            r.flight_ms = static_cast<int>(f.land.ms - f.off.ms);
            r.rose_px = static_cast<int>(std::lround(f.off.y - f.land.y));
            if (f.land.ms > f.push.ms)
                r.gravity = gravity_from(f.push.y, static_cast<double>(f.push.vy) * PacketWorld::kStepsPerSecond,
                                         f.land.y, static_cast<double>(f.land.ms - f.push.ms) / 1000.0);
        }
    }

    // The counts since the press. One lower than before is a capture started
    // afresh meanwhile: nothing to go by.
    const std::optional<PacketWorld::AirCounts> after = eyes_.air();
    const auto since = [](std::uint64_t now, std::uint64_t was) {
        return now >= was ? static_cast<int>(std::min<std::uint64_t>(now - was, 1000)) : -1;
    };
    bool counted = false;
    if (after) {
        const int jumps = since(after->jumps, before->jumps), walkoffs = since(after->walkoffs, before->walkoffs);
        const int second = since(after->second, before->second), high = since(after->high, before->high);
        const int landed = since(after->landed, before->landed);
        counted = jumps >= 0 && walkoffs >= 0 && second >= 0 && high >= 0 && landed >= 0;
        if (counted) {
            r.jumps = std::max(0, jumps - walkoffs);
            r.walkoffs = walkoffs;
            r.second = second;
            r.high = high;
            r.landed = landed;
        }
    }
    if (!press.whole) return ends(KindOutcome::Cut);
    if (!counted) return ends(KindOutcome::Unsure);
    const bool its = kind == JumpKind::Up       ? r.jumps > 0
                     : kind == JumpKind::UpUp   ? r.jumps > 0 && r.second > 0
                                                : r.high > 0;
    if (its) return ends(KindOutcome::Jumped);
    if (r.jumps + r.second + r.high > 0) return ends(KindOutcome::KindMissed);
    // Cut short by a stop: none counted yet is no proof of none.
    return ends(cut ? KindOutcome::Unsure : KindOutcome::NoJump);
}

JumpCheck::Press JumpCheck::press_with(InputManager& input, JumpKind kind, const std::function<bool()>& go_on) {
    // Always ↑: the key is fixed here, never a configured one.
    constexpr std::uint32_t kUp = 0x26;
    Press p;
    switch (kind) {
        case JumpKind::Up: {
            const InputManager::JumpPress j = input.jump_once(kUp, go_on);
            p.went = j.sent != InputManager::JumpSent::NotSent;
            p.whole = p.went;
            p.down_ms = j.first_down_ms;
            p.held_ms = j.held1;
            break;
        }
        case JumpKind::UpUp: {
            const InputManager::JumpPress j = input.jump_twice(kUp, kJumpUpSecondAtMs, go_on);
            p.went = j.sent != InputManager::JumpSent::NotSent;
            p.whole = j.sent == InputManager::JumpSent::Both;
            p.down_ms = j.first_down_ms;
            p.held_ms = std::max(j.held1, j.held2);
            break;
        }
        case JumpKind::CtrlUp: {
            const InputManager::DropPress d = input.high_jump(go_on);
            p.went = d.sent == InputManager::DropSent::Sent || d.sent == InputManager::DropSent::Cut;
            p.whole = d.sent == InputManager::DropSent::Sent;
            p.down_ms = d.down_ms;
            p.held_ms = d.held_ms;
            break;
        }
    }
    return p;
}

const char* JumpCheck::kind_outcome_name(KindOutcome o) {
    switch (o) {
        case KindOutcome::Jumped:     return "jumped";
        case KindOutcome::KindMissed: return "jumped, but not as asked";
        case KindOutcome::NoJump:     return "no jump";
        case KindOutcome::Cut:        return "pressed only in part";
        case KindOutcome::Unsure:     return "not sure";
        case KindOutcome::NotSent:    return "not pressed: the game left the front, or a stop or a hold came";
        case KindOutcome::FloorAbove: return "not pressed: a floor above";
        case KindOutcome::NoPackets:  return "not pressed: the packets cannot tell the kinds apart";
        case KindOutcome::Held:       return "held";
        case KindOutcome::Busy:       return "keys busy";
        case KindOutcome::NotInFront: return "game not in front";
        case KindOutcome::Stopped:    break;
    }
    return "stopped";
}

std::string JumpCheck::kind_said_vi(const KindResult& r) {
    const std::string key = jump_kind_key(r.kind);
    switch (r.outcome) {
        case KindOutcome::Jumped: {
            std::string s = "Đã bấm " + key + " — nhân vật đã " +
                            (r.kind == JumpKind::Up     ? std::string("nhảy")
                             : r.kind == JumpKind::UpUp ? std::string("nhảy rồi nhảy thêm lần nữa trên không")
                                                        : std::string("nhảy cao")) +
                            " (theo gói tin)";
            if (r.flight_ms >= 0) {
                s += ": bay " + std::to_string(r.flight_ms) + " ms";
                if (r.gravity) s += ", g≈" + std::to_string(static_cast<int>(std::lround(*r.gravity)));
                if (r.rose_px > kStillPx)
                    s += ", đáp xuống cao hơn chỗ nhảy " + std::to_string(r.rose_px) + " px";
                else if (r.rose_px < -kStillPx)
                    s += ", đáp xuống thấp hơn chỗ nhảy " + std::to_string(-r.rose_px) + " px";
            }
            s += ".";
            if (r.held_long) s += " Phím ↑ bị giữ lâu (" + std::to_string(r.held_ms) + " ms): máy có thể đang bận.";
            return s;
        }
        case KindOutcome::KindMissed:
            if (r.kind == JumpKind::UpUp)
                return "Đã bấm ↑↑ nhưng nhân vật chỉ nhảy một lần, không nhảy thêm trên không (theo gói tin).";
            if (r.kind == JumpKind::CtrlUp)
                return "Đã bấm Ctrl+↑ nhưng nhân vật chỉ nhảy thường, không nhảy cao (theo gói tin).";
            return "Đã bấm ↑ nhưng nhân vật nhảy kiểu khác (theo gói tin).";
        case KindOutcome::NoJump:
            return "Đã bấm " + key + " nhưng nhân vật không nhảy (theo gói tin). Hãy kiểm tra phím nhảy trong cài "
                   "đặt phím của game.";
        case KindOutcome::Cut:
            if (r.kind == JumpKind::UpUp)
                return "Chỉ bấm được phím ↑ đầu của ↑↑ (bot vừa bận, hoặc game không còn ở phía trước) — thử lại.";
            return "Bấm " + key + " chưa trọn: phím ↑ được nhả quá sớm — thử lại.";
        case KindOutcome::Unsure:
            return "Đã bấm " + key + " nhưng chưa kịp biết nhân vật có nhảy không (đã dừng, hoặc mất gói tin) — "
                   "hãy nhìn trong game.";
        case KindOutcome::NotSent:
            return "Chưa bấm: game không còn ở phía trước, hoặc bot vừa bận. Thử lại.";
        case KindOutcome::FloorAbove:
            return "Chưa bấm " + key + ": bản đồ cho thấy có bục phía trên, cao khoảng " +
                   std::to_string(r.floor_above_px) + " px (trong vòng " + std::to_string(kJumpUpCeilingPx) +
                   " px), nhảy thử có thể rơi lên bục đó. Hãy Thử ở mặt đất phẳng, không có bục phía trên.";
        case KindOutcome::NoPackets:
            return "Chưa bấm: GoBot chưa đọc được gói tin của game, nên không biết nhân vật nhảy kiểu gì. Cần dấu "
                   "Vị trí từ gói tin.";
        case KindOutcome::Held:
            return "Bot đang tạm dừng (" + (r.hold.empty() ? std::string("đang bận") : r.hold) + ") — thử lại sau.";
        case KindOutcome::Busy:
            return "Chưa bấm: bot đang bấm phím khác. Thử lại sau.";
        case KindOutcome::NotInFront:
            return "Chưa bấm: game không ở phía trước. Bấm vào cửa sổ game rồi thử lại.";
        case KindOutcome::Stopped:
            break;
    }
    return "Đã dừng, chưa bấm phím nào.";
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
        case Outcome::NeverPressed: return "not pressed: Down is never pressed";
        case Outcome::NoKey:      break;
    }
    return "no jump key";
}

std::string JumpCheck::said_vi(const Result& r, const std::string& key) {
    switch (r.outcome) {
        case Outcome::Jumped:
            if (r.by_packets) return "Đã bấm " + key + " một lần — nhân vật đã nhảy (theo gói tin).";
            return "Đã bấm " + key + " một lần — nhân vật đã nhảy (cao khoảng " +
                   std::to_string(r.rose_px) + " px).";
        case Outcome::NoJump:
            if (r.by_packets)
                return "Đã bấm " + key + " một lần nhưng nhân vật không nhảy (theo gói tin). Hãy kiểm tra "
                       "phím nhảy trong cài đặt phím của game.";
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
        case Outcome::NeverPressed:
            return "Chưa bấm: GoBot không bấm ↓ (đứng trên cửa mà bấm ↓ là sang bản đồ khác). Đặt phím "
                   "nhảy khác, thường là ↑.";
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
    // Before anything else it may clash with: it is never pressed at all.
    if (InputManager::never_pressed(jump)) {
        a.kind = Advice::DownChangesMap;
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

std::string JumpCheck::walk_key_warning_vi(std::uint32_t walk_jump, std::uint32_t attack_key) {
    if (walk_jump == 0) return {};
    const bool attack = attack_key != 0 && walk_jump == attack_key;
    if (!attack && walk_jump != kSpace) return {};
    return "Phím nhảy khi đi tới điểm đã lưu (jump_key trong phần [Move] của configs\\hunt.ini) là " +
           InputManager::key_name(walk_jump) + (attack ? ", trùng phím đánh" : ", mà trong game Space là phím Tấn công") +
           ": bot sẽ đánh chứ không nhảy. Phím nhảy trong game là ↑ (jump_key=38).";
}

} // namespace gobot
