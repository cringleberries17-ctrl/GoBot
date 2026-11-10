#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace gobot {

/// The jobs that hold every key off and then wait a second with nobody at the
/// mouse or keyboard before they start -- the potion shop (GuiApp::trash_loop's
/// run_shop) and the quick quest (QuestRun) -- and what they do when somebody
/// was there: look again later each time, and say in the log what was seen.
///
/// On 2026-11-08 the shop's quiet second was spoiled ten times in five minutes,
/// a hand on the mouse, and each look held the skills, the potions and the
/// buffs off for a second: a hitch every 31 s. Now a look spoiled waits 30 s,
/// then 2, 5 and 10 minutes, one spoiled after another -- never longer than
/// the job's own interval -- and the first step again once a quiet second
/// passes, a button asks for the job, or its box is ticked on again.
class QuietBackOff {
public:
    /// How long after a spoiled look the job looks again, `cap_ms` its own
    /// interval -- never shorter than the first step whatever the cap -- and
    /// the next spoiled look a step further.
    std::uint64_t next_ms(std::uint64_t cap_ms);
    /// What next_ms would give, without moving on.
    std::uint64_t peek_ms(std::uint64_t cap_ms) const;
    /// A quiet second that passed, a button, the box ticked on again: the
    /// first step next time.
    void reset() { level_ = 0; }
    /// The step the next spoiled look takes, 0 to kSteps - 1: kept by
    /// whoever keeps the job's clock from one worker to the next.
    int  level() const { return level_; }
    void set_level(int level);

    static constexpr int           kSteps = 4;
    static constexpr std::uint64_t kStepsMs[kSteps] = { 30'000, 120'000, 300'000, 600'000 };

private:
    int level_ = 0;
};

/// The pointer, the mouse's buttons and the program in front at one moment:
/// what a spoiled quiet second is told by in the log. The owner asked who was
/// at the mouse; GetLastInputInfo says only that somebody was.
struct InputGlance {
    bool        cursor = false;     ///< the pointer was read
    long        x = 0;
    long        y = 0;
    unsigned    buttons = 0;        ///< the buttons held down: kLeft | kRight ...
    std::string front;              ///< the program in front, by its file name; "" when not known

    static constexpr unsigned kLeft = 1, kRight = 2, kMiddle = 4, kX1 = 8, kX2 = 16;
};

/// Where an InputGlance is read from: Windows' own (real()), or a test's
/// pretend desktop. A part left unset is not read. Only ever reads.
struct InputGlancer {
    std::function<bool(long& x, long& y)> cursor;           ///< GetCursorPos
    std::function<bool(int vk)>           button_down;      ///< GetAsyncKeyState's top bit
    std::function<std::string()>          front_program;    ///< the window in front's program, by file name

    InputGlance glance() const;
    static InputGlancer real();
};

/// The log's words for a spoiled quiet second, from a glance as it began and
/// one as it ended: "the pointer moved 37 px (812,440 -> 845,452); buttons
/// held: left; in front: chrome.exe". A pointer that stayed with no button
/// held says so: a key, the wheel, or a move there and back.
std::string quiet_spoiled_text(const InputGlance& before, const InputGlance& after);

/// The time a job's clock starts at in a new worker: the one the last worker
/// left -- across a MACRO OFF and ON -- but never sooner than `grace_ms` from
/// `now`, so the first look after a start is as late as it always was. `kept`
/// 0 is none kept.
std::uint64_t resumed_clock(std::uint64_t kept, std::uint64_t now, std::uint64_t grace_ms);

} // namespace gobot
