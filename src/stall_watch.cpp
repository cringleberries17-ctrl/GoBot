#include "gobot/stall_watch.hpp"
#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/game_install.hpp"
#include "gobot/logger.hpp"
#include "gobot/session_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>

#include <windows.h>

namespace gobot {

void StallSettings::load(const IniFile& ini) {
    enabled = ini.get_bool("Crash", "stall_enabled", false);
    stall_min = std::clamp(ini.get_int("Crash", "stall_min", 5), kMinMinutes, kMaxMinutes);
    loading_max_min = std::clamp(ini.get_int("Crash", "stall_loading_max_min", 15), stall_min,
                                 kMaxLoadingMinutes);
}

const char* StallWatch::state_name(State s) {
    switch (s) {
        case State::Off:      return "off";
        case State::Watching: return "watching";
        case State::Paused:   return "paused";
        case State::Counting: return "counting";
        case State::Stalled:  return "stalled";
    }
    return "?";
}

const char* StallWatch::screen_name(Screen s) {
    switch (s) {
        case Screen::Unknown:   return "unknown";
        case Screen::Loading:   return "loading";
        case Screen::LoginPage: return "page";
    }
    return "?";
}

void StallWatch::restart(std::uint64_t now_ms) {
    counted_ms_ = 0;
    last_counted_ = false;
    changes_.clear();
    pages_.clear();
    grace_until_ms_ = now_ms + kGraceMs;
    last_ = Verdict{};
    last_.state = settings_.enabled ? State::Watching : State::Off;
}

StallWatch::Verdict StallWatch::look(const StallLook& l, std::uint64_t now_ms) {
    Verdict v;
    v.limit_ms = static_cast<std::uint64_t>(settings_.stall_min) * 60'000ull;

    // How long since the last look, as far as counting goes: a long gap is
    // the keeper busy elsewhere or the machine asleep, not minutes watched.
    const std::uint64_t step =
        last_ms_ != 0 && now_ms > last_ms_ ? std::min(now_ms - last_ms_, kMaxStepMs) : 0;
    last_ms_ = now_ms;

    const auto clear = [&] {
        counted_ms_ = 0;
        last_counted_ = false;
        changes_.clear();
        pages_.clear();
    };

    if (!settings_.enabled) {
        clear();
        v.state = State::Off;
        last_ = v;
        return v;
    }

    // In the world: nothing to count. Dead: nothing to count either -- the
    // reviver, or the player's rule that the bot stops when it cannot
    // revive, deals with that, and a restart would throw the body out of the
    // map the player is reviving it on.
    if (l.hud || l.death) {
        clear();
        v.state = State::Watching;
        last_ = v;
        return v;
    }

    // Looks that cannot be trusted, or minutes GoBot itself has the game in
    // hand, hold the count where it is. They do not start it again: a dialog
    // GoBot looked at for ten seconds is not the HUD coming back.
    if (l.blind || l.held || l.crossing || now_ms < grace_until_ms_) {
        last_counted_ = false;
        v.state = State::Paused;
        v.counted_ms = counted_ms_;
        v.screen = last_.screen;
        last_ = v;
        return v;
    }

    // Counted from one counting look to the next, so a pause loses at most
    // one step rather than adding the whole of itself.
    if (last_counted_) counted_ms_ += step;
    last_counted_ = true;

    changes_.push_back(l.changed);
    pages_.push_back(l.login_page);
    while (static_cast<int>(changes_.size()) > kChangeWindow) changes_.pop_front();
    while (static_cast<int>(pages_.size()) > kChangeWindow) pages_.pop_front();
    const int moved = static_cast<int>(std::count(changes_.begin(), changes_.end(), true));
    const bool page = std::find(pages_.begin(), pages_.end(), true) != pages_.end();

    // A login page waits stall_min whatever it does -- the character select
    // is animated, and it is exactly the screen a client left on its own
    // ends up at. A screen that keeps moving and is none of them is taken
    // for a map loading, and is given loading_max_min.
    if (page) {
        v.screen = Screen::LoginPage;
    } else if (moved >= kChangeAtLeast) {
        v.screen = Screen::Loading;
        v.limit_ms = static_cast<std::uint64_t>(settings_.loading_max_min) * 60'000ull;
    } else {
        v.screen = Screen::Unknown;
    }
    v.counted_ms = counted_ms_;
    v.state = counted_ms_ >= v.limit_ms ? State::Stalled : State::Counting;
    last_ = v;
    return v;
}

std::vector<std::uint8_t> StallEyes::thumbnail(const Frame& f) {
    std::vector<std::uint8_t> out;
    if (!f.valid() || f.width < kThumbW || f.height < kThumbH) return out;
    out.resize(static_cast<std::size_t>(kThumbW) * kThumbH);
    for (int ty = 0; ty < kThumbH; ++ty) {
        const int y0 = ty * f.height / kThumbH, y1 = (ty + 1) * f.height / kThumbH;
        for (int tx = 0; tx < kThumbW; ++tx) {
            const int x0 = tx * f.width / kThumbW, x1 = (tx + 1) * f.width / kThumbW;
            std::uint64_t sum = 0, n = 0;
            for (int y = y0; y < y1; ++y) {
                const std::uint8_t* p =
                    &f.pixels[(static_cast<std::size_t>(y) * f.width + static_cast<std::size_t>(x0)) * 4];
                for (int x = x0; x < x1; ++x, p += 4) {
                    sum += static_cast<std::uint64_t>(p[0] + p[1] + p[2]);
                    n += 3;
                }
            }
            out[static_cast<std::size_t>(ty) * kThumbW + tx] =
                static_cast<std::uint8_t>(n ? sum / n : 0);
        }
    }
    return out;
}

double StallEyes::difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.empty() || a.size() != b.size()) return -1.0;
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < a.size(); ++i) sum += static_cast<std::uint64_t>(std::abs(a[i] - b[i]));
    return static_cast<double>(sum) / static_cast<double>(a.size());
}

