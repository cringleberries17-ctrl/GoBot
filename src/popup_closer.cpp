#include "gobot/popup_closer.hpp"
#include "gobot/config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

#include <windows.h>

namespace gobot {

namespace {

/// Part of a picture, as it is; empty when it does not fit.
Frame crop(const Frame& f, const ScreenRect& r) {
    Frame out;
    if (!f.valid() || r.x < 0 || r.y < 0 || r.right() > f.width || r.bottom() > f.height || !r.valid())
        return out;
    out.width = r.w;
    out.height = r.h;
    out.pixels.resize(static_cast<std::size_t>(r.w) * r.h * 4);
    for (int y = 0; y < r.h; ++y)
        std::copy_n(f.pixels.begin() +
                        static_cast<std::ptrdiff_t>((static_cast<std::size_t>(r.y + y) * f.width + r.x) * 4),
                    static_cast<std::size_t>(r.w) * 4,
                    out.pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * r.w * 4));
    return out;
}

/// The widest part of a picture with nothing see-through in it, trimmed from
/// each edge inwards that has any: the same picture to the matcher, matched
/// without a mask, which costs a quarter of a masked match. `at` gets where
/// the part starts in the picture -- GoC's ok.png has a see-through column on
/// its right only, and a match is then that much further in than the button.
Frame opaque_middle(const Frame& f, ScreenRect* at = nullptr) {
    ScreenRect r{ 0, 0, f.width, f.height };
    if (at) *at = r;
    if (!f.valid()) return f;
    const auto clear_at = [&](int x, int y) {
        return f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4 + 3] < TemplateMatcher::kOpaque;
    };
    const auto column_clear = [&](int x) {
        for (int y = r.y; y < r.bottom(); ++y)
            if (clear_at(x, y)) return true;
        return false;
    };
    const auto row_clear = [&](int y) {
        for (int x = r.x; x < r.right(); ++x)
            if (clear_at(x, y)) return true;
        return false;
    };
    for (bool trimmed = true; trimmed && r.w > 2 && r.h > 2;) {
        trimmed = false;
        if (column_clear(r.x))            { r.x += 1; r.w -= 1; trimmed = true; }
        if (r.w > 2 && column_clear(r.right() - 1)) { r.w -= 1; trimmed = true; }
        if (row_clear(r.y))               { r.y += 1; r.h -= 1; trimmed = true; }
        if (r.h > 2 && row_clear(r.bottom() - 1))   { r.h -= 1; trimmed = true; }
    }
    if (at) *at = r;
    return crop(f, r);
}

ScreenRect clamp_to(const Frame& f, ScreenRect r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.right()), y1 = std::min(f.height, r.bottom());
    return ScreenRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

bool close_to(int a, int b, int slack) { return std::abs(a - b) <= slack; }

bool same_place(const ScreenRect& a, const ScreenRect& b) {
    return close_to(a.x, b.x, PopupEyes::kSlack) && close_to(a.y, b.y, PopupEyes::kSlack);
}

std::string at_text(int x, int y) { return std::to_string(x) + "," + std::to_string(y); }

std::string score_text(float s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(s));
    return buf;
}

} // namespace

const char* popup_kind_name(PopupKind k) {
    switch (k) {
        case PopupKind::CheckinNotice: return "the check-in notice";
        case PopupKind::MonsterBook:   return "Yêu Quái Bách Khoa";
        case PopupKind::EscMenu:       return "the ESC menu";
    }
    return "?";
}

const char* popup_kind_vi(PopupKind k) {
    switch (k) {
        case PopupKind::CheckinNotice: return "thông báo nhận quà điểm danh";
        case PopupKind::MonsterBook:   return "Yêu Quái Bách Khoa";
        case PopupKind::EscMenu:       return "menu ESC do GoBot mở";
    }
    return "?";
}

const PopupSeen* PopupScene::find(PopupKind k) const {
    for (const PopupSeen& p : popups)
        if (p.kind == k) return &p;
    return nullptr;
}

// ---------------------------------------------------------------- the eyes

PopupEyes::PopupEyes() = default;
PopupEyes::~PopupEyes() = default;

