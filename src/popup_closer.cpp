#include "gobot/popup_closer.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>
#include <optional>
#include <thread>
#include <utility>

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

/// "+564,+14": how far one point is from another.
std::string offset_text(int dx, int dy) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+d,%+d", dx, dy);
    return buf;
}

std::string score_text(float s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(s));
    return buf;
}

/// The check-in board's title where a board at `board` (PopupSeen::at) has
/// it, the size of `title`; and the board's top band, where its X is.
ScreenRect board_title_at(const ScreenRect& board, const Frame& title) {
    return ScreenRect{ board.x - kCheckinBoardFromTitle.x, board.y - kCheckinBoardFromTitle.y, title.width,
                       title.height };
}
ScreenRect board_band(const ScreenRect& board) {
    const int tx = board.x - kCheckinBoardFromTitle.x, ty = board.y - kCheckinBoardFromTitle.y;
    return ScreenRect{ tx + kCheckinBandFromTitle.x, ty + kCheckinBandFromTitle.y, kCheckinBandFromTitle.w,
                       kCheckinBandFromTitle.h };
}

/// Closed with ESC rather than by its control: the board always, the book
/// when its X was not found. ESC goes to whatever is in front, so only when
/// nothing else of the list is up.
bool closes_with_esc(const PopupSeen& p) {
    return p.by_esc || (p.kind == PopupKind::MonsterBook && !p.control);
}

/// What a popup is looked for again as, round its own place under the hold
/// and after its press: the board as itself alone -- its title round where
/// it was, a 296x46 search -- and the rest as before, every allowed kind but
/// the board.
unsigned relook_kinds(PopupKind kind, unsigned allowed) {
    const unsigned board = popup_bit(PopupKind::CheckinBoard);
    return kind == PopupKind::CheckinBoard ? board : allowed & ~board;
}

/// How blockers() runs its searches: side by side on a PC with cores to
/// spare, one after the other on this thread on one with 2 or fewer (or none
/// said) -- there a thread each only adds their stacks and the switching
/// between them to a machine the game needs, for no time saved (2026-11-16).
/// A deferred future runs at its get(), so the results are the same.
std::launch blockers_launch() {
    static const unsigned cpus = std::thread::hardware_concurrency();
    return cpus <= 2 ? std::launch::deferred : std::launch::async;
}

} // namespace

const char* popup_kind_name(PopupKind k) {
    switch (k) {
        case PopupKind::CheckinNotice: return "the check-in notice";
        case PopupKind::MonsterBook:   return "Yêu Quái Bách Khoa";
        case PopupKind::EscMenu:       return "the ESC menu";
        case PopupKind::CheckinBoard:  return "the check-in board";
    }
    return "?";
}