bool StallEyes::changed(const Frame& f) {
    std::vector<std::uint8_t> now = thumbnail(f);
    const double d = difference(previous_, now);
    previous_ = std::move(now);
    return d > kChangedAbove;
}

bool stall_frame_trusted(bool grabbed, bool minimised, bool from_screen, bool clear) {
    return grabbed && !minimised && (!from_screen || clear);
}

bool stall_screen_clear(bool foreground, bool uncovered, bool hud_corner_seen) {
    return foreground && uncovered && hud_corner_seen;
}

bool stall_window_is_the_games(void* window, const std::string& game_process,
                               const GameInstall& install) {
    if (window == nullptr) return false;
    for (const InputManager::WindowInfo& w : game_windows(game_process, install))
        if (w.hwnd == window) return true;
    return false;
}

bool login_page_up(const Frame& f, const AutoLogin::Settings& login, const TemplateStore& pages) {
    if (!f.valid()) return false;
    const ScreenRect all{ 0, 0, f.width, f.height };
    for (const AutoLogin::Page& page : login.pages) {
        if (page.on != AutoLogin::On::Game || page.picture.empty()) continue;
        const Frame* picture = pages.get(page.picture);
        if (picture != nullptr && TemplateMatcher::find(f, all, *picture, page.score).found) return true;
    }
    return false;
}

StallLooker::StallLooker() = default;
StallLooker::~StallLooker() = default;

void StallLooker::load(const std::string& templates_ini) {
    have_landmark_ = TemplateStore::load_one("hpmp_indicator2", templates_ini, landmark_);
    // The death panel by GoC's corners or the player's own picture, and the
    // revive question, for the reviver's own look; the bin's two dialogs and
    // their pair of buttons, which it tells the question from -- kept by the
    // names bag.cpp looks them up by too. This client's Xác Nhận in its three
    // drawings, for reading a dialog.
    struct Wanted {
        const char* name;
        const char* also;       ///< a second name it is looked up by, or null
    };
    for (const Wanted& w : { Wanted{ "dialog_panic", nullptr }, Wanted{ "chardeadpanel", nullptr },
                             Wanted{ "hoisinh_confirm", nullptr },
                             Wanted{ "confirm_button", "grabbed/confirm_button" },
                             Wanted{ "del_item_confirm_1", "grabbed/del_item_confirm_1" },
                             Wanted{ "del_item_confirm_2", "grabbed/del_item_confirm_2" },
                             Wanted{ "inven/1/ok", nullptr }, Wanted{ "inven/1/ok1", nullptr },
                             Wanted{ "inven/1/ok2", nullptr } }) {
        Frame f;
        if (!TemplateStore::load_one(w.name, templates_ini, f)) continue;
        if (w.also) pictures_.put(w.also, f);
        pictures_.put(w.name, f);
    }
    font_.load(GlyphFont::default_dir());
    // An input manager with no window, which the reviver is never asked to
    // use: only its look is.
    no_hands_ = std::make_unique<InputManager>();
    reviver_ = std::make_unique<Reviver>(*no_hands_, pictures_, [](Frame&) { return false; });
}

