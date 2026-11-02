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

    // check = false says in the file that nothing on screen can show this
    // click landed. One that names a confirmation as well is a half-made edit.
    if (!step.check && (!step.confirm.empty() || step.gone)) {
        out.trouble = "the step on '" + step.find +
                      "' says check = false and also names a way to check it -- keep one";
        return out;
    }
    // A step has to be able to say it worked, or say plainly that it cannot
    // (check = false). Clicking and hoping is what makes a sequence walk
    // itself into nothing and report success.
    if (step.check && step.confirm.empty() && !step.gone) {
        out.trouble = "the step on '" + step.find +
                      "' has no way of telling whether the click landed";
        return out;
    }

    const int attempts = std::clamp(step.attempts, 1, 10);
    // Whether a click of this step's has been asked for yet: a confirmation
    // up before the first proves nothing, one up before a retry came after it.
    bool tried = false;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (cancelled_) { out.trouble = "cancelled"; return out; }
        out.attempts_used = attempt;

        ScreenRect at;
        // Only the first attempt waits the full time. After a click that did
        // not take, the page is already there or it is not.
        const int wait = attempt == 1 ? step.wait_ms : std::min(step.wait_ms, 1500);
        if (!wait_for(step.find, step.score, wait, at)) {
            // A stop during the wait is not "not there, which is allowed".
            if (cancelled_) { out.trouble = "cancelled"; return out; }
            if (step.optional) {
                out.ok = true;
                out.trouble = "'" + step.find + "' was not there, which is allowed";
                return out;
            }
            out.trouble = "waited for '" + step.find + "' and it never appeared";
            continue;
        }
        out.at = at;

        int cx = 0, cy = 0;
        click_point(at, step, cx, cy);
        if (step.click_dx >= at.w || step.click_dy >= at.h)
            GB_WARN("DialogDriver: the offset %d,%d on '%s' is past its %dx%d -- "
                    "clicking at its edge",
                    step.click_dx, step.click_dy, step.find.c_str(), at.w, at.h);
        if (cancelled_) { out.trouble = "cancelled"; return out; }

        // The confirmation has to be something this click brings up, so it is
        // looked for first. One on screen already proves nothing: the shop's
        // Mua is up whether or not an item was picked, and the bin's Xac Nhan
        // would answer for a shop's. Up before the first click, the step is
        // refused with nothing clicked. Up before a retry, it came after an
        // earlier click it was slow to answer: that click landed, and another
        // would do it twice.
        if (step.check && !step.gone) {
            Frame before;
            ScreenRect up;
            if (!look(before)) {
                out.trouble = "could not look at the screen before clicking '" + step.find + "'";
                GB_WARN("DialogDriver: attempt %d of %d -- %s",
                        attempt, attempts, out.trouble.c_str());
                continue;
            }
            if (find_now(before, step.confirm, step.score, up)) {
                if (!tried) {
                    out.trouble = "'" + step.confirm + "' was already on screen before the click, "
                                  "so it could not show whether the click landed -- nothing was clicked";
                    GB_WARN("DialogDriver: %s", out.trouble.c_str());
                    return out;
                }
                out.ok = true;
                out.trouble.clear();
                say("'" + step.find + "' done: '" + step.confirm +
                    "' came up late, after the last click -- not clicked again");
                return out;
            }
        }

        // Only with the game in front and the game itself under the point: the
        // frames come from PrintWindow, which shows the game even when it is
        // covered, and a click goes to whatever is at that point of the
        // screen. The confirmation is looked for all the same when the click
        // was refused, because the screen is the judge -- the player may have
        // clicked it -- and a pretend game in the tests answers without one.
        InputManager::GameClick how = InputManager::GameClick::Failed;
        const bool clicked =
            input_.click_on_game(cx, cy, false, [this] { return cancelled_.load(); }, &how);
        tried = true;
        const std::string point = std::to_string(cx) + "," + std::to_string(cy);

        // Nothing on screen can show this click landed (check = false): it is
        // made once and said to be unchecked, never reported as confirmed.
        // Only a click that was refused, which sent nothing, is made again.
        if (!step.check) {
            if (clicked) {
                out.ok = true;
                out.trouble.clear();
                GB_INFO("DialogDriver: clicked '%s' at %s -- not checked (check = false)",
                        step.find.c_str(), point.c_str());
                say("clicked '" + step.find + "' at " + point +
                    " -- NOT CHECKED: nothing on screen shows whether this click landed");
                // The moment a checked step spends on its first look for the
                // confirmation, so the game has the click before the next
                // step makes its own.
                std::this_thread::sleep_for(std::chrono::milliseconds(kLookEveryMs));
                return out;
            }
            if (cancelled_) { out.trouble = "cancelled"; return out; }
            out.trouble = "the click on '" + step.find + "' at " + point + " was refused (" +
                          InputManager::game_click_name(how) +
                          "), and with check = false nothing on screen can show it was done";
            GB_WARN("DialogDriver: attempt %d of %d -- %s",
                    attempt, attempts, out.trouble.c_str());
            continue;
        }

        const bool landed = step.gone
            ? wait_gone(step.find, step.score, step.confirm_ms)
            : [&] { ScreenRect where; return wait_for(step.confirm, step.score,
                                                      step.confirm_ms, where); }();
        if (landed) {
            out.ok = true;
            out.trouble.clear();
            say(clicked ? "clicked '" + step.find + "' at " + point
                        : "'" + step.find + "' done at " + point +
                              " without a click of GoBot's: it was refused (" +
                              InputManager::game_click_name(how) + ")");
            return out;
        }
        if (cancelled_) { out.trouble = "cancelled"; return out; }

        const std::string outcome = step.gone
            ? std::string("it is still there")
            : "'" + step.confirm + "' never appeared";
        out.trouble = clicked
            ? "clicked '" + step.find + "' and " + outcome
            : "the click on '" + step.find + "' at " + point + " was refused (" +
                  InputManager::game_click_name(how) + ") and " + outcome;
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

void DialogDriver::click_point(const ScreenRect& at, const Step& step, int& x, int& y) {
    const int last_x = std::max(0, at.w - 1);
    const int last_y = std::max(0, at.h - 1);
    x = at.x + (step.click_dx >= 0 ? std::min(step.click_dx, last_x) : at.w / 2);
    y = at.y + (step.click_dy >= 0 ? std::min(step.click_dy, last_y) : at.h / 2);
}

} // namespace gobot