void PopupEyes::load(const TemplateStore& store, const std::string& configs_dir) {
    store_ = &store;
    missing_.clear();
    const auto picture = [&](const char* name) -> Frame {
        if (const Frame* f = store.get(name)) return *f;
        missing_.push_back(name);
        return Frame{};
    };
    ScreenRect ok_part;
    ok_ = opaque_middle(picture("inven/1/ok"), &ok_part);
    ok_dx_ = ok_part.x;
    ok_dy_ = ok_part.y;
    const Frame notice = picture("grabbed/login_popup1");
    notice_words_ = crop(notice, ScreenRect{ kWordsX, kWordsY, kWordsW, kWordsH });
    const Frame book = picture("inven/1/monsterf");
    book_ = book.valid() ? crop(book, ScreenRect{ kBookInset, kBookInset, book.width - 2 * kBookInset,
                                                  book.height - 2 * kBookInset })
                         : Frame{};
    book_key_ = book.valid() ? crop(book, ScreenRect{ kBookKeyX, kBookKeyY, kBookKeyW, kBookKeyH }) : Frame{};
    menu_ = picture("grabbed/client_menu_dialog");
    // The close buttons: the one every window here has, and the client's two
    // other drawings of one, which a window nothing knows may wear instead.
    close_x_.clear();
    close_x_.push_back(picture("inven/1/btn_CloseDialog.spr/0"));
    for (const char* other : { "inven/1/monster-close", "inven/1/btn_CloseBox.spr/0" })
        if (const Frame* f = store.get(other)) close_x_.push_back(*f);
    // What else is up is told by the pictures the features that answer it
    // go by; without them nothing can be ruled out, and nothing is closed.
    for (const char* name : { "grabbed/confirm_button", "grabbed/inventory" })
        if (!store.get(name)) missing_.push_back(name);

    const std::string dir = configs_dir.empty() ? std::string{} : configs_dir + "\\";
    CleanSettings clean;
    if (clean.load(dir + "trash.ini")) bag_ = clean.layout;
    IniFile party;
    if (party.load(dir + "party.ini")) invite_.load(party);
    if (!store.get(invite_.picture)) missing_.push_back(invite_.picture);
    chat_.load(dir + "chat.ini");
    quest_settings_.load(dir + "quest.ini");
    quest_.load(store, quest_settings_);

    // An input manager with no window, which the reviver is never asked to
    // use: only its look is.
    no_hands_ = std::make_unique<InputManager>();
    reviver_ = std::make_unique<Reviver>(*no_hands_, store, [](Frame&) { return false; });
    std::string why;
    if (!reviver_->ready(&why)) missing_.push_back("the death panel's pictures (" + why + ")");
    if (!quest_.ready()) missing_.push_back("the Nhiệm Vụ window's pictures");
    if (!can_look_for_chat(store, chat_)) missing_.push_back("the chat bar's pictures");
}

bool PopupEyes::ready(std::string* why) const {
    if (store_ == nullptr) {
        if (why) *why = "not loaded";
        return false;
    }
    if (!missing_.empty()) {
        if (why) *why = missing_.front();
        return false;
    }
    return ok_.valid() && notice_words_.valid() && book_.valid() && menu_.valid() && !close_x_.empty() &&
           close_x_.front().valid();
}

bool PopupEyes::close_button_in(const Frame& f, const ScreenRect& area, int& x, int& y) const {
    float best = 0.0f;
    for (const Frame& pic : close_x_) {
        const ScreenRect a = clamp_to(f, area);
        if (!pic.valid() || a.w < pic.width || a.h < pic.height) continue;
        const auto m = TemplateMatcher::find(f, a, pic, kCloseScore);
        if (!m.found || m.score <= best) continue;
        best = m.score;
        x = m.at.cx();
        y = m.at.cy();
    }
    return best > 0.0f;
}

bool PopupEyes::book_title_in(const Frame& f, const ScreenRect& area) const {
    // The live client's book by its first tab, where a book with that window
    // has it (bag::find_book).
    if (store_ != nullptr && bag::find_book(f, *store_, &area).found) return true;
    if (!book_key_.valid()) return false;
    const ScreenRect a = clamp_to(f, area);
    return a.w >= book_key_.width && a.h >= book_key_.height &&
           TemplateMatcher::find(f, a, book_key_, kBookScore).found;
}