bool StallLooker::grab(void* window, Frame& f, bool& trusted) {
    trusted = false;
    const HWND w = static_cast<HWND>(window);
    const bool minimised = w == nullptr || IsIconic(w) != FALSE;
    const bool grabbed = !minimised && capture_.grab(window, f) && f.valid();
    // Read off the screen, the frame is the game only while the game is in
    // front with nothing over it -- the crossing's own rule for trusting one
    // -- and with the HUD's corner on a screen. The crossing lets a window
    // part off the screen be looked at; here, a HUD dragged past the edge
    // reads black, black is no HUD, and the healthy game would be ended.
    const bool from_screen = grabbed && capture_.from_screen();
    const bool clear = from_screen && stall_screen_clear(GetForegroundWindow() == w, AutoLogin::uncovered(window),
                                                         AutoLogin::part_seen(window, kHudCorner));
    trusted = stall_frame_trusted(grabbed, minimised, from_screen, clear);
    return grabbed;
}

StallLook StallLooker::look_at(const Frame& f, bool trusted, bool held, bool crossing,
                               const AutoLogin::Settings& login, const TemplateStore& pages) {
    StallLook l;
    l.held = held;
    l.crossing = crossing;
    if (!trusted || !f.valid()) {
        l.blind = true;
        eyes_.forget();
        return l;
    }
    // The HUD's Level plate where it always sits, or the bars as a pair: the
    // bars alone are not found on most frames with the bag open. The plate
    // first: it is a single small match, and finding the bars writes a line
    // to the log each time -- every few seconds, all night.
    l.hud = (have_landmark_ && AutoLogin::plate_at(f, &landmark_)) || SessionManager::hud_present(f);
    if (!l.hud) {
        l.death = reviver_ && reviver_->look(f).dead();
        l.login_page = login_page_up(f, login, pages);
    }
    l.changed = eyes_.changed(f);
    return l;
}

std::string StallLooker::ok_popup(const Frame& f) const {
    if (!f.valid()) return "none";
    const ScreenRect all{ 0, 0, f.width, f.height };
    for (const char* name : { "inven/1/ok", "inven/1/ok1", "inven/1/ok2" }) {
        const Frame* ok = pictures_.get(name);
        if (ok == nullptr) continue;
        const auto m = TemplateMatcher::find(f, all, *ok, 0.90f);
        if (!m.found) continue;

        // A question has Hủy Bỏ beside its Xác Nhận. bag::find_dialogs knows
        // every such pair by both of the client's drawings of it -- the one
        // pair picture alone scores 0.79 on the bin's question, the revive
        // question and the delete question, under what it takes -- and places
        // the dialog from it as the bin's own pictures sit on their pair.
        bool question = false;
        int left = m.at.x - 146, top = m.at.y - 58;
        for (const bag::Dialog& d : bag::find_dialogs(f, pictures_)) {
            const ScreenRect near_pair{ d.pair.x - 4, d.pair.y - 4, d.pair.w + 8, d.pair.h + 8 };
            if (!near_pair.contains(m.at.x + m.at.w / 2, m.at.y + m.at.h / 2)) continue;
            question = true;
            left = d.pair.x - 101;
            top = d.pair.y - 51;
            break;
        }
        // Else the one pair picture, as before. Its Xác Nhận sits further
        // left in the dialog than a notice's lone one: 104-106 pixels from the
        // dialog's left against 146-148, measured on the client's own dialogs.
        if (!question) {
            if (const Frame* pair = pictures_.get("confirm_button")) {
                const int x0 = std::max(0, m.at.x - 12), y0 = std::max(0, m.at.y - 8);
                const ScreenRect around{ x0, y0, std::min(f.width - x0, pair->width + 24),
                                         std::min(f.height - y0, pair->height + 16) };
                question = around.w >= pair->width && around.h >= pair->height &&
                           TemplateMatcher::find(f, around, *pair, 0.82f).found;
            }
            if (question) {
                left = m.at.x - 105;
                top = m.at.y - 56;
            }
        }
        std::string words;
        if (font_.loaded())
            for (int line = 0; line < 2; ++line) {
                const int x0 = std::max(0, left + 20), y0 = std::max(0, top + 12 + line * 14);
                const ScreenRect r{ x0, y0, std::min(158, f.width - x0), std::min(15, f.height - y0) };
                if (r.w <= 0 || r.h <= 0) continue;
                const std::string text = font_.read_line(f, r);
                if (!text.empty()) words += (words.empty() ? "" : " ") + text;
            }
        return std::string(question ? "a question" : "a notice") + " with Xác Nhận at " +
               std::to_string(m.at.x) + "," + std::to_string(m.at.y) +
               (words.empty() ? std::string{} : ": \"" + words + "\"");
    }
    return "none";
}