const char* popup_kind_vi(PopupKind k) {
    switch (k) {
        case PopupKind::CheckinNotice: return "thông báo nhận quà điểm danh";
        case PopupKind::MonsterBook:   return "Yêu Quái Bách Khoa";
        case PopupKind::EscMenu:       return "menu ESC do GoBot mở";
        case PopupKind::CheckinBoard:  return "bảng Sự kiện và điểm danh";
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

void PopupEyes::load(std::shared_ptr<const TemplateStore> store, const std::string& configs_dir) {
    static const TemplateStore none;
    load(store ? *store : none, configs_dir);
    held_ = std::move(store);
}

void PopupEyes::load(const TemplateStore& store, const std::string& configs_dir) {
    held_.reset();
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

bool PopupEyes::still_partly_in(PopupKind kind, const Frame& f, const ScreenRect& where) const {
    switch (kind) {
        case PopupKind::MonsterBook:
            return book_title_in(f, where);
        case PopupKind::CheckinBoard:
            // Its title where it was: the ESC menu GoBot's ESC brings up is
            // drawn in the middle, over the board's calendar, not its title.
            // Some of it -- the game's lines of news over its title -- counts.
            return !popups(f, false, &where, popup_bit(PopupKind::CheckinBoard)).empty() ||
                   board_covered_at(f, where);
        case PopupKind::CheckinNotice:
        case PopupKind::EscMenu:
            break;
    }
    return false;
}

bool PopupEyes::sees_board() const {
    const Frame* title = store_ != nullptr ? checkin_board_picture(*store_) : nullptr;
    return title != nullptr && title->valid();
}

bool PopupEyes::board_covered_at(const Frame& f, const ScreenRect& board, PopupSeen* out) const {
    const Frame* pic = store_ != nullptr ? checkin_board_picture(*store_) : nullptr;
    if (pic == nullptr || !pic->valid() || !f.valid()) return false;
    const ScreenRect was = board_title_at(board, *pic);
    ScreenRect title;
    float score = 0.0f;
    if (!checkin_board_title(f, *pic, &title, &score, &was, kCheckinBoardCoveredScore)) return false;
    // Where that board has it, give or take as much as one still counts as
    // there: a title 8 px off at the lower bar is no sign of this board.
    const ScreenRect at{ title.x + kCheckinBoardFromTitle.x, title.y + kCheckinBoardFromTitle.y,
                         kCheckinBoardFromTitle.w, kCheckinBoardFromTitle.h };
    if (!same_place(at, board)) return false;
    if (out != nullptr) {
        *out = PopupSeen{};
        out->kind = PopupKind::CheckinBoard;
        out->at = at;
        out->score = score;
        out->by_esc = true;
        out->covered = score < kCheckinBoardScore;
    }
    return true;
}

std::vector<PopupSeen> PopupEyes::popups(const Frame& f, bool with_menu, const ScreenRect* only_at,
                                         unsigned kinds, bool goc_book) const {
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
    // A look of the whole screen searches it for up to four pictures: turned
    // into grey once for all of them rather than once each -- on a one-core
    // PC that is time the game gets back. Same answers to the bit. Not for a
    // look round one place, where each search cuts only a small part.
    std::optional<TemplateMatcher::OneFrame> same;
    if (only_at == nullptr) same.emplace(f);

    // The check-in notice: a Xác Nhận with the notice's words where they sit
    // in the notice, give or take two pixels. The words are what tells it:
    // every dialog's frame and buttons are the same. Measured from a lone
    // Xác Nhận's place, they are nowhere near where a two-button dialog has
    // its words.
    if ((kinds & popup_bit(PopupKind::CheckinNotice)) != 0 && ok_.valid() && notice_words_.valid() &&
        look.w >= ok_.width && look.h >= ok_.height) {
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

    // The check-in board, by its title: anywhere, or -- round `only_at`, a
    // board's own place -- only where that board has its title, a few pixels
    // either way. Its X is looked for in its top band, where the art has it
    // at +555,5 from the title, only on a look of the whole screen: found or
    // not, the board is closed with ESC, and any X in that band is its own.
    const Frame* board_title = store_ != nullptr ? checkin_board_picture(*store_) : nullptr;
    if ((kinds & popup_bit(PopupKind::CheckinBoard)) != 0 && board_title != nullptr && board_title->valid()) {
        const ScreenRect was = only_at == nullptr ? ScreenRect{} : board_title_at(*only_at, *board_title);
        ScreenRect title;
        float score = 0.0f;
        if (checkin_board_title(f, *board_title, &title, &score, only_at == nullptr ? nullptr : &was)) {
            PopupSeen p;
            p.kind = PopupKind::CheckinBoard;
            p.at = ScreenRect{ title.x + kCheckinBoardFromTitle.x, title.y + kCheckinBoardFromTitle.y,
                               kCheckinBoardFromTitle.w, kCheckinBoardFromTitle.h };
            p.score = score;
            p.by_esc = true;
            if (only_at == nullptr) p.control = close_button_in(f, board_band(p.at), p.x, p.y);
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
    const bool want_book = (kinds & popup_bit(PopupKind::MonsterBook)) != 0;
    const bag::Book live = want_book && store_ != nullptr ? bag::find_book(f, *store_, only_at) : bag::Book{};
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
    // top-right corner of where the whole picture sits. Without `goc_book`
    // only when the live book's tab was found without its X: some book is
    // up, and GoC's picture may be the one that finds its X.
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
    if (!book_seen && want_book && (goc_book || live.found)) {
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
    if (with_menu && (kinds & popup_bit(PopupKind::EscMenu)) != 0 && menu_.valid()) {
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
    // Run here one after another (a PC with two cores or fewer), the searches
    // share one grey copy of the frame. Run on threads of their own, they do
    // not see it and convert their part each, as before; this one then costs
    // nothing, as nothing on this thread asks it for the frame.
    TemplateMatcher::OneFrame same(f);

    // Side by side: each reads only the frame and the pictures, and a look
    // that takes the time of the slowest rather than of all of them is a
    // frame that much younger when the click goes -- on a PC with the cores
    // for it; on one with 2 or fewer, one after the other (blockers_launch).
    const std::launch how = blockers_launch();
    auto two_buttons = std::async(how, [&] { return !bag::find_dialogs(f, store).empty(); });
    auto invited = std::async(how, [&] {
        PartyInvite asking;
        asking.set_settings(invite_);
        return asking.look(f, store, nullptr).up;
    });
    auto dead = std::async(how, [&] { return reviver_ && reviver_->look(f).dead(); });
    auto quest = std::async(how, [&] {
        return quest_.ready() && find_quest_window(f, quest_, quest_settings_).found;
    });
    auto bag_up = std::async(how, [&] { return bag::find(f, store, bag_).found; });
    auto chat_open = std::async(how, [&] {
        return look_chat_bar(f, store, chat_).state == ChatBarState::Open;
    });
    // Each drawing of a close button on its own: they are matched through
    // their see-through corners, the dearest searches here by far -- a third
    // of a second together, a tenth side by side -- and the look under the
    // hold keeps the potion keys waiting for as long as it takes.
    std::vector<std::future<std::vector<TemplateMatcher::Match>>> xs;
    for (const Frame& x : close_x_)
        if (x.valid())
            xs.push_back(std::async(how, [&f, &all, &x] {
                return TemplateMatcher::find_all(f, all, x, kCloseScore, 16);
            }));
    auto oks = std::async(how, [&] {
        return ok_.valid() ? TemplateMatcher::find_all(f, all, ok_, kOkScore, 16)
                           : std::vector<TemplateMatcher::Match>{};
    });
    // The ESC menu by its own picture, not only by its X: an X drawn lit
    // under the pointer is not the X's picture, and the player's menu may be
    // on its way to Thoát Game. Not when it is among `popups` -- the one
    // GoBot's own ESC brought up, which its check closes.
    auto menu_up = std::async(how, [&] {
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
    // nothing here knows -- Kho Cash, a shop, the player list. The check-in
    // board's is any in its top band: the live board's X has never been seen,
    // and one drawn a little off the art's place would otherwise keep the
    // board itself shut.
    std::vector<TemplateMatcher::Match> found_x;
    for (auto& these : xs) {
        const std::vector<TemplateMatcher::Match> got = these.get();
        found_x.insert(found_x.end(), got.begin(), got.end());
    }
    for (const auto& x : found_x) {
        const bool known = std::any_of(popups.begin(), popups.end(), [&](const PopupSeen& p) {
            if (p.kind == PopupKind::CheckinBoard) return board_band(p.at).contains(x.at.cx(), x.at.cy());
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

int PopupCloser::local_second() const {
    if (clock_.local_second) return clock_.local_second();
    // A pretend clock has no time of day: the PC's would put a test run at
    // 23:58 into the night hold.
    if (clock_.now_ms) return -1;
    return local_second_of_day();
}

bool PopupCloser::board_due(std::uint64_t at, std::uint64_t since, std::uint64_t arrived, int second) {
    // Still for kSteadyMs, as every popup.
    if (at < since || at - since < kSteadyMs) return false;
    // 30 s from the later of the arrival and the first sight: a board seen
    // only a while after the arrival -- covered, the HUD late -- is still
    // given its 30 s to be read, and one that came up long after an arrival
    // -- at 00:00 -- is waited for from its own sight.
    const std::uint64_t from = std::max(arrived, since);
    if (at < from || at - from < kCheckinBoardLoginWaitMs) return false;
    return !checkin_night_hold(second);
}

int PopupCloser::next_look_ms(Turn last, bool light) {
    if (last != Turn::Nothing) return kLookMs;
    return light ? kIdleLookLightMs : kIdleLookMs;
}

void PopupCloser::say(const std::string& line) {
    if (report_) report_("Popups: " + line);
}

void PopupCloser::say_board(Track& tr, unsigned why, const std::string& line) {
    if ((tr.said_why & why) != 0) return;
    tr.said_why |= why;
    say(line);
}

bool PopupCloser::click(int x, int y) {
    if (hands_.click) return hands_.click(x, y);
    // The point has to be on the game, and the game in front, as the button
    // goes down: a click goes to whatever is under the pointer. Nothing is
    // brought forward for this. A stop in the pointer's pause clicks nothing.
    return input_.click_on_game(x, y, false, guard_.stopped);
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
    last_kind_ = -1;
    // Grabbed into the same buffer turn after turn (turn_frame_).
    Frame& f = turn_frame_;
    if (!grab_ || !grab_(f) || !f.valid()) {
        last_vi_ = "Không chụp được màn hình game.";
        return Turn::Blind;
    }

    // The ESC menu is closed only straight after GoBot's own ESC, as part of
    // that ESC's check below. Otherwise it is the player's -- they may be on
    // their way to Thoát Game or Cửa Sổ Chọn Kênh -- and its X is one more
    // window nothing here may touch, which keeps everything else shut too.
    // Only the kinds allowed now are closed, and only theirs own anything on
    // the screen; the check-in board is looked for all the same, and seen
    // while it is not allowed it keeps everything shut (B-2).
    //
    // GoC's picture of the book's first page on every look while anything is
    // up, and otherwise on every kGocBookEvery-th look in a row that saw
    // nothing (2026-11-16). A popup is closed only on a turn after the one
    // that first saw it, so the look a press is decided on always had the
    // book looked for: an ESC never goes to a book left unseen. A board seen
    // again after a look that missed it is not closed on that turn either
    // (Track::back_at).
    const unsigned allowed = allowed_;
    const bool goc_book = idle_looks_ % kGocBookEvery == 0;
    PopupScene scene;
    bool board_barred = false;
    std::vector<PopupSeen> seen =
        eyes_.popups(f, false, nullptr, allowed | popup_bit(PopupKind::CheckinBoard), goc_book);
    // The board not found whole, where it was seen whole a moment ago: its
    // title looked for there at the lower bar. The game draws its lines of
    // news at the top of the screen over the board's title (2026-11-17), and
    // a look in which they cover it is no sign the board went. Never a first
    // sight, and not after a new arrival in the world (kept_board).
    const int board_k = static_cast<int>(PopupKind::CheckinBoard);
    const std::uint64_t arrived = clock_.arrived_ms ? clock_.arrived_ms() : 0;
    // Timed from the first look that missed it, not from its last sight: a
    // while with no look at all -- the game behind another window -- is no
    // time out of sight.
    const auto kept_board = [&](const Track& tr) {
        return tr.placed && arrived <= tr.last_seen &&
               (tr.seen || (t >= tr.missed_at && t - tr.missed_at <= kBoardGapMs));
    };
    {
        const Track& tr = tracks_[board_k];
        PopupSeen covered;
        if (std::none_of(seen.begin(), seen.end(),
                         [](const PopupSeen& p) { return p.kind == PopupKind::CheckinBoard; }) &&
            kept_board(tr) && t - tr.whole_seen <= kBoardGraceMs && eyes_.board_covered_at(f, tr.at, &covered))
            seen.push_back(covered);
    }
    for (const PopupSeen& p : seen) {
        if ((allowed & popup_bit(p.kind)) != 0) scene.popups.push_back(p);
        else if (p.kind == PopupKind::CheckinBoard) board_barred = true;
    }
    idle_looks_ = seen.empty() ? (idle_looks_ + 1) % kGocBookEvery : 0;

    // Where each kind has been, and since when. The board said as it is first
    // seen at a place, its X with it: the live board has never been seen.
    //
    // The board's sighting outlives a look that misses it (kBoardGapMs):
    // seen again where it was, it keeps its first sight and what was said
    // about it -- its 30 s are not started over by a moment out of sight.
    for (int k = 0; k < kPopupKinds; ++k) {
        Track& tr = tracks_[k];
        const PopupSeen* p = scene.find(static_cast<PopupKind>(k));
        const bool board = k == board_k;
        if (p == nullptr) {
            if (board && tr.seen) tr.missed_at = t;
            tr.seen = false;
            if (!board || !kept_board(tr)) {
                tr.said_blocked = false;
                tr.placed = false;
            }
            continue;
        }
        if (board && !tr.seen && kept_board(tr) && same_place(tr.at, p->at)) {
            // Seen again, the same board: said once a sighting, with how long
            // it was out of sight, and that its wait goes on from its first.
            say_board(tr, kWhyGap,
                      "the check-in board is up again at " + at_text(p->at.x, p->at.y) + " after " +
                          std::to_string(t - tr.last_seen) + " ms out of sight (" +
                          (p->covered ? "its title partly covered, " + score_text(p->score)
                                      : "title " + score_text(p->score)) +
                          "): the same board -- its wait still counts from its first sight, " +
                          std::to_string((t - tr.since) / 1000) + " s ago");
            tr.seen = true;
            tr.back_at = t;
        }
        if (!tr.seen || !same_place(tr.at, p->at)) {
            tr.seen = true;
            tr.at = p->at;
            tr.since = t;
            tr.tries = 0;
            tr.said_blocked = false;
            tr.said_wait = 0;
            tr.said_why = 0;
            tr.said_gate.clear();
            tr.relook_misses = 0;
            tr.back_at = 0;
            if (p->kind == PopupKind::CheckinBoard) {
                const int tx = p->at.x - kCheckinBoardFromTitle.x, ty = p->at.y - kCheckinBoardFromTitle.y;
                say("the check-in board (Sự kiện và điểm danh) is up at " + at_text(p->at.x, p->at.y) + " (title " +
                    score_text(p->score) + " at " + at_text(tx, ty) + "; " +
                    (p->control ? "its X at " + offset_text(p->x - tx, p->y - ty) + " from the title"
                                : std::string("no X seen")) +
                    ")");
            }
        }
        if (board) {
            tr.placed = true;
            tr.last_seen = t;
            if (!p->covered) tr.whole_seen = t;
            else
                say_board(tr, kWhyCovered,
                          "the check-in board's title at " + at_text(p->at.x, p->at.y) + " is partly covered (" +
                              score_text(p->score) + ", the game's lines of news over it?): still taken as up");
        }
    }
    if (scene.popups.empty()) {
        last_vi_ = "Không có thông báo nào cần đóng.";
        return Turn::Nothing;
    }

    // The one to close: up at one place for kSteadyMs, not put off. The
    // board, and the monster book without its X, only when nothing else of
    // the list is up: an ESC would go to that first -- the board's notice is
    // closed before it. A popup tried and still up is put off kBackoffMs and
    // tried again, as often as it takes (2026.11.24, the owner: "Kiểm tra các
    // phần khác xem có phần nào không làm được thì dừng không, cần thay cơ chế
    // này bằng 1 cơ chế khác"); until then, after two tries in ten minutes it
    // was left alone for the rest of them (Turn::LeftAlone, gone).
    //
    // The check-in board waits longer (the owner, 2026-11-15: "Sau khi chọn
    // nhân vật xong và vào tới màn hình game thì sẽ chờ 30 giây rồi mới ấn ESC
    // để tắt thông báo. Trường hợp qua ngày thì chờ tới 00:03 mới ấn ESC"):
    // board_due, here and not in the gates -- a gate would hold the notice and
    // the book too, and put the board off a whole kBackoffMs. Its wait counts
    // no try and sets no backoff: the ESC goes at the first turn it is due.
    // `board_wait` says which wait it is in: 1 the 30 s, 2 the night hold.
    const PopupSeen* pick = nullptr;
    bool waiting = false, other_waiting = false;
    int board_wait = 0;
    for (const PopupSeen& p : scene.popups) {
        Track& tr = tracks_[static_cast<int>(p.kind)];
        if (t < tr.backoff_until) continue;
        if (p.kind == PopupKind::CheckinBoard) {
            const int second = local_second();
            if (!board_due(t, tr.since, arrived, second)) {
                waiting = true;
                board_wait = checkin_night_hold(second) ? 2 : 1;
                // Said once a sighting, and again when the wait changes --
                // the night hold over, the 30 s still to run.
                if (tr.said_wait != board_wait) {
                    tr.said_wait = board_wait;
                    if (board_wait == 2) {
                        char hms[16];
                        std::snprintf(hms, sizeof hms, "%02d:%02d:%02d", second / 3600, second / 60 % 60,
                                      second % 60);
                        say(std::string("the check-in board waits for its ESC until 00:03:00: the PC's clock reads ") +
                            hms + ", in the night hold from 23:55:00 (the game opens the board again as the day "
                                    "turns)");
                    } else {
                        const std::uint64_t from = std::max(arrived, tr.since);
                        const std::uint64_t due =
                            std::max(tr.since + kSteadyMs, from + kCheckinBoardLoginWaitMs);
                        say(std::string("the check-in board waits for its ESC until ") +
                            std::to_string(kCheckinBoardLoginWaitMs / 1000) + " s after " +
                            (arrived > tr.since ? "the character's arrival in the world"
                                                : "it was first seen (the arrival, or the board at 00:00)") +
                            ": in " + std::to_string(due > t ? (due - t + 999) / 1000 : 0) + " s");
                    }
                }
                continue;
            }
            // Due, but seen again only now after a look that missed it: this
            // turn's look most often skipped GoC's picture of the monster book
            // (idle_looks_), and a book opened while the board was out of
            // sight would take the ESC. Its kSteadyMs back first, as any popup
            // newly up -- the looks meanwhile have the book looked for.
            if (tr.back_at != 0 && t - tr.back_at < kSteadyMs) {
                waiting = true;
                continue;
            }
        } else if (t - tr.since < kSteadyMs) {
            waiting = true;
            other_waiting = true;
            continue;
        }
        if (closes_with_esc(p) && scene.popups.size() > 1) {
            // The board due, and an ESC would go to the other first: said
            // once a sighting -- on 2026-11-17 a board due got no ESC and
            // nothing in the log said why.
            if (p.kind == PopupKind::CheckinBoard) {
                std::string others;
                for (const PopupSeen& o : scene.popups)
                    if (o.kind != p.kind) others += (others.empty() ? "" : ", ") + std::string(popup_kind_name(o.kind));
                say_board(tr, kWhyOther,
                          "the check-in board is due its ESC, but " + others +
                              " is up too and an ESC would go to it first: no ESC while it is");
            }
            continue;
        }
        pick = &p;
        break;
    }
    if (pick == nullptr) {
        // The board's own wait, said on the tab, when nothing else is waiting
        // its 3 s. A board put off after an ESC that left it up is no turn
        // about it (last_kind_ stays -1), so the worker's tab keeps that try's
        // own line, "... thử lại sau 30 giây.", until the next try.
        const bool board_waits = waiting && board_wait != 0 && !other_waiting;
        if (board_waits) last_kind_ = static_cast<int>(PopupKind::CheckinBoard);
        last_vi_ = board_waits ? (board_wait == 2
                                      ? "Qua ngày: chờ tới 00:03 rồi mới bấm ESC tắt bảng Sự kiện và điểm danh."
                                      : "Thấy bảng Sự kiện và điểm danh: chờ 30 giây sau khi vào game rồi mới bấm ESC.")
                 : waiting ? "Thấy thông báo, chờ 3 giây rồi mới đóng."
                           : "Có thông báo, chờ lượt sau.";
        return waiting ? Turn::Waiting : Turn::Held;
    }
    const PopupKind kind = pick->kind;
    const ScreenRect where = pick->at;
    Track& tr = tracks_[static_cast<int>(kind)];
    last_kind_ = static_cast<int>(kind);
    const bool board = kind == PopupKind::CheckinBoard;

    // Nothing else may be up.
    std::vector<std::string> why_vi;
    scene.blockers = eyes_.blockers(f, scene.popups, &why_vi);
    if (board_barred) {
        scene.blockers.insert(scene.blockers.begin(), "the check-in board is up, and it is not to be closed now");
        why_vi.insert(why_vi.begin(), "đang có bảng Sự kiện và điểm danh");
    }
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
    // machine: put off for half a minute when it is not the moment. The
    // board, due already, only kBlockedMs: its ESC is to go on the first turn
    // that may press -- a trash hold over a second after its 30 s would put
    // it off half a minute more -- and the gates ask nothing of the game.
    // Each reason said once a sighting for it, not every 5 s.
    if (guard_.gates) {
        const std::string why = guard_.gates();
        if (!why.empty()) {
            const std::uint64_t off = board ? kBlockedMs : kBackoffMs;
            tr.backoff_until = t + off;
            if (!board || tr.said_gate != why) {
                say(std::string("not closing ") + popup_kind_name(kind) + " now: " + why);
                if (board) tr.said_gate = why;
            }
            last_vi_ = std::string("Chưa đóng ") + popup_kind_vi(kind) + ": để sau " + std::to_string(off / 1000) +
                       " giây.";
            return Turn::Held;
        }
    }
    // The hold, timed from the moment it is asked for: while it lasts the
    // potion keys wait, so it is let go as soon as the hands are done, and
    // never kept past kHoldMs.
    const std::uint64_t asked = now();
    const auto held_for = [&] { return now() - asked; };
    // Refused here for as short-lived reasons as a gate -- the party job's
    // click holding the mouse a second or two, a key still under way, a
    // touch of the mouse, a job taking the hold since the gates were asked:
    // the due board put off kBlockedMs too, and said once a sighting.
    if (guard_.take && !guard_.take()) {
        const std::uint64_t off = board ? kBlockedMs : kBackoffMs;
        tr.backoff_until = t + off;
        if (board) say_board(tr, kWhyTake, std::string("not closing ") + popup_kind_name(kind) + " now: not the moment");
        else say(std::string("not closing ") + popup_kind_name(kind) + " now: not the moment");
        last_vi_ = std::string("Chưa đóng ") + popup_kind_vi(kind) + ": để sau " + std::to_string(off / 1000) +
                   " giây.";
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
    // second of the hold; the check-in board only as its title, round where
    // it was (B-5); anything else up is looked for everywhere, as before.
    const std::uint64_t relook_from = now();
    const auto said_slow = [&] {
        const std::uint64_t took = now() - relook_from;
        if (took > kSlowRelookMs) say("the re-look under the hold took " + std::to_string(took) + " ms");
    };
    Frame g;
    if (!grab_ || !grab_(g) || !g.valid()) {
        if (board)
            say_board(tr, kWhyBlind, "the check-in board is due its ESC, but no picture of the game came under the "
                                     "hold: nothing pressed; tried again on the next turn");
        return Turn::Blind;
    }
    PopupScene again;
    for (const PopupSeen& p : scene.popups)
        for (const PopupSeen& q : eyes_.popups(g, false, &p.at, relook_kinds(p.kind, allowed)))
            if (same(again, q.kind, q.at) == nullptr) again.popups.push_back(q);
    // The board not found whole round its place: as the turn's own look, its
    // title at the lower bar there -- the game's lines of news may have come
    // over it between the two looks.
    PopupSeen covered;
    if (board && same(again, kind, where) == nullptr && eyes_.board_covered_at(g, where, &covered))
        again.popups.push_back(covered);
    const PopupSeen* still = same(again, kind, where);
    if (still == nullptr) {
        said_slow();
        // Not silently, and not given up: until 2026.11.17c this said nothing
        // and the board was looked at again only 2 s later, from scratch. The
        // next turn tries again; a second miss in a row puts it off
        // kBlockedMs.
        if (board) {
            if (++tr.relook_misses >= 2) tr.backoff_until = t + kBlockedMs;
            say_board(tr, kWhyRelook,
                      "the check-in board is due its ESC, but the look under the hold did not find it again round " +
                          at_text(where.x, where.y) + ", whole or covered: nothing pressed; looked for again on the "
                                                      "next turn");
            last_vi_ = "Chưa bấm ESC tắt bảng Sự kiện và điểm danh: nhìn lại lượt sau.";
            return Turn::Held;
        }
        return Turn::Nothing;
    }
    tr.relook_misses = 0;
    again.blockers = eyes_.blockers(g, again.popups);
    said_slow();
    if (again.blocked()) {
        tr.backoff_until = t + kBlockedMs;
        say(std::string("left ") + popup_kind_name(kind) + " alone: " + again.blockers.front());
        return Turn::Blocked;
    }
    if (closes_with_esc(*still) && again.popups.size() > 1) {
        if (board) {
            std::string others;
            for (const PopupSeen& o : again.popups)
                if (o.kind != kind) others += (others.empty() ? "" : ", ") + std::string(popup_kind_name(o.kind));
            say_board(tr, kWhyOther,
                      "the check-in board is due its ESC, but " + others +
                          " came up beside it under the hold and an ESC would go to it first: no ESC while it is");
        }
        return Turn::Held;
    }

    // A stop that came while the fresh frame was looked at: nothing pressed.
    if (guard_.stopped && guard_.stopped()) return Turn::Held;

    // That look took too long -- a busy machine -- to press now and still let
    // go of the game in time: put off, nothing pressed, no try counted.
    const bool with_esc = closes_with_esc(*still);
    const std::uint64_t took = held_for();
    if (took > (with_esc ? kEscPressByMs : kPressByMs)) {
        tr.backoff_until = now() + kBackoffMs;
        say(std::string("not closing ") + popup_kind_name(kind) + " now: the look before the press took " +
            std::to_string(took) + " ms of the hold");
        last_vi_ = std::string("Chưa đóng ") + popup_kind_vi(kind) + ": để sau 30 giây.";
        return Turn::Held;
    }

    // One click, or one ESC.
    ++tr.tries;
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

    // Gone within kCheckMs of the press, by the clock -- the owner's 30 s for
    // slow PCs (2026-11-15), the first look that finds it gone ending it at
    // once -- and if GoBot's own ESC brought up the ESC menu, that is closed
    // once, by its X: while the hold lasts, or with the hold taken again when
    // it is seen only once it was let go. The hold itself is let go within
    // kHoldMs as before: the potion keys never wait on a slow game, only this
    // worker does. Each look asks the stop first, and ends the check on one.
    // Each look is for the popup round its own place and the menu anywhere: a
    // few milliseconds where a look for everything takes a tenth of a second
    // or more, which would leave the menu to be seen that much later.
    bool gone = false, menu_tried = false, stopped = false;
    // Whether the last look had the ESC menu on it: a popup not found under
    // GoBot's own menu is covered, not gone.
    bool menu_up = false;
    // Each look grabbed into `h` and swapped into turn_frame_ -- the last
    // look (last_look) -- so the two buffers go back and forth with no copy.
    // The turn's own look is not needed again once the press is made.
    Frame h;
    bool looked_after = false;
    const std::uint64_t pressed_ms = now();
    std::uint64_t check_until = pressed_ms + static_cast<std::uint64_t>(kCheckMs);
    while (now() < check_until) {
        // No look started under the hold that could run past kHoldMs.
        if (!done.over && held_for() + kCheckLookMs + kLookAllowMs > kHoldMs) done.let_go();
        sleep(kCheckLookMs);
        if (guard_.stopped && guard_.stopped()) {
            stopped = true;
            break;
        }
        if (!grab_ || !grab_(h) || !h.valid()) continue;
        std::swap(turn_frame_, h);
        looked_after = true;
        const Frame& look = turn_frame_;
        PopupScene now_up;
        now_up.popups =
            eyes_.popups(look, true, &where, relook_kinds(kind, allowed) | popup_bit(PopupKind::EscMenu));
        // The board with the game's lines of news over its title is still up,
        // not gone: taken for gone, it was counted closed and its next ESC
        // waited another 30 s.
        gone = same(now_up, kind, where) == nullptr && !(board && eyes_.board_covered_at(look, where));
        const PopupSeen* menu = now_up.find(PopupKind::EscMenu);
        menu_up = menu != nullptr;
        // One ESC does one thing: it shuts the book or the board, or it
        // brings up the menu. A menu seen once nothing of the popup is left --
        // not even its title, which a menu GoBot's ESC brought up over it
        // leaves showing -- was under it: the player's, opened before the
        // popup, and never GoBot's to close.
        if (with_esc && gone && menu != nullptr && !menu_tried && !eyes_.still_partly_in(kind, look, where)) {
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
            // The menu dealt with, the ESC is done with the game -- and it went
            // to the menu, not to the popup: that is watched only kAfterMenuMs
            // more, not the rest of the 30 s.
            done.let_go();
            check_until = std::min(check_until, now() + static_cast<std::uint64_t>(kAfterMenuMs));
            continue;
        }
        // The popup gone and no menu: the ESC did its one thing -- a click
        // brings up no menu, so after one a menu changes nothing. Never gone
        // while the menu GoBot's ESC brought up is still on the look, its X
        // clicked or not: that ESC went to the menu, and the popup is only
        // covered by it -- on a slow PC the menu can be drawn a while after
        // its X is clicked, and a look then is no sign the popup went
        // (review 2026-11-15). Looked at again until the menu goes, as the
        // 1.5 s check did, within kAfterMenuMs.
        if (gone && (menu == nullptr || !with_esc)) break;
    }
    // The watch over with the menu GoBot's ESC brought up still over where
    // the popup was: it is covered, not closed -- not counted, not said closed
    // in the log or the tab, and tried again after kBackoffMs as any popup
    // still up.
    if (gone && menu_tried && menu_up) {
        say("the ESC menu GoBot's ESC brought up still covers " + std::string(popup_kind_name(kind)) +
            ": not taken for closed");
        gone = false;
    }
    done.let_go();
    // A stop while it was looked at: nothing more, and nothing judged.
    if (stopped && !gone) {
        tr.backoff_until = now() + kBackoffMs;
        say(std::string("a stop came while ") + popup_kind_name(kind) +
            " was watched for after its press; left as it is");
        last_vi_ = std::string("Đã dừng trong lúc chờ ") + popup_kind_vi(kind) + " tắt.";
        return Turn::Held;
    }
    const std::uint64_t took_ms = now() - pressed_ms;
    if (gone && took_ms > static_cast<std::uint64_t>(kReactSlowMs))
        say("the game took " + std::to_string(took_ms) + " ms to show " + popup_kind_name(kind) + " gone");
    if (!gone && !menu_tried)
        say("nothing after " + std::to_string(kCheckMs / 1000) + " s: " + popup_kind_name(kind) + " still up");

    const std::string how = with_esc ? std::string("with ESC")
                          : kind == PopupKind::CheckinNotice ? std::string("with its Xác Nhận")
                                                             : std::string("with its X");
    if (gone) {
        ++closed_;
        tr.seen = false;
        // Closed: a board seen after this is a new one, with a wait of its own.
        tr.placed = false;
        say(std::string("closed ") + popup_kind_name(kind) + " at " + at_text(where.x, where.y) + " (" +
            (kind == PopupKind::CheckinNotice ? "words " : board ? "title " : "picture ") + score_text(score) +
            ") " + how);
        // Whatever came up in its place -- a window the notice's Xác Nhận
        // opened, say -- is said, and left as it is.
        if (looked_after) {
            const std::vector<std::string> other = eyes_.blockers(turn_frame_, {});
            if (!other.empty()) say("after that, " + other.front() + "; left as it is");
        }
        last_vi_ = board ? std::string("Đã tắt bảng Sự kiện và điểm danh (bấm ESC).")
                         : std::string("Đã đóng ") + popup_kind_vi(kind) + ".";
        return Turn::Closed;
    }
    // Tried, and still up: again after kBackoffMs, every time, as often as it
    // takes (2026.11.24, the owner: "Kiểm tra các phần khác xem có phần nào
    // không làm được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác").
    // Until then, after kTries (2) in kTriesWindowMs (10 minutes) it was left
    // alone, the tab asking the player to close it. Never anything else for it
    // -- the board's X least of all -- and every gate as for the first try.
    tr.backoff_until = now() + kBackoffMs;
    say(std::string(popup_kind_name(kind)) + " is still up after closing it " + how + " (try " +
        std::to_string(tr.tries) + "); trying again in " + std::to_string(kBackoffMs / 1000) + " s");
    last_vi_ = board ? "Đã bấm ESC mà bảng Sự kiện và điểm danh vẫn còn: thử lại sau " +
                           std::to_string(kBackoffMs / 1000) + " giây."
                     : std::string("Đã thử đóng ") + popup_kind_vi(kind) + " mà vẫn còn: thử lại sau " +
                           std::to_string(kBackoffMs / 1000) + " giây.";
    return Turn::NotClosed;
}

// ------------------------------------------------- pictures for the owner

void CheckinShots::start(std::uint64_t now, int day, std::uint64_t crossed_in_ms) {
    started_ = now;
    start_kept_ = false;
    first_day_ = 0;
    day_ = day;
    // An arrival whose 15 s were over before this worker began is not its to
    // picture: the board, if it came, has been up a while or gone.
    crossed_seen_ = crossed_in_ms != 0 && crossed_in_ms + kAfterMs < now ? crossed_in_ms : 0;
}

std::vector<std::string> CheckinShots::due(std::uint64_t now, int day, int second, bool board_up, bool board_closed,
                                           std::uint64_t crossed_in_ms) {
    std::vector<std::string> out;
    if (board_up && first_day_ != day) {
        out.push_back("first");
        first_day_ = day;
    }
    if (board_closed) out.push_back("after");
    if (crossed_in_ms != 0 && crossed_in_ms != crossed_seen_ && now >= crossed_in_ms + kAfterMs) {
        out.push_back("in15");
        crossed_seen_ = crossed_in_ms;
    }
    if (!start_kept_ && now >= started_ + kAfterMs) {
        out.push_back("start15");
        start_kept_ = true;
    }
    // The PC's clock may be minutes out (Windows Time is off on the owner's
    // machine): the day turning over is what counts, at 00:00:30 or the first
    // look after it.
    if (day != day_ && second >= kMidnightS) {
        out.push_back("midnight");
        day_ = day;
    }
    return out;
}

std::string keep_popup_frame(const std::string& dir, const Frame& f, const std::string& why, bool probe) {
    if (dir.empty() || !f.valid() || !make_folder(dir)) return {};
    const std::string head = probe ? "probe_" : "checkin_";
    // The newest of each kind, room made for this one: named by when they
    // were taken, they sort oldest first.
    std::vector<std::string> old = files_in(dir, head + "*.png");
    std::sort(old.begin(), old.end());
    for (std::size_t i = 0; i + CheckinShots::kKeep <= old.size(); ++i) remove_file(dir + "\\" + old[i]);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
    // Two digits, so that several in one second still sort in the order taken.
    static std::atomic<int> n{ 0 };
    char count[8];
    std::snprintf(count, sizeof count, "%02d", n++ % 100);
    std::string tag;
    for (const char c : why) tag += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    const std::string path =
        dir + "\\" + head + stamp + "_" + count + (probe || tag.empty() ? std::string() : "_" + tag) + ".png";
    return save_png(f, path) ? path : std::string();
}

} // namespace gobot
