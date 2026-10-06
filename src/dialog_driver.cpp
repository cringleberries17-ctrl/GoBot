#include "gobot/dialog_driver.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace gobot {

DialogDriver::DialogDriver(InputManager& input, const TemplateStore& store, Grab grab)
    : input_(input), store_(store), grab_(std::move(grab)) {}

bool DialogDriver::look(Frame& frame) {
    return grab_ && grab_(frame) && frame.valid();
}

bool DialogDriver::find_now(const Frame& frame, const std::string& name, float score,
                            ScreenRect& at) const {
    const Frame* picture = store_.get(name);
    if (!picture || !picture->valid()) return false;
    const auto hit = TemplateMatcher::find(
        frame, ScreenRect{ 0, 0, frame.width, frame.height }, *picture, score);
    if (!hit.found) return false;
    at = hit.at;
    return true;
}

bool DialogDriver::wait_for(const std::string& name, float score, int wait_ms,
                            ScreenRect& at) {
    if (!store_.get(name)) {
        GB_WARN("DialogDriver: there is no picture called '%s'", name.c_str());
        return false;
    }

    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(std::max(0, wait_ms));
    Frame frame;
    for (;;) {
        if (cancelled_) return false;
        if (look(frame) && find_now(frame, name, score, at)) return true;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(kLookEveryMs));
    }
}

bool DialogDriver::wait_gone(const std::string& name, float score, int wait_ms) {
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(std::max(0, wait_ms));
    Frame frame;
    ScreenRect ignored;
    for (;;) {
        if (cancelled_) return false;
        if (look(frame) && !find_now(frame, name, score, ignored)) return true;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(kLookEveryMs));
    }
}

DialogDriver::Result DialogDriver::run(const Step& step) {
    Result out;

    if (step.find.empty()) {
        out.trouble = "the step names nothing to look for";
        return out;
    }
    if (!store_.get(step.find)) {
        out.trouble = "there is no picture called '" + step.find + "'";
        return out;
    }
    if (!step.confirm.empty() && !store_.get(step.confirm)) {
        out.trouble = "there is no picture called '" + step.confirm + "'";
        return out;
    }

    // A step has to be able to say it worked. Clicking and hoping is what makes
    // a sequence walk itself into nothing and report success.
    if (step.confirm.empty() && !step.gone) {
        out.trouble = "the step on '" + step.find +
                      "' has no way of telling whether the click landed";
        return out;
    }

    const int attempts = std::clamp(step.attempts, 1, 10);
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (cancelled_) { out.trouble = "cancelled"; return out; }
        out.attempts_used = attempt;

        ScreenRect at;
        // Only the first attempt waits the full time. After a click that did
        // not take, the page is already there or it is not.
        const int wait = attempt == 1 ? step.wait_ms : std::min(step.wait_ms, 1500);
        if (!wait_for(step.find, step.score, wait, at)) {
            if (step.optional) {
                out.ok = true;
                out.trouble = "'" + step.find + "' was not there, which is allowed";
                return out;
            }
            out.trouble = "waited for '" + step.find + "' and it never appeared";
            continue;
        }
        out.at = at;

        const int cx = at.x + (step.click_dx >= 0 ? step.click_dx : at.w / 2);
        const int cy = at.y + (step.click_dy >= 0 ? step.click_dy : at.h / 2);
        input_.click_at(cx, cy);

        const bool landed = step.gone
            ? wait_gone(step.find, step.score, step.confirm_ms)
            : [&] { ScreenRect where; return wait_for(step.confirm, step.score,
                                                      step.confirm_ms, where); }();
        if (landed) {
            out.ok = true;
            out.trouble.clear();
            say("clicked '" + step.find + "' at " + std::to_string(cx) + "," +
                std::to_string(cy));
            return out;
        }

        out.trouble = step.gone
            ? "clicked '" + step.find + "' and it is still there"
            : "clicked '" + step.find + "' and '" + step.confirm + "' never appeared";
        GB_WARN("DialogDriver: attempt %d of %d -- %s",
                attempt, attempts, out.trouble.c_str());
    }
    return out;
}

std::size_t DialogDriver::run_all(const std::vector<Step>& steps, std::string* trouble) {
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const Result r = run(steps[i]);
        if (!r.ok) {
            if (trouble)
                *trouble = "step " + std::to_string(i + 1) + " of " +
                           std::to_string(steps.size()) + " ('" + steps[i].find +
                           "'): " + r.trouble;
            return i;
        }
    }
    return steps.size();
}

int DialogDriver::close_popups(int budget) {
    // Every way this game offers to dismiss something.
    //
    // All eight close buttons, because the game has eight. The first version of
    // this listed four -- 2, 4, 6 and 8 -- copied from the commercial client's
    // own strings rather than checked against the set actually in hand. A
    // survey against the live client then found `close_1` on screen at a score
    // of 1.000, which is to say: the one dialog that happened to be open was
    // closed by a button this did not know about.
    //
    // Plain closes before the confirmations, because clicking a confirm when a
    // close would have done agrees to something nobody asked about.
    static const char* kButtons[] = {
        "close_ic/close_1", "close_ic/close_2", "close_ic/close_3",
        "close_ic/close_4", "close_ic/close_5", "close_ic/close_6",
        "close_ic/close_7", "close_ic/close_8",
        "dialog_ok", "confirm_button",
    };

    int closed = 0;
    Frame frame;
    for (int round = 0; round < std::clamp(budget, 1, 16); ++round) {
        if (cancelled_) break;
        if (!look(frame)) break;

        bool clicked = false;
        for (const char* button : kButtons) {
            ScreenRect at;
            if (!find_now(frame, button, 0.85f, at)) continue;

            input_.click_at(at.x + at.w / 2, at.y + at.h / 2);
            clicked = true;

            // Check it actually went, and stop if it did not.
            //
            // This counted clicks before, not closures, and that made it lie.
            // Run against the live client it found close_1 at 828,164, clicked
            // it six times in a row at the same place while nothing on screen
            // changed, and reported "closed 6". Every other step in this file
            // has to prove it worked; there is no reason this one should not,
            // and it was the one actually being run.
            if (wait_gone(button, 0.85f, 1200)) {
                ++closed;
                GB_INFO("DialogDriver: closed '%s' at %d,%d", button, at.x, at.y);
            } else {
                GB_WARN("DialogDriver: clicked '%s' at %d,%d and it is still "
                        "there -- the click is not reaching the game. Check it "
                        "is the foreground window and that GoBot is running "
                        "elevated.", button, at.x, at.y);
                return closed;
            }
            break;
        }
        if (!clicked) break;          // nothing left in the way
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return closed;
}

} // namespace gobot