bool keep_stall_frame(const Frame& f, const std::string& dir, std::size_t keep) {
    // Through the wide calls: the folder is beside GoBot, wherever the player
    // unzipped it, Vietnamese letters and all.
    if (!f.valid() || !make_folder(dir)) return false;
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char name[48];
    std::strftime(name, sizeof name, "stall_%Y%m%d_%H%M%S.png", &tm);
    if (!save_png(f, dir + "\\" + name)) return false;
    // Named by when they were taken, they sort oldest first.
    std::vector<std::string> names = files_in(dir, "stall_*.png");
    std::sort(names.begin(), names.end());
    for (std::size_t i = 0; i + keep < names.size(); ++i) remove_file(dir + "\\" + names[i]);
    return true;
}

namespace {

std::string screen_words(StallWatch::Screen s) {
    switch (s) {
        case StallWatch::Screen::LoginPage: return "a login page";
        case StallWatch::Screen::Loading:   return "a screen that keeps changing, no login page";
        case StallWatch::Screen::Unknown:   break;
    }
    return "screen static, no login page";
}

} // namespace

void StallKeeper::set_settings(const StallSettings& s, const std::string& templates_ini) {
    const bool was = watch_.settings().enabled;
    watch_.set_settings(s);
    if (s.enabled && !loaded_) {
        loaded_ = true;
        looker_.load(templates_ini);
    }
    if (s.enabled && !was)
        GB_INFO("Keeper: the watchdog is on -- the client is ended and logged back into when nobody has "
                "been in the world for %d min (%d while the screen keeps changing)",
                s.stall_min, s.loading_max_min);
}

void StallKeeper::restart(std::uint64_t now_ms) {
    watch_.restart(now_ms);
    looker_.forget();
    logged_minute_ = -1;
}

bool StallKeeper::check(void* window, const std::string& game_process, bool held,
                        const AutoLogin::Settings& login, const TemplateStore& pages, std::uint64_t now_ms,
                        CrashWatch::Look& out, const std::string& debug_dir) {
    if (!watch_.settings().enabled || now_ms < next_ms_) return false;
    next_ms_ = now_ms + kLookMs;
    // Only a window of the game's own, by where its program runs from: the
    // window GoBot follows may have been picked by hand, or found by its
    // title alone, and a window that is not the game never shows the HUD --
    // its looks would end a healthy game. Not looked at, it is a blind look.
    looking_at_the_game_ =
        stall_window_is_the_games(window, game_process, install_set_ ? install_ : GameInstall::here());
    if (!looking_at_the_game_ && !said_not_the_game_)
        GB_INFO("Keeper: the watchdog counts nothing for now -- %s",
                window == nullptr ? "no window of the game's is followed"
                                  : "the window followed is not one of the game's own");
    said_not_the_game_ = !looking_at_the_game_;
    Frame frame;
    bool trusted = false;
    if (looking_at_the_game_) looker_.grab(window, frame, trusted);
    return judge(frame, trusted, held, login, pages, now_ms, out, debug_dir);
}

bool StallKeeper::judge(const Frame& frame, bool trusted, bool held, const AutoLogin::Settings& login,
                        const TemplateStore& pages, std::uint64_t now_ms, CrashWatch::Look& out,
                        const std::string& debug_dir) {
    const StallLook look = looker_.look_at(frame, trusted, held, false, login, pages);
    const StallWatch::Verdict v = watch_.look(look, now_ms);
    if (v.state == StallWatch::State::Counting && v.minutes() > 0 && v.minutes() != logged_minute_) {
        logged_minute_ = v.minutes();
        GB_INFO("Keeper: no HUD for %d of %d min (%s)", v.minutes(), static_cast<int>(v.limit_ms / 60'000),
                screen_words(v.screen).c_str());
    } else if (v.state == StallWatch::State::Watching) {
        logged_minute_ = -1;
    }
    if (v.state != StallWatch::State::Stalled) return false;

    // Stalled. Before anything is ended: whatever dialog with a Xác Nhận is
    // up, read for the log -- and the frame kept -- but nothing clicked.
    const int minutes = v.minutes();
    const std::string popup = looker_.ok_popup(frame);
    if (!debug_dir.empty() && keep_stall_frame(frame, debug_dir))
        GB_INFO("Keeper: kept what the screen showed in keeper_debug");
    out = CrashWatch::Look{};
    out.what = CrashWatch::Trouble::Stalled;
    out.minutes = minutes;
    out.detail = "no character in the world for " + std::to_string(minutes) + " min (screen: " +
                 StallWatch::screen_name(v.screen) + "; popup: " + popup + ")";
    GB_WARN("Keeper: stalled -- %s", out.detail.c_str());
    // Whatever comes of it -- a restart, or a person needed -- this count is
    // over; the next starts once the game is in the world again.
    restart(now_ms);
    return true;
}

} // namespace gobot
