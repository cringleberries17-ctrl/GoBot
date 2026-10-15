#include "gobot/reviver.hpp"
#include "gobot/bag.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

#include <windows.h>

namespace gobot {
namespace {

/// Every dialog with [Xác Nhận][Hủy Bỏ] is about this big round its buttons,
/// which sit at its bottom right: the revive question's at 103,55 of its
/// 198x81 picture, the bin's at 100-101,50-51 of theirs. With a few pixels to
/// spare, for a dialog drawn a little differently.
constexpr int kDialogLeft = 108, kDialogUp = 60, kDialogW = 210, kDialogH = 90;

/// Part of a picture, as it is; empty when the rectangle is not inside it.
Frame crop(const Frame& f, const ScreenRect& r) {
    Frame out;
    if (!f.valid() || r.x < 0 || r.y < 0 || r.right() > f.width || r.bottom() > f.height ||
        !r.valid())
        return out;
    out.width = r.w;
    out.height = r.h;
    out.pixels.resize(static_cast<std::size_t>(r.w) * r.h * 4);
    for (int y = 0; y < r.h; ++y)
        std::copy_n(f.pixels.begin() + ((static_cast<std::size_t>(r.y + y) * f.width + r.x) * 4),
                    static_cast<std::size_t>(r.w) * 4,
                    out.pixels.begin() + static_cast<std::size_t>(y) * r.w * 4);
    return out;
}

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::string at(int x, int y) { return std::to_string(x) + "," + std::to_string(y); }

/// A rectangle cut down to the frame. The matcher reads only the part of an
/// area that is on the frame but counts what it finds from the area's own
/// corner, so an area starting left of or above the frame would put a match
/// that many pixels out.
ScreenRect on_frame(const Frame& f, const ScreenRect& r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.right()), y1 = std::min(f.height, r.bottom());
    return ScreenRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

} // namespace

const char* Reviver::turn_name(Turn t) {
    switch (t) {
        case Turn::Blind:      return "blind";
        case Turn::Alive:      return "alive";
        case Turn::Waiting:    return "waiting";
        case Turn::Revived:    return "revived";
        case Turn::Failed:     return "failed";
        case Turn::OutOfTries: return "out of tries";
        case Turn::Stopped:    return "stopped";
    }
    return "?";
}

std::string Reviver::out_of_tries_vi() {
    return "Đã dừng bot: hồi sinh 3 lần không được — có thể đã hết giấy hồi sinh/bùa hồi sinh. "
           "GoBot không hồi sinh tại làng (mất 2% EXP). Hãy hồi sinh nhân vật bằng tay rồi bấm "
           "BẮT ĐẦU.";
}

Reviver::Reviver(InputManager& input, const TemplateStore& store, Grab grab)
    : input_(input), store_(store), grab_(std::move(grab)) {
    // GoC's two corners, cut from the panel's picture once. Corners rather
    // than the panel: they carry no words, so the client's language does not
    // matter, and the pair of them is never seen without the panel.
    if (const Frame* panic = picture("dialog_panic", "inven/0/dialog_Panic.spr/0")) {
        anchor1_ = crop(*panic, ScreenRect{ kAnchor1X, kAnchor1Y, kAnchorSize, kAnchorSize });
        anchor2_ = crop(*panic, ScreenRect{ kAnchor2X, kAnchor2Y, kAnchorSize, kAnchorSize });
    }
}

const Frame* Reviver::picture(const char* name, const char* path) const {
    if (const Frame* f = store_.get(name)) return f;
    return store_.get(path);
}

bool Reviver::ready(std::string* why) const {
    const auto no = [why](const char* text) {
        if (why) *why = text;
        return false;
    };
    if ((!anchor1_.valid() || !anchor2_.valid()) &&
        picture("chardeadpanel", "grabbed/chardeadpanel") == nullptr)
        return no("Không có hình bảng Trạng thái bất tỉnh của game, nên không nhận ra được lúc "
                  "nhân vật chết.");
    if (picture("hoisinh_confirm", "grabbed/hoisinh_confirm") == nullptr)
        return no("Không có hình câu hỏi dùng giấy hồi sinh, nên không bấm Xác Nhận được.");
    // Without these the bin's dialogs cannot be told from the revive
    // question, and an answer meant for one could go to the other.
    if (!bag::can_check(store_, TrashList::Action::Delete))
        return no("Không có hình các hộp thoại của thùng rác, nên không phân biệt được chúng với "
                  "câu hỏi hồi sinh.");
    return true;
}

Reviver::Seen Reviver::see(const Frame& f, bool question_anyway) const {
    Seen s;
    if (!f.valid()) return s;
    const ScreenRect all{ 0, 0, f.width, f.height };

    // The panel, by GoC's two corners: the village button's, then the item
    // button's 121-126 pixels to its right. The first corner alone also
    // matches the Cash button's, which has nothing at that distance to its
    // right; only the pair is the panel.
    const Frame* whole = picture("chardeadpanel", "grabbed/chardeadpanel");
    const auto found_whole = [&](const TemplateMatcher::Match& m) {
        s.panel = true;
        s.panel_score = m.score;
        s.item = ScreenRect{ m.at.x + kPanelItemX, m.at.y + kPanelItemY, kItemW, kItemH };
    };
    if (anchor1_.valid() && anchor2_.valid()) {
        // The village corner's places, the near ones too: the strong ones are
        // where the pair is looked for, and every one is where the whole
        // panel would be looked for if the pair is not there -- a corner
        // drawn a little differently, the item button lit up under the
        // pointer that has just clicked it. The whole panel is then only
        // matched round those places, not over the whole screen.
        const auto corners =
            TemplateMatcher::find_all(f, all, anchor1_, kCornerNear, kCornerPlaces);
        for (const auto& a : corners) {
            if (a.score < kAnchorScore) continue;
            const ScreenRect right_of = on_frame(
                f, ScreenRect{ a.at.x + kAnchorDxMin, a.at.y + kAnchorDyMin,
                               kAnchorSize + kAnchorDxMax - kAnchorDxMin,
                               kAnchorSize + kAnchorDyMax - kAnchorDyMin });
            const auto b = TemplateMatcher::find(f, right_of, anchor2_, kAnchorScore);
            if (!b.found) continue;
            s.panel = true;
            s.by_anchors = true;
            s.panel_score = std::min(a.score, b.score);
            s.item = ScreenRect{ b.at.x, b.at.y, kItemW, kItemH };
            break;
        }
        // Each place taken for the village button's corner -- and a strong
        // one for the Cash button's as well: with the village button lit up
        // under the pointer, the Cash corner is the strongest place left, and
        // the panel lies 280 pixels left of it rather than 34.
        for (std::size_t i = 0; !s.panel && whole != nullptr && i < corners.size(); ++i) {
            for (const int dx : { kPanelVillageX, kPanelCashX }) {
                if (dx == kPanelCashX && corners[i].score < kAnchorScore) break;
                const ScreenRect around = on_frame(
                    f, ScreenRect{ corners[i].at.x - dx - kPanelSlack,
                                   corners[i].at.y - kPanelItemY - kPanelSlack,
                                   whole->width + 2 * kPanelSlack, whole->height + 2 * kPanelSlack });
                const auto m = TemplateMatcher::find(f, around, *whole, kPanelScore);
                if (m.found) {
                    found_whole(m);
                    break;
                }
            }
        }
    } else if (whole != nullptr) {
        // No corners to go by at all: the whole panel, anywhere.
        const auto m = TemplateMatcher::find(f, all, *whole, kPanelScore);
        if (m.found) found_whole(m);
    }

    // The question, held high above the bin's dialogs and the drop question.
    // Its score is kept from 0.5 up, to say how near a dialog came. Only at a
    // death when a turn looks: nothing else answers it.
    const Frame* q = (question_anyway || s.panel)
                         ? picture("hoisinh_confirm", "grabbed/hoisinh_confirm")
                         : nullptr;
    if (q != nullptr) {
        const auto m = TemplateMatcher::find(f, all, *q, 0.5f);
        if (m.found) {
            s.question_score = m.score;
            if (m.score >= kQuestionScore) {
                s.question = true;
                s.question_at = m.at;
            }
        }
    }

    // Only when there is something to answer: the dialogs cost a look of
    // their own. Any of the bin's up and nothing is clicked at all; the
    // others are left alone, and nothing is clicked where they lie.
    if (s.dead()) {
        const int pair_x = s.question_at.x + kPairDx, pair_y = s.question_at.y + kPairDy;
        for (const bag::Dialog& d : bag::find_dialogs(f, store_)) {
            const bool its_own = s.question && std::abs(d.pair.x - pair_x) <= 3 &&
                                 std::abs(d.pair.y - pair_y) <= 3;
            if (d.ours()) {
                s.bin_asking = true;
                if (its_own) s.question = false;       // the bin's, whatever it looked like
                continue;
            }
            if (its_own) continue;
            s.others.push_back(ScreenRect{ d.pair.x - kDialogLeft, d.pair.y - kDialogUp, kDialogW,
                                           kDialogH });
        }
    }
    return s;
}

bool Reviver::in_the_way(const Seen& s, int x, int y, int w, int h) {
    if (x < 0 || y < 0 || x >= w || y >= h) return true;
    for (const ScreenRect& r : s.others)
        if (r.contains(x, y)) return true;
    return false;
}

bool Reviver::aim(const Seen& s, int frame_w, int frame_h, int& x, int& y, bool& on_question,
                  std::string& why_not) {
    if (s.bin_asking) {
        why_not = "dead, and one of the trash bin's dialogs is up -- nothing is clicked until it "
                  "has closed";
        return false;
    }
    on_question = s.question;
    if (s.question) {
        x = s.confirm_x();
        y = s.confirm_y();
        return true;
    }
    x = s.item_x();
    y = s.item_y();
    if (in_the_way(s, x, y, frame_w, frame_h)) {
        why_not = "dead, and another dialog is over the revive-item button -- waiting for it to "
                  "close";
        return false;
    }
    return true;
}

Reviver::Turn Reviver::give_up() {
    if (!gave_up_) {
        gave_up_ = true;
        const std::string reason = std::to_string(kAttempts) +
                                   " tries at reviving did not bring the character back -- "
                                   "probably out of revive items; GoBot never revives in the "
                                   "village";
        say(reason);
        if (out_hook_) out_hook_(reason);
    }
    return Turn::OutOfTries;
}

// ----------------------------------------------------------------- one turn

Reviver::Turn Reviver::turn() {
    if (stop_now()) return Turn::Stopped;
    Frame frame;
    Seen s;
    if (!fresh(frame, s)) return stop_now() ? Turn::Stopped : Turn::Blind;

    // A death is the panel. The question on its own is one only once the
    // panel has been seen at this death -- before it came up, or under it.
    // Every dialog with Xác Nhận and Hủy Bỏ has the same frame, and one that
    // opens with the same words scores nearly as well as the revive question:
    // with nobody dead it is somebody else's to answer, and Xác Nhận on it
    // could spend an item. (Nor is it looked for then: see look().)
    if (s.panel) death_on_ = true;
    if (!dead_at(s)) {
        // A death is over once the character has stayed alive a while; the
        // next one gets its own tries. Not on one look: the panel can
        // flicker as an attempt goes through, and a game with no revive item
        // left may put it away for a moment after Xác Nhận.
        const auto t = now();
        if (!alive_seen_) {
            alive_seen_ = true;
            alive_since_ = t;
        } else if (t - alive_since_ >= std::chrono::milliseconds(kAliveMs)) {
            if (tries_ > 0)
                say("the character has been alive for " + std::to_string(kAliveMs / 1000) +
                    " s -- the next death gets its own " + std::to_string(kAttempts) + " tries");
            tries_ = 0;
            gave_up_ = false;
            death_on_ = false;
        }
        said_waiting_ = false;
        return Turn::Alive;
    }
    alive_seen_ = false;

    // Every try used: nothing more until the character is seen alive.
    if (tries_ >= kAttempts) return give_up();

    const auto wait = [this](const std::string& why) {
        if (!said_waiting_) say(why);
        said_waiting_ = true;
        return Turn::Waiting;
    };
    int x = 0, y = 0;
    bool on_question = false;
    std::string why_wait;
    if (!aim(s, frame.width, frame.height, x, y, on_question, why_wait)) return wait(why_wait);

    // Whoever runs this holds everything else off -- the mouse included --
    // before the first click. Put off, nothing is clicked and nothing counted.
    if (guard_.take && !guard_.take()) return stop_now() ? Turn::Stopped : Turn::Waiting;
    struct Release {
        const Guard& g;
        ~Release() { if (g.done) g.done(); }
    } release{ guard_ };

    // Looked at again before the first click, with the game in front. Taking
    // the guard can mean waiting out a whole trash pass, and the look that
    // decided this was taken before it -- and, for a game read off the
    // screen, before the game was brought forward. Meanwhile the character
    // may have been revived by hand, or the pass may have left the bin's own
    // question where the revive's was. Anything changed -- nobody dead now,
    // the bin asking, a dialog over the button, the place to click moved --
    // and nothing is clicked and no try counted: the next turn decides
    // afresh.
    if (!hands_.click) bring_forward();
    {
        Frame again;
        Seen s2;
        if (!fresh(again, s2))
            return stop_now() ? Turn::Stopped
                              : wait("dead, and the game could not be looked at again just "
                                     "before the click -- nothing clicked");
        if (s2.panel) death_on_ = true;
        if (!dead_at(s2)) return wait("the panel went while waiting to click -- nothing clicked");
        int x2 = 0, y2 = 0;
        bool q2 = false;
        std::string why2;
        if (!aim(s2, again.width, again.height, x2, y2, q2, why2)) return wait(why2);
        if (q2 != on_question || std::abs(x2 - x) > kAimSlack || std::abs(y2 - y) > kAimSlack)
            return wait("the screen changed while waiting to click -- nothing clicked; it is "
                        "looked at afresh");
        frame = std::move(again);
        s = s2;
        x = x2;
        y = y2;
    }

    const int attempt = tries_ + 1;
    const char* what = on_question ? "Xac Nhan on the revive question" : "the revive-item button";
    std::string not_out;
    if (!click(x, y, what, &not_out)) {
        // Not clicked -- the game not in front, or something over it. Not a
        // try: nothing reached the game.
        return stop_now() ? Turn::Stopped : wait("dead, and nothing clicked: " + not_out);
    }
    said_waiting_ = false;
    tries_ = attempt;
    say("attempt " + std::to_string(attempt) + " of " + std::to_string(kAttempts) +
        " at this death -- " +
        (s.by_anchors ? "the panel found by its corners"
         : s.panel    ? "the panel found by its picture"
                      : "the revive question up over the panel"));
    auto last_click = now();
    std::string why_not = "Xac Nhan was clicked and the character is still dead";

    // After the item button, the question: answered as soon as it is up.
    if (!on_question) {
        Seen q;
        Frame qf;
        const bool came = watch(kQuestionWaitMs, [this](const Seen& v) {
            return v.question || v.bin_asking || !dead_at(v);
        }, q, qf);
        if (stop_now()) return Turn::Stopped;
        if (came && q.bin_asking) {
            why_not = "one of the bin's dialogs came up instead of the revive question, and "
                      "nothing was answered";
        } else if (came && q.question) {
            if (click(q.confirm_x(), q.confirm_y(), "Xac Nhan on the revive question"))
                last_click = now();
            else
                why_not = "the revive question came up and Xac Nhan could not be clicked";
        } else if (came) {
            // Gone without asking: looked at below like any other.
        } else {
            why_not = "the revive question never came up after the revive-item button";
        }
    }

    // Revived: the panel and the question gone on two looks running with HP
    // read above 0 -- or, where HP cannot be read, gone for kSettleBlindMs.
    // With no revive item left the game may put the panel away for a moment
    // after Xác Nhận, and two looks half a second apart took that for a
    // revive: the tries were given back every time, and the bot clicked
    // every couple of seconds instead of stopping.
    const auto deadline = last_click + std::chrono::milliseconds(kReviveWaitMs + kSettleBlindMs);
    int clear = 0;
    std::chrono::steady_clock::time_point clear_from{};
    bool up = false;
    Seen v;
    Frame vf;
    while (!stop_now() && now() < deadline) {
        if (fresh(vf, v)) {
            const float hp = hp_ ? hp_() : -1.0f;
            if (dead_at(v)) {
                if (clear > 0) why_not = "the panel went for a moment and came back";
                clear = 0;
            } else if (hp >= 0.0f && hp < kHpZero) {
                clear = 0;
                why_not = "the panel went, and HP still reads 0";
            } else {
                if (clear++ == 0) clear_from = now();
                const bool sure =
                    hp >= kHpZero || now() - clear_from >= std::chrono::milliseconds(kSettleBlindMs);
                if (clear >= 2 && sure) {
                    up = true;
                    break;
                }
            }
        }
        if (!wait_ms(kLookMs)) break;
    }
    if (stop_now()) return Turn::Stopped;

    if (up) {
        ++revived_;
        // The try is not given back yet: that waits for the character to
        // stay up for kAliveMs (see above). A panel back at once is this
        // death still, so a game that only hides it for a while when there
        // is no revive item runs out of tries, and the bot stops, instead
        // of being revived for ever.
        death_on_ = false;
        alive_seen_ = true;
        alive_since_ = now();
        say("revived, on attempt " + std::to_string(attempt));
        if (revived_hook_) revived_hook_();
        return Turn::Revived;
    }

    keep(vf.valid() ? vf : frame);
    say("attempt " + std::to_string(attempt) + " of " + std::to_string(kAttempts) +
        " did not revive: " + why_not);
    if (tries_ >= kAttempts) return give_up();
    return Turn::Failed;
}

bool Reviver::watch(int ms, const std::function<bool(const Seen&)>& want, Seen& last,
                    Frame& frame) {
    const auto until = now() + std::chrono::milliseconds(ms);
    while (!stop_now()) {
        if (fresh(frame, last) && want(last)) return true;
        if (now() >= until) return false;
        if (!wait_ms(kLookMs)) return false;
    }
    return false;
}

bool Reviver::fresh(Frame& f, Seen& s) {
    s = Seen{};
    if (!grab_ || !grab_(f) || !f.valid()) return false;
    s = see(f, death_on_);
    return true;
}

// ------------------------------------------------------------------ hands

bool Reviver::click(int x, int y, const char* what, std::string* why_not) {
    if (stop_now()) return false;
    if (!hands_.click) {
        bring_forward();
        if (!mouse_ok(x, y, why_not)) return false;
        // Asked once more at the last moment: bringing the game forward
        // takes a while, and a stop pressed meanwhile means no click.
        if (stop_now()) return false;
    }
    const bool ok = hands_.click ? hands_.click(x, y) : input_.click_at(x, y);
    if (ok) {
        say(std::string("clicked ") + what + " at " + at(x, y));
    } else {
        say(std::string("could not click ") + what + " at " + at(x, y));
        if (why_not) *why_not = "the click did not go out";
    }
    return ok;
}

bool Reviver::mouse_ok(int x, int y, std::string* why_not) {
    // A click goes to whatever is under the pointer, and the game is read
    // off the screen or drawn by itself whatever is over it: the game has to
    // be in front, and the point on it.
    const auto no = [why_not](const std::string& why) {
        if (why_not) *why_not = why;
        return false;
    };
    if (!input_.window_focused()) return no("the game is not in front");
    HWND game = static_cast<HWND>(input_.target_window());
    POINT p{ x, y };
    if (game == nullptr || !ClientToScreen(game, &p)) return no("the game's window is gone");
    HWND under = WindowFromPoint(p);
    if (under == nullptr || (under != game && GetAncestor(under, GA_ROOT) != game))
        return no("another window is over the game at " + at(x, y));
    return true;
}

void Reviver::bring_forward() {
    if (input_.window_focused()) return;
    input_.focus_window();
    sleep(300);
}

// ------------------------------------------------------------------- time

bool Reviver::stop_now() const {
    return cancelled_.load() || (stop_ && stop_());
}

bool Reviver::wait_ms(int ms) {
    for (int slept = 0; slept < ms; slept += 50) {
        if (stop_now()) return false;
        sleep(std::min(50, ms - slept));
    }
    return !stop_now();
}

std::chrono::steady_clock::time_point Reviver::now() const {
    return clock_.now ? clock_.now() : std::chrono::steady_clock::now();
}

void Reviver::sleep(int ms) {
    if (ms <= 0) return;
    if (clock_.sleep) clock_.sleep(ms);
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// -------------------------------------------------------------- reporting

void Reviver::say(const std::string& line) const {
    if (report_) report_(line);
}

void Reviver::keep(const Frame& f) {
    if (debug_dir_.empty() || !f.valid()) return;
    CreateDirectoryA(debug_dir_.c_str(), nullptr);

    // The newest few: the oldest go first, and the names sort by time.
    std::vector<std::string> old;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((debug_dir_ + "\\revive_*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do old.push_back(fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(old.begin(), old.end());
    for (std::size_t i = 0; i + kKeptFrames <= old.size(); ++i)
        DeleteFileA((debug_dir_ + "\\" + old[i]).c_str());

    static int n = 0;
    const std::string path =
        debug_dir_ + "\\revive_" + stamp() + "_" + std::to_string(n++ % 100) + ".png";
    if (save_png(f, path)) say("kept the screen as " + path);
}

} // namespace gobot
