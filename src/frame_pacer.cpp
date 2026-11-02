#include "gobot/frame_pacer.hpp"

#include <windows.h>    // the WM_ codes; nothing here calls Windows

#include <algorithm>

namespace gobot {

std::uint64_t FramePacer::period_of(int fps) {
    const int held = std::clamp(fps, 1, 60);
    return static_cast<std::uint64_t>(1000 / held);
}

std::uint32_t FramePacer::tick_wait_ms(std::uint64_t woke, std::uint64_t now) {
    const std::uint64_t spent = now > woke ? now - woke : 0;
    return spent >= kMaxWaitMs ? 0u : static_cast<std::uint32_t>(kMaxWaitMs - spent);
}

bool FramePacer::decide(const Pass& p) {
    woke_ms_ = p.now_ms;
    if (p.input) {
        had_input_ = true;
        last_input_ms_ = p.now_ms;
    }
    // What has to be shown is kept until it is: a click that comes while the
    // window is minimised is drawn when it is back.
    if (p.input || p.paint || p.redraw) pending_ = true;

    const FrameRates& r = p.rates;
    interactive_ = p.want_interactive ||
                   (had_input_ && p.now_ms >= last_input_ms_ && p.now_ms - last_input_ms_ < kInteractiveForMs);
    interactive_period_ms_ = period_of(r.interactive);
    period_ms_ = interactive_ ? interactive_period_ms_ : period_of(p.foreground ? r.idle : r.background);

    // Nobody can see it: nothing is drawn, and what was asked for waits.
    hidden_ = p.minimised || p.occluded;
    if (hidden_) return false;

    bool due = !drawn_;
    if (!due) {
        // A clock that went back (a test's, or a frame dated after this
        // pass) counts as no time gone.
        const std::uint64_t since = p.now_ms > last_frame_ms_ ? p.now_ms - last_frame_ms_ : 0;
        const auto early = [](std::uint64_t period) { return std::min(kSlackMs, period / 8); };
        // At the rate in force; and what was asked for -- input, a paint, a
        // redraw -- at once, as soon as the interactive rate allows, so that
        // nothing ever draws faster than that.
        due = since + early(period_ms_) >= period_ms_ ||
              (pending_ && since + early(interactive_period_ms_) >= interactive_period_ms_);
    }
    if (!due) return false;

    drawn_ = true;
    last_frame_ms_ = p.now_ms;
    pending_ = false;
    return true;
}

std::uint32_t FramePacer::wait_ms(std::uint64_t now) const {
    std::uint32_t wait = tick_wait_ms(woke_ms_, now);
    if (hidden_) return wait;               // looked at again within kMaxWaitMs
    if (!drawn_) return 0;                  // the first frame, at once
    // Until the next frame is due, at the rate in force or, with something
    // asked for and not drawn yet, the interactive one.
    const std::uint64_t period = pending_ ? std::min(period_ms_, interactive_period_ms_) : period_ms_;
    const std::uint64_t due = last_frame_ms_ + period - std::min(kSlackMs, period / 8);
    if (now >= due) return 0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(wait, due - now));
}

bool is_interactive_message(unsigned message) {
    switch (message) {
        // A timer, and a paint: WM_PAINT asks for one frame of its own
        // (FramePacer::Pass::paint), not for the interactive rate.
        case WM_TIMER:
        case WM_PAINT:
        case WM_NCPAINT:
        case WM_ERASEBKGND:
        case WM_SYNCPAINT:
        // Queries sent with nobody's hand on anything. GetWindowText on this
        // window -- a list of every window, as the window list and the
        // checks for what covers the game make -- sends WM_GETTEXT;
        // WindowFromPoint from GoBot's own thread sends WM_NCHITTEST, and real
        // movement comes as WM_MOUSEMOVE or WM_NCMOUSEMOVE anyway. Counted, each
        // would hold the window at the interactive rate for a second.
        case WM_NULL:
        case WM_GETTEXT:
        case WM_GETTEXTLENGTH:
        case WM_GETICON:
        case WM_GETOBJECT:
        case WM_NCHITTEST:
        case WM_GETMINMAXINFO:
        case WM_WINDOWPOSCHANGING:
        case WM_WINDOWPOSCHANGED:
            return false;
        default:
            // Everything else: mouse and keys, WM_CHAR, the IME's messages,
            // WM_MOUSELEAVE, the non-client mouse, WM_SETCURSOR,
            // WM_CAPTURECHANGED, activation, focus, size, DPI.
            return true;
    }
}

} // namespace gobot