std::vector<PopupSeen> PopupEyes::popups(const Frame& f, bool with_menu, const ScreenRect* only_at) const {
    std::vector<PopupSeen> out;
    if (!f.valid()) return out;
    const ScreenRect all{ 0, 0, f.width, f.height };
    // Where the notice's button and the book's title are looked for: all of
    // the screen, or round `only_at` -- a popup where it was a moment ago,
    // give or take as much as one still counts as there.
    const ScreenRect look =
        only_at == nullptr ? all
                           : clamp_to(f, ScreenRect{ only_at->x - kSlack - 1, only_at->y - kSlack - 1,
                                                     only_at->w + 2 * (kSlack + 1), only_at->h + 2 * (kSlack + 1) });

    // The check-in notice: a Xác Nhận with the notice's words where they sit
    // in the notice, give or take two pixels. The words are what tells it:
    // every dialog's frame and buttons are the same. Measured from a lone
    // Xác Nhận's place, they are nowhere near where a two-button dialog has
    // its words.
    if (ok_.valid() && notice_words_.valid() && look.w >= ok_.width && look.h >= ok_.height) {
        for (const auto& ok : TemplateMatcher::find_all(f, look, ok_, kOkScore, 8)) {
            const int dx = ok.at.x - ok_dx_ - kNoticeOkDx, dy = ok.at.y - ok_dy_ - kNoticeOkDy;
            const ScreenRect place =
                clamp_to(f, ScreenRect{ dx + kWordsX - 2, dy + kWordsY - 2, kWordsW + 4, kWordsH + 4 });
            if (place.w < notice_words_.width || place.h < notice_words_.height) continue;
            const auto words = TemplateMatcher::find(f, place, notice_words_, kWordsScore);
            if (!words.found) continue;
            PopupSeen p;
            p.kind = PopupKind::CheckinNotice;
            p.at = ScreenRect{ dx, dy, kNoticeOkDx + ok.at.w + 14, kNoticeOkDy + ok.at.h + 6 };
            p.score = words.score;
            p.control = true;
            p.x = ok.at.cx();
            p.y = ok.at.cy();
            out.push_back(p);
        }
    }

    // The monster book as the live client draws it, by its first tab and its
    // X where the tab puts it (bag::find_book): the page a fragment's
    // right-click opens it on is a monster's, which GoC's picture below is
    // not, and the tab is on every page. Round `only_at`, where it was a
    // moment ago, when given. With its X it is the book, and that is that;
    // without, GoC's picture of the first page is looked for as well, which
    // looks for its X round its top-right corner -- and failing that it is
    // the book without its X.
    const bag::Book live = store_ != nullptr ? bag::find_book(f, *store_, only_at) : bag::Book{};
    const auto add_live = [&] {
        PopupSeen p;
        p.kind = PopupKind::MonsterBook;
        p.at = live.window;
        p.score = live.score;
        p.control = live.has_x;
        p.x = live.x.cx();
        p.y = live.x.cy();
        out.push_back(p);
    };
    bool book_seen = false;
    if (live.found && live.has_x) {
        add_live();
        book_seen = true;
    }

    // The monster book, by GoC's picture of it: its title box anywhere, then
    // the whole picture where that puts it. Its X is looked for round the
    // top-right corner of where the whole picture sits.
    const auto book_at = [&]() {
        TemplateMatcher::Match none;
        if (!book_.valid() || !book_key_.valid()) return none;
        if (look.w < book_key_.width || look.h < book_key_.height) return none;
        const auto key = TemplateMatcher::find(f, look, book_key_, kBookScore);
        if (!key.found) return none;
        const ScreenRect around = clamp_to(f, ScreenRect{ key.at.x - kBookKeyX + kBookInset - kSlack,
                                                          key.at.y - kBookKeyY + kBookInset - kSlack,
                                                          book_.width + 2 * kSlack, book_.height + 2 * kSlack });
        if (around.w < book_.width || around.h < book_.height) return none;
        return TemplateMatcher::find(f, around, book_, kBookScore);
    };
    if (!book_seen) {
        const auto m = book_at();
        if (m.found) {
            PopupSeen p;
            p.kind = PopupKind::MonsterBook;
            p.at = ScreenRect{ m.at.x - kBookInset, m.at.y - kBookInset, m.at.w + 2 * kBookInset,
                               m.at.h + 2 * kBookInset };
            p.score = m.score;
            p.control = close_button_in(f, ScreenRect{ p.at.right() - kBookXSlack, p.at.y - kBookXSlack,
                                                       2 * kBookXSlack, 2 * kBookXSlack },
                                        p.x, p.y);
            out.push_back(p);
            book_seen = true;
        }
    }
    if (!book_seen && live.found) add_live();

    // The ESC menu, and its X where the live client draws it.
    if (with_menu && menu_.valid()) {
        const auto m = TemplateMatcher::find(f, all, menu_, kMenuScore);
        if (m.found) {
            PopupSeen p;
            p.kind = PopupKind::EscMenu;
            p.at = m.at;
            p.score = m.score;
            const Frame& x = close_x_.front();
            p.control = close_button_in(f, ScreenRect{ m.at.x + kMenuXDx - kSlack, m.at.y + kMenuXDy - kSlack,
                                                       x.width + 2 * kSlack, x.height + 2 * kSlack },
                                        p.x, p.y);
            out.push_back(p);
        }
    }
    return out;
}

