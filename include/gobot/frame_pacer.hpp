#pragma once

#include <cstdint>

namespace gobot {

/// How many frames a second GoBot's window is drawn at, in each of its three
/// states. [Performance] in configs/app.ini turns the pacer off; light mode
/// ("Nhẹ máy") lowers them.
struct FrameRates {
    int interactive = 30;   ///< somebody is using the window: a click, a key, the mouse over it, typing
    int idle = 5;           ///< GoBot is the window in front, and nobody touches it
    int background = 2;     ///< another window -- the game, as a rule -- is in front
};

/// When GoBot's window draws a frame, and how long its loop may wait.
///
/// The window used to build and present a whole ImGui frame on every pass of
/// its loop, about 60 a second with vsync and more where vsync does not block
/// -- inside a VMware guest with no 3D, every one of them drawn by the CPU,
/// with the game beside it wanting the same CPU. A control panel nobody is
/// looking at needs a couple of frames a second. So a frame is drawn:
///   - at once on input to the window (any message but WM_TIMER and WM_PAINT,
///     and a few queries nobody's hand sends: frame_pacer.cpp), then at the
///     interactive rate for kInteractiveForMs after the last one, and for as
///     long as the window wants it (typing, a combo or popup open, a mouse
///     button held, a key box waiting for its key);
///   - otherwise at the idle rate while GoBot is in front, the background rate
///     while another window is;
///   - once more on WM_PAINT or a redraw asked for (a status line that
///     changed, a job that started or stopped);
///   - never while the window is minimised or fully covered.
/// Never faster than the interactive rate, whatever asks.
///
/// What the loop does between frames is not the pacer's business, with one
/// exception: it must come round often. tick() reads GoBot's own keys there
/// -- F12, the panic key, among them -- by GetAsyncKeyState, so a key is
/// seen only if it is down at a pass, and pushes HP and MP to the potions.
/// So the loop never waits longer than kMaxWaitMs, from when it woke, whatever
/// the frame rate: with Windows' 15.6 ms clock tick that lands every pass on
/// the next tick, as today's loop does at 60 frames a second. tick() is never
/// tied to the frame rate.
///
/// No Windows call here: the clock and every state come in with each pass,
/// so the tests drive it with a clock of their own.
class FramePacer {
public:
    /// After the last input, the interactive rate is kept up this long.
    static constexpr std::uint64_t kInteractiveForMs = 1000;
    /// The longest the loop may wait between passes, counted from its wake.
    static constexpr std::uint32_t kMaxWaitMs = 10;
    /// How early a frame may come. Windows ends a wait on its 15.6 ms clock
    /// tick, so a frame due at 33 ms waited for the third tick, 47 ms, and 30
    /// frames a second came out as 21. Up to this much (and an eighth of the
    /// period at most) early, it is drawn on the tick before.
    static constexpr std::uint64_t kSlackMs = 8;

    /// The rates for everybody, and for light mode ("Nhẹ máy").
    static constexpr FrameRates normal_rates() { return FrameRates{ 30, 5, 2 }; }
    static constexpr FrameRates light_rates() { return FrameRates{ 20, 3, 1 }; }

    /// What one pass of the loop saw.
    struct Pass {
        std::uint64_t now_ms = 0;          ///< when the loop woke for this pass
        bool input = false;                ///< an interactive message since the last pass
        bool paint = false;                ///< WM_PAINT since the last pass
        bool want_interactive = false;     ///< typing, a popup open, a button held, a key box waiting
        bool foreground = false;           ///< GoBot's window is the one in front
        bool minimised = false;
        bool occluded = false;             ///< fully covered: Present said so
        bool redraw = false;               ///< a frame asked for: a status changed, a job started
        FrameRates rates;
    };

    /// Whether to draw a frame in this pass. True counts the frame as drawn
    /// at pass.now_ms: the loop draws it before it waits.
    bool decide(const Pass& pass);

    /// How long the loop may wait now, after this pass's work: until the next
    /// frame is due, and never more than tick_wait_ms() -- kMaxWaitMs from
    /// when it woke.
    std::uint32_t wait_ms(std::uint64_t now) const;

    /// kMaxWaitMs from `woke`, less what the pass has taken since: 0 once that
    /// has gone. For a pass that never asks decide(), as when the window is
    /// covered.
    static std::uint32_t tick_wait_ms(std::uint64_t woke, std::uint64_t now);

    /// Milliseconds between frames at `fps`, which is held to 1..60.
    static std::uint64_t period_of(int fps);

    /// The period the last decide() worked to, and whether it was the
    /// interactive one: for the tests and the log.
    std::uint64_t period_ms() const { return period_ms_; }
    bool interactive() const { return interactive_; }

private:
    bool          drawn_ = false;           ///< a frame has been drawn
    std::uint64_t last_frame_ms_ = 0;
    bool          had_input_ = false;
    std::uint64_t last_input_ms_ = 0;
    bool          pending_ = false;         ///< input, paint or a redraw not drawn yet
    bool          hidden_ = false;          ///< the last pass: minimised or covered
    bool          interactive_ = false;
    std::uint64_t woke_ms_ = 0;
    std::uint64_t period_ms_ = 200;
    std::uint64_t interactive_period_ms_ = 33;
};

/// Whether a window message is someone using the window, for FramePacer::Pass::
/// input: every message except WM_TIMER and WM_PAINT, and the queries Windows
/// or GoBot itself send with nobody's hand on anything -- a window's text
/// asked for by a list of windows, WindowFromPoint's hit test, accessibility.
bool is_interactive_message(unsigned message);

} // namespace gobot