std::vector<std::string> PopupEyes::blockers(const Frame& f, const std::vector<PopupSeen>& popups,
                                             std::vector<std::string>* vi) const {
    std::vector<std::string> out;
    const auto add = [&](const std::string& en, const char* in_vi) {
        out.push_back(en);
        if (vi) vi->push_back(in_vi);
    };
    if (!f.valid() || store_ == nullptr) {
        add("nothing to look with", "không có gì để nhìn");
        return out;
    }
    const TemplateStore& store = *store_;
    const ScreenRect all{ 0, 0, f.width, f.height };

    // Side by side: each reads only the frame and the pictures, and a look
    // that takes the time of the slowest rather than of all of them is a
    // frame that much younger when the click goes.
    auto two_buttons = std::async(std::launch::async, [&] { return !bag::find_dialogs(f, store).empty(); });
    auto invited = std::async(std::launch::async, [&] {
        PartyInvite asking;
        asking.set_settings(invite_);
        return asking.look(f, store, nullptr).up;
    });
    auto dead = std::async(std::launch::async, [&] { return reviver_ && reviver_->look(f).dead(); });
    auto quest = std::async(std::launch::async, [&] {
        return quest_.ready() && find_quest_window(f, quest_, quest_settings_).found;
    });
    auto bag_up = std::async(std::launch::async, [&] { return bag::find(f, store, bag_).found; });
    auto chat_open = std::async(std::launch::async, [&] {
        return look_chat_bar(f, store, chat_).state == ChatBarState::Open;
    });
    // Each drawing of a close button on its own: they are matched through
    // their see-through corners, the dearest searches here by far -- a third
    // of a second together, a tenth side by side -- and the look under the
    // hold keeps the potion keys waiting for as long as it takes.
    std::vector<std::future<std::vector<TemplateMatcher::Match>>> xs;
    for (const Frame& x : close_x_)
        if (x.valid())
            xs.push_back(std::async(std::launch::async, [&f, &all, &x] {
                return TemplateMatcher::find_all(f, all, x, kCloseScore, 16);
            }));
    auto oks = std::async(std::launch::async, [&] {
        return ok_.valid() ? TemplateMatcher::find_all(f, all, ok_, kOkScore, 16)
                           : std::vector<TemplateMatcher::Match>{};
    });
    // The ESC menu by its own picture, not only by its X: an X drawn lit
    // under the pointer is not the X's picture, and the player's menu may be
    // on its way to Thoát Game. Not when it is among `popups` -- the one
    // GoBot's own ESC brought up, which its check closes.
    auto menu_up = std::async(std::launch::async, [&] {
        if (!menu_.valid() || std::any_of(popups.begin(), popups.end(),
                                          [](const PopupSeen& p) { return p.kind == PopupKind::EscMenu; }))
            return false;
        return TemplateMatcher::find(f, all, menu_, kMenuScore).found;
    });

    if (two_buttons.get()) add("a dialog with Xác Nhận and Hủy Bỏ is up", "đang có hộp thoại Xác Nhận / Hủy Bỏ");
    if (invited.get()) add("a party invitation is up", "đang có lời mời vào tổ đội");
    if (dead.get()) add("the death panel or the revive question is up", "đang có bảng hồi sinh");
    if (quest.get()) add("the Nhiệm Vụ window is open", "cửa sổ Nhiệm Vụ đang mở");
    if (bag_up.get()) add("the bag is open", "túi đồ đang mở");
    if (chat_open.get()) add("the chat line is open", "ô chat đang mở");
    if (menu_up.get()) add("the ESC menu is up", "menu ESC đang mở");

    // Every Xác Nhận on screen has to be the check-in notice's own: any other
    // is another dialog -- the party's notice, a refusal, something unknown.
    for (const auto& ok : oks.get()) {
        const bool notice = std::any_of(popups.begin(), popups.end(), [&](const PopupSeen& p) {
            return p.kind == PopupKind::CheckinNotice && p.at.contains(ok.at.cx(), ok.at.cy());
        });
        if (!notice) {
            add("another dialog with Xác Nhận is up at " + at_text(ok.at.x, ok.at.y), "đang có thông báo khác");
            break;
        }
    }
    // And every X has to be an allowed popup's own: any other is a window
    // nothing here knows -- Kho Cash, a shop, the player list.
    std::vector<TemplateMatcher::Match> found_x;
    for (auto& these : xs) {
        const std::vector<TemplateMatcher::Match> got = these.get();
        found_x.insert(found_x.end(), got.begin(), got.end());
    }
    for (const auto& x : found_x) {
        const bool known = std::any_of(popups.begin(), popups.end(), [&](const PopupSeen& p) {
            return p.control && close_to(p.x, x.at.cx(), kSlack) && close_to(p.y, x.at.cy(), kSlack);
        });
        if (!known) {
            add("a window with a close button is up at " + at_text(x.at.x, x.at.y), "đang có cửa sổ khác");
            break;
        }
    }
    return out;
}

PopupScene PopupEyes::look(const Frame& f) const {
    PopupScene s;
    s.popups = popups(f);
    if (!s.popups.empty()) s.blockers = blockers(f, s.popups);
    return s;
}

// -------------------------------------------------------------- the closer

PopupCloser::PopupCloser(InputManager& input, const PopupEyes& eyes, Grab grab)
    : input_(input), eyes_(eyes), grab_(std::move(grab)) {}

const char* PopupCloser::turn_name(Turn t) {
    switch (t) {
        case Turn::Blind:     return "blind";
        case Turn::Nothing:   return "nothing";
        case Turn::Waiting:   return "waiting";
        case Turn::Blocked:   return "blocked";
        case Turn::Held:      return "held";
        case Turn::Closed:    return "closed";
        case Turn::NotClosed: return "not closed";
        case Turn::LeftAlone: return "left alone";
    }
    return "?";
}

std::uint64_t PopupCloser::now() const {
    if (clock_.now_ms) return clock_.now_ms();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void PopupCloser::sleep(int ms) {
    if (clock_.sleep) clock_.sleep(ms);
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void PopupCloser::say(const std::string& line) {
    if (report_) report_("Popups: " + line);
}

bool PopupCloser::click(int x, int y) {
    if (hands_.click) return hands_.click(x, y);
    // The point has to be on the game, and the game in front: a click goes to
    // whatever is under the pointer. Nothing is brought forward for this.
    if (!input_.window_focused()) return false;
    HWND game = static_cast<HWND>(input_.target_window());
    POINT p{ x, y };
    if (game == nullptr || !ClientToScreen(game, &p)) return false;
    HWND under = WindowFromPoint(p);
    if (under == nullptr || (under != game && GetAncestor(under, GA_ROOT) != game)) return false;
    return input_.click_at(x, y);
}

bool PopupCloser::esc() {
    if (hands_.esc) return hands_.esc();
    if (!input_.window_focused()) return false;
    return input_.key_press(VK_ESCAPE);
}

const PopupSeen* PopupCloser::same(const PopupScene& scene, PopupKind want, const ScreenRect& at) {
    for (const PopupSeen& p : scene.popups)
        if (p.kind == want && same_place(p.at, at)) return &p;
    return nullptr;
}

PopupCloser::Turn PopupCloser::turn() {
    const std::uint64_t t = now();
    Frame f;
    if (!grab_ || !grab_(f) || !f.valid()) {
        last_vi_ = "Không chụp được màn hình game.";
        return Turn::Blind;
    }

    // The ESC menu is closed only straight after GoBot's own ESC, as part of
    // that ESC's check below. Otherwise it is the player's -- they may be on
    // their way to Thoát Game or Cửa Sổ Chọn Kênh -- and its X is one more
    // window nothing here may touch, which keeps everything else shut too.
    const auto allowed = [this](const Frame& frame) { return eyes_.popups(frame, false); };
    PopupScene scene;
    scene.popups = allowed(f);

    // Where each kind has been, and since when.
    for (int k = 0; k < kPopupKinds; ++k) {
        Track& tr = tracks_[k];
        const PopupSeen* p = scene.find(static_cast<PopupKind>(k));
        if (p == nullptr) {
            tr.seen = false;
            tr.said_blocked = false;
            continue;
        }
        if (!tr.seen || !same_place(tr.at, p->at)) {
            tr.seen = true;
            tr.at = p->at;
            tr.since = t;
            tr.said_blocked = false;
        }
    }
    if (scene.popups.empty()) {
        last_vi_ = "Không có thông báo nào cần đóng.";
        return Turn::Nothing;
    }

    // The one to close: up at one place for kSteadyMs, not put off, not tried
    // too often lately. The monster book without its X only when it is the
    // only thing up: its ESC would go to anything else first.
    const PopupSeen* pick = nullptr;
    bool waiting = false, left = false;
    for (const PopupSeen& p : scene.popups) {
        Track& tr = tracks_[static_cast<int>(p.kind)];
        tr.tries.erase(std::remove_if(tr.tries.begin(), tr.tries.end(),
                                      [&](std::uint64_t when) { return t - when >= kTriesWindowMs; }),
                       tr.tries.end());
        if (static_cast<int>(tr.tries.size()) >= kTries) {
            if (!tr.said_left) {
                say(std::string("left ") + popup_kind_name(p.kind) + " alone: tried " +
                    std::to_string(kTries) + " times in 10 minutes and it is still up");
                tr.said_left = true;
            }
            left = true;
            continue;
        }
        tr.said_left = false;
        if (t < tr.backoff_until) continue;
        if (t - tr.since < kSteadyMs) {
            waiting = true;
            continue;
        }
        if (p.kind == PopupKind::MonsterBook && !p.control && scene.popups.size() > 1) continue;
        pick = &p;
        break;
    }
    if (pick == nullptr) {
        last_vi_ = waiting ? "Thấy thông báo, chờ 3 giây rồi mới đóng."
                 : left    ? "Có thông báo đã thử đóng 2 lần không được: để yên."
                           : "Có thông báo, chờ lượt sau.";
        return waiting ? Turn::Waiting : left ? Turn::LeftAlone : Turn::Held;
    }
    const PopupKind kind = pick->kind;
    const ScreenRect where = pick->at;
    Track& tr = tracks_[static_cast<int>(kind)];

    // Nothing else may be up.
    std::vector<std::string> why_vi;
    scene.blockers = eyes_.blockers(f, scene.popups, &why_vi);
    if (scene.blocked()) {
        tr.backoff_until = t + kBlockedMs;
        if (!tr.said_blocked) {
            say(std::string("left ") + popup_kind_name(kind) + " alone: " + scene.blockers.front());
            tr.said_blocked = true;
        }
        last_vi_ = std::string("Không đóng ") + popup_kind_vi(kind) + ": " +
                   (why_vi.empty() ? std::string("trong game đang có cửa sổ khác") : why_vi.front()) + ".";
        return Turn::Blocked;
    }

    // What the rest of GoBot is doing, and whether the player is at the
    // machine: put off for half a minute when it is not the moment.
    if (guard_.gates) {
        const std::string why = guard_.gates();
        if (!why.empty()) {
            tr.backoff_until = t + kBackoffMs;
            say(std::string("not closing ") + popup_kind_name(kind) + " now: " + why);
            last_vi_ = std::string("Chưa đóng ") + popup_kind_vi(kind) + ": để sau 30 giây.";
            return Turn::Held;
        }
    }
    // The hold, timed from the moment it is asked for: while it lasts the
    // potion keys wait, so it is let go as soon as the hands are done, and
    // never kept past kHoldMs.
    const std::uint64_t asked = now();
    const auto held_for = [&] { return now() - asked; };
    if (guard_.take && !guard_.take()) {
        tr.backoff_until = t + kBackoffMs;
        say(std::string("not closing ") + popup_kind_name(kind) + " now: not the moment");
        last_vi_ = std::string("Chưa đóng ") + popup_kind_vi(kind) + ": để sau 30 giây.";
        return Turn::Held;
    }
    struct Done {
        const Guard& g;
        bool over = false;
        void let_go() {
            if (!over && g.done) g.done();
            over = true;
        }
        ~Done() { let_go(); }
    } done{ guard_ };

    // Everything once more on a fresh frame, just before the press: the
    // look that decided it is a moment old, and with the hold just taken.
    // The allowed popups are looked for only where they were -- a few
    // milliseconds, where a search of the whole screen takes a tenth of a
    // second of the hold; anything else up is looked for everywhere, as
    // before.
    Frame g;
    if (!grab_ || !grab_(g) || !g.valid()) return Turn::Blind;
    PopupScene again;
    for (const PopupSeen& p : scene.popups)
        for (const PopupSeen& q : eyes_.popups(g, false, &p.at))
            if (same(again, q.kind, q.at) == nullptr) again.popups.push_back(q);
    const PopupSeen* still = same(again, kind, where);
    if (still == nullptr) return Turn::Nothing;
    again.blockers = eyes_.blockers(g, again.popups);
    if (again.blocked()) {
        tr.backoff_until = t + kBlockedMs;
        say(std::string("left ") + popup_kind_name(kind) + " alone: " + again.blockers.front());
        return Turn::Blocked;
    }
    if (kind == PopupKind::MonsterBook && !still->control && again.popups.size() > 1)
        return Turn::Held;

    // A stop that came while the fresh frame was looked at: nothing pressed.
    if (guard_.stopped && guard_.stopped()) return Turn::Held;

    // That look took too long -- a busy machine -- to press now and still let
    // go of the game in time: put off, nothing pressed, no try counted.
    const bool with_esc = !still->control;
    const std::uint64_t took = held_for();
    if (took > (with_esc ? kEscPressByMs : kPressByMs)) {
        tr.backoff_until = now() + kBackoffMs;
        say(std::string("not closing ") + popup_kind_name(kind) + " now: the look before the press took " +
            std::to_string(took) + " ms of the hold");
        last_vi_ = std::string("Chưa đóng ") + popup_kind_vi(kind) + ": để sau 30 giây.";
        return Turn::Held;
    }

    // One click, or one ESC.
    tr.tries.push_back(t);
    const int cx = still->x, cy = still->y;
    const float score = still->score;
    const bool pressed = with_esc ? esc() : click(cx, cy);
    if (with_esc) own_esc_ms_ = now();
    if (!pressed) {
        tr.backoff_until = now() + kBackoffMs;
        say(std::string("could not ") + (with_esc ? "press ESC on " : "click ") + popup_kind_name(kind));
        last_vi_ = std::string("Chưa đóng được ") + popup_kind_vi(kind) + ".";
        return Turn::NotClosed;
    }
    // A click is all the hands do: the game is let go at once, and the rest
    // is looking.
    if (!with_esc) done.let_go();

    // The ESC menu GoBot's own ESC brought up, first seen only once the game
    // was let go -- an ESC late in the hold and a slow look after it: the hold
    // taken again, every gate with it, for that one click on its X, and let go
    // straight after. Left up, it would be taken for the player's from then
    // on, and keep everything else shut for as long as it stayed (PU-R5).
    const auto close_menu_again = [&](const PopupSeen& seen) {
        if (guard_.gates) {
            const std::string why = guard_.gates();
            if (!why.empty()) {
                say("GoBot's ESC brought up the ESC menu, seen only once the game was let go, and it is not the "
                    "moment to take the game again (" + why + "): left as it is");
                return;
            }
        }
        const std::uint64_t asked_again = now();
        if (guard_.take && !guard_.take()) {
            say("GoBot's ESC brought up the ESC menu, seen only once the game was let go, and the game could not "
                "be taken again: left as it is");
            return;
        }
        Done again{ guard_ };
        // Still there, where it was, on a fresh frame: its X is clicked only
        // while it is.
        Frame k;
        if (!grab_ || !grab_(k) || !k.valid()) return;
        PopupScene now_up;
        now_up.popups = eyes_.popups(k, true, &seen.at);
        const PopupSeen* menu = same(now_up, PopupKind::EscMenu, seen.at);
        if (menu == nullptr) {
            say("GoBot's ESC brought up the ESC menu, and it has gone by itself");
            return;
        }
        if (!menu->control || (guard_.stopped && guard_.stopped()) || now() - asked_again > kPressByMs) {
            say("GoBot's ESC brought up the ESC menu, seen only once the game was let go: left as it is");
            return;
        }
        say("GoBot's ESC brought up the ESC menu, seen only once the game was let go: taken again and closed "
            "with its X at " + at_text(menu->x, menu->y));
        click(menu->x, menu->y);
    };

    // Gone within kCheckMs -- and if GoBot's own ESC brought up the ESC menu,
    // that is closed once, by its X: while the hold lasts, or with the hold
    // taken again when it is seen only once it was let go. Each look is for
    // the popup round its own place and the menu anywhere: a few milliseconds
    // where a look for everything takes a tenth of a second or more, which
    // would leave the menu to be seen that much later.
    bool gone = false, menu_tried = false;
    Frame after;
    for (int waited = 0; waited < kCheckMs; waited += kCheckLookMs) {
        // No look started under the hold that could run past kHoldMs.
        if (!done.over && held_for() + kCheckLookMs + kLookAllowMs > kHoldMs) done.let_go();
        sleep(kCheckLookMs);
        Frame h;
        if (!grab_ || !grab_(h) || !h.valid()) continue;
        after = h;
        PopupScene now_up;
        now_up.popups = eyes_.popups(h, true, &where);
        gone = same(now_up, kind, where) == nullptr;
        const PopupSeen* menu = now_up.find(PopupKind::EscMenu);
        // One ESC does one thing: it shuts the book, or it brings up the
        // menu. A menu seen once nothing of the book is left -- not even its
        // title, which a menu GoBot's ESC brought up over the book leaves
        // showing -- was under it: the player's, opened before the book, and
        // never GoBot's to close.
        if (with_esc && gone && menu != nullptr && !menu_tried && !eyes_.book_title_in(h, where)) {
            say("the ESC menu was already open under " + std::string(popup_kind_name(kind)) +
                "; left as it is");
            break;
        }
        if (with_esc && menu != nullptr && !menu_tried && now() - own_esc_ms_ <= kOwnEscMs) {
            menu_tried = true;
            if (done.over || held_for() > kHoldMs) {
                done.let_go();
                close_menu_again(*menu);
            } else if (guard_.stopped && guard_.stopped()) {
                say("GoBot's ESC brought up the ESC menu, and a stop came: left as it is");
            } else if (menu->control) {
                say("GoBot's ESC brought up the ESC menu: closing it with its X at " +
                    at_text(menu->x, menu->y));
                click(menu->x, menu->y);
            } else {
                say("GoBot's ESC brought up the ESC menu, and its X is not where it should be: "
                    "left as it is");
            }
            // The menu dealt with, the ESC is done with the game.
            done.let_go();
            continue;
        }
        // The book gone and no menu: the ESC did its one thing.
        if (gone && menu == nullptr) break;
    }
    done.let_go();

    const std::string how = with_esc ? std::string("with ESC")
                          : kind == PopupKind::CheckinNotice ? std::string("with its Xác Nhận")
                                                             : std::string("with its X");
    if (gone) {
        ++closed_;
        tr.seen = false;
        say(std::string("closed ") + popup_kind_name(kind) + " at " + at_text(where.x, where.y) +
            " (" + (kind == PopupKind::CheckinNotice ? "words " : "picture ") + score_text(score) +
            ") " + how);
        // Whatever came up in its place -- a window the notice's Xác Nhận
        // opened, say -- is said, and left as it is.
        if (after.valid()) {
            const std::vector<std::string> other = eyes_.blockers(after, {});
            if (!other.empty()) say("after that, " + other.front() + "; left as it is");
        }
        last_vi_ = std::string("Đã đóng ") + popup_kind_vi(kind) + ".";
        return Turn::Closed;
    }
    tr.backoff_until = now() + kBackoffMs;
    say(std::string(popup_kind_name(kind)) + " is still up after closing it " + how +
        (static_cast<int>(tr.tries.size()) < kTries ? "; trying again in 30 s" : "; leaving it alone"));
    last_vi_ = std::string("Đã thử đóng ") + popup_kind_vi(kind) + " mà vẫn còn.";
    return Turn::NotClosed;
}

} // namespace gobot
