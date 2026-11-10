#pragma once

#include "gobot/auto_chat.hpp"
#include "gobot/bag.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/reviver.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gobot {

/// Closes the few popups that get in the way of a run, and nothing else.
///
/// GoC closes dialogs only around its own jobs, with ESC, on whatever it
/// recognises; it has no watcher during fighting. This one watches, so it is
/// held to far less: a short list of popups fixed here in the code -- not in a
/// file, so that a file cannot widen it -- each closed by its own control:
///
/// - the notice that a check-in reward was received ("Bạn đã nhận được vật
///   phẩm thư ống điểm danh, hãy kiểm tra hòm quà."): its Xác Nhận. The one
///   Xác Nhận this ever presses, and only on a dialog with that one button
///   whose words match the notice's at 0.90 -- every other dialog's words
///   score below 0 against them;
/// - Yêu Quái Bách Khoa, the monster book: its X, or ESC when its X cannot be
///   found and nothing else is up. Told by its first tab and its X, both cut
///   from the live client (bag::find_book) -- or by GoC's own picture of its
///   first page;
/// - the game's ESC menu, only when GoBot's own ESC brought it up a moment
///   before: its X. The player's own ESC menu may be on its way to Thoát Game;
/// - the "Sự kiện và điểm danh" board the game opens on its "Điểm danh"
///   calendar after every login and as the clock passes 00:00: ESC, and only
///   when nothing else of the list is up -- its notice is closed first. Told
///   by its title (checkin_board.hpp). Its X is never clicked: no frame of
///   the live board has been seen yet, and its place is the art's. Any close
///   button in the board's top band counts as its own, so that an X drawn a
///   little off the art's place does not keep it shut.
///
/// A popup is closed only once it has stayed in one place for 3 seconds, and
/// never on a look where anything else is up -- a dialog with Xác Nhận and Hủy
/// Bỏ (the bin's, the revive question, a shop's), a party invitation, another
/// one-button dialog, the death panel, the Nhiệm Vụ window, the bag, the chat
/// line open, or any window with a close button that is none of the above.
/// A close button on its own is never a reason to click anything.

enum class PopupKind {
    CheckinNotice,      ///< the check-in reward's notice
    MonsterBook,        ///< Yêu Quái Bách Khoa
    EscMenu,            ///< the game's ESC menu, when GoBot's own ESC opened it
    CheckinBoard,       ///< "Sự kiện và điểm danh", closed with ESC
};
constexpr int kPopupKinds = 4;
/// One bit a kind, for a set of kinds: what is looked for, what is allowed.
constexpr unsigned popup_bit(PopupKind k) { return 1u << static_cast<unsigned>(k); }
constexpr unsigned kAllPopups = (1u << kPopupKinds) - 1;
/// "the check-in notice", for the log.
const char* popup_kind_name(PopupKind k);
/// "thông báo nhận quà điểm danh", for the screen.
const char* popup_kind_vi(PopupKind k);

/// One allowed popup on one frame.
struct PopupSeen {
    PopupKind  kind = PopupKind::CheckinNotice;
    /// The popup. For the check-in board, the whole board as the art has it
    /// from its title (kCheckinBoardFromTitle); the title is at +298,3.
    ScreenRect at;
    /// Its picture; for the notice, its words; for the board, its title.
    float      score = 0.0f;
    /// Its own close control was found, at x, y. The monster book without it
    /// is closed with ESC, when nothing else is up.
    bool       control = false;
    int        x = 0, y = 0;
    /// Closed with ESC whether its X was found or not, when nothing else is
    /// up: the check-in board, whose X is never clicked.
    bool       by_esc = false;
};

/// What one look found.
struct PopupScene {
    std::vector<PopupSeen>   popups;
    /// What else is up, in English: any of it and nothing is done on this look.
    std::vector<std::string> blockers;

    bool blocked() const { return !blockers.empty(); }
    const PopupSeen* find(PopupKind k) const;
};

/// Everything looked at to say what is up. Load once; look as often as wanted,
/// from any one thread at a time.
class PopupEyes {
public:
    // ---- what is measured ----

    /// This client's Xác Nhận (GoC's inven/1/ok.png, which matches it at
    /// 1.000) and, from it, the check-in notice's top-left.
    static constexpr float kOkScore = 0.90f;
    static constexpr int   kNoticeOkDx = 146, kNoticeOkDy = 58;
    /// The notice's words, cut from grabbed/login_popup1 where every dialog
    /// has them (bag.cpp's words panel), and how alike they have to be.
    static constexpr int   kWordsX = 20, kWordsY = 12, kWordsW = 158, kWordsH = 28;
    static constexpr float kWordsScore = 0.90f;
    /// The monster book, GoC's inven/1/monsterf, its rounded edge left off:
    /// the book's first page, "Về Yêu Quái Bách Khoa". The live client opens
    /// it from a fragment on that monster's page, which this does not match;
    /// that one is told by bag::find_book, first.
    static constexpr float kBookScore = 0.85f;
    static constexpr int   kBookInset = 5;
    /// Where it is looked for first: its title box, "Từ Điển Bách Khoa Yêu
    /// Quái", a sixth of the picture and a tenth of the cost of a search with
    /// all of it; the whole picture is then matched only where that says.
    static constexpr int   kBookKeyX = 60, kBookKeyY = 20, kBookKeyW = 56, kBookKeyH = 196;
    /// Where GoC's first page has its X looked for: round its top-right
    /// corner. The live book's X is where bag::find_book measures it.
    static constexpr int   kBookXSlack = 40;
    /// The ESC menu (grabbed/client_menu_dialog) and its X -- inven/1's
    /// btn_CloseDialog -- measured on the live client: 156,7 from the menu's
    /// top-left.
    static constexpr float kMenuScore = 0.90f;
    static constexpr int   kMenuXDx = 156, kMenuXDy = 7;
    /// Any close button: the same X on the bag, Kho Cash and the menu.
    static constexpr float kCloseScore = 0.90f;
    /// The same place, give or take.
    static constexpr int   kSlack = 3;

    PopupEyes();
    ~PopupEyes();
    PopupEyes(const PopupEyes&) = delete;
    PopupEyes& operator=(const PopupEyes&) = delete;

    /// From a store holding the game's pictures and grabbed/, and the files
    /// that say where things are: trash.ini (the bag), party.ini (the
    /// invitation), chat.ini (the chat bar), quest.ini (the quest window).
    /// A file that is not there leaves its defaults. `store` has to outlive
    /// this.
    void load(const TemplateStore& store, const std::string& configs_dir);

    /// The pictures every look needs are there; `why` gets the first missing.
    bool ready(std::string* why = nullptr) const;

    /// One look. Finds the allowed popups first, and looks for anything else
    /// only when one is up: nothing is closed on a look that has not ruled
    /// all of it out, and most looks find nothing.
    PopupScene look(const Frame& f) const;

    /// The allowed popups alone, without what else is up. For the tab's
    /// "Thử xem" too, which then says what else is up with blockers().
    /// Without `with_menu` the ESC menu is not looked for: it is closed only
    /// straight after GoBot's own ESC, and the look made every second of a
    /// run is the cheaper for leaving it out. With `only_at`, the notice and
    /// the book are looked for only round that rectangle, a few pixels either
    /// way: a popup's own place, looked at again under the hold, in a few
    /// milliseconds rather than a search of the whole screen -- and the
    /// check-in board only as its title, within kCheckinRelookSlack of where
    /// a board at `only_at` has it: a 296x46 search. `kinds` (popup_bit) are
    /// the kinds looked for at all.
    std::vector<PopupSeen> popups(const Frame& f, bool with_menu = true, const ScreenRect* only_at = nullptr,
                                  unsigned kinds = kAllPopups) const;
    /// What else is up, given the popups found on the frame, in English for
    /// the log -- and in Vietnamese into `vi`, for the screen, when given.
    /// A close button is one of `popups`' own when it is where that popup's
    /// control is -- or, for the check-in board, anywhere in its top band.
    std::vector<std::string> blockers(const Frame& f, const std::vector<PopupSeen>& popups,
                                      std::vector<std::string>* vi = nullptr) const;

    /// The monster book's title box is somewhere in `area` -- or the live
    /// book's first tab where a book with the window `area` has it: some of
    /// the book is still up there, even with something drawn over the rest of
    /// it.
    bool book_title_in(const Frame& f, const ScreenRect& area) const;
    /// Some of the popup of `kind` that was at `where` is still up, even with
    /// something drawn over the rest of it: the book's title box
    /// (book_title_in), the board's title where it was. For the rule that an
    /// ESC menu seen once the popup is wholly gone was under it -- the
    /// player's. False for the kinds never closed with ESC.
    bool still_partly_in(PopupKind kind, const Frame& f, const ScreenRect& where) const;
    /// The check-in board's title picture is there: without it the board is
    /// never seen, and the rest of the list is closed as before.
    bool sees_board() const;

private:
    /// The best of the close buttons in `area`, by any of their pictures.
    bool close_button_in(const Frame& f, const ScreenRect& area, int& x, int& y) const;

    const TemplateStore*  store_ = nullptr;
    Frame ok_, notice_words_, book_, book_key_, menu_;
    /// Where ok_, trimmed to what is opaque, starts in GoC's picture.
    int   ok_dx_ = 0, ok_dy_ = 0;
    /// The close buttons, every look the client has for one: btn_CloseDialog,
    /// monster-close, btn_CloseBox.
    std::vector<Frame>    close_x_;
    BagLayout             bag_;
    PartyInvite::Settings invite_;
    ChatSettings          chat_;
    QuestSettings         quest_settings_;
    QuestPictures         quest_;
    /// The death panel is told by the reviver's own look; it is never asked
    /// to click anything here.
    std::unique_ptr<InputManager> no_hands_;
    std::unique_ptr<Reviver>      reviver_;
    std::vector<std::string>      missing_;
};

/// The worker's half: what to close and when, with the game in its own hands.
///
/// Everything it does to the game goes through a grab, a click, an ESC and a
/// clock, each replaceable, so the whole of it is tried on a pretend game in
/// the tests -- as Reviver and BagCleaner are.
class PopupCloser {
public:
    using Grab = std::function<bool(Frame&)>;
    using Report = std::function<void(const std::string&)>;

    /// Seen at one place for this long before it is closed: a popup that is
    /// going by itself, or that the player is reading, is left be.
    static constexpr std::uint64_t kSteadyMs = 3000;
    /// What is not allowed now -- a hold, the mouse taken, somebody at the
    /// machine -- puts the popup off this long.
    static constexpr std::uint64_t kBackoffMs = 30'000;
    /// Something else up beside it puts it off this long: telling what else
    /// is up is the dearest part of a look, and a bag left open for an hour
    /// is not looked at again every second.
    static constexpr std::uint64_t kBlockedMs = 5000;
    /// How long a click or an ESC is given to close it.
    static constexpr int kCheckMs = 1500;
    static constexpr int kCheckLookMs = 250;
    /// The longest the game is held for one close -- the potion keys with it,
    /// since a hold refuses the macro's presses: from asking for the hold to
    /// letting it go, at most this long (PU-R5).
    static constexpr std::uint64_t kHoldMs = 2000;
    /// Nothing is pressed later than this into the hold: a look before the
    /// press that took longer, on a busy machine, puts the popup off instead.
    /// A click lets go of the hold straight after; an ESC keeps it a little
    /// longer, to close the ESC menu it may have brought up.
    static constexpr std::uint64_t kPressByMs = 1500;
    static constexpr std::uint64_t kEscPressByMs = 1200;
    /// What one look after an ESC is allowed to take -- the capture, the look
    /// and a click on the menu's X -- before it would run past kHoldMs: the
    /// hold is let go rather than started on another.
    static constexpr std::uint64_t kLookAllowMs = 350;
    /// A look under the hold slower than this is said in the log: the board's
    /// is a 296x46 search, and one this slow is a busy machine (B-5).
    static constexpr std::uint64_t kSlowRelookMs = 600;
    /// Tries for one kind in any kTriesWindowMs; after them it is left alone.
    static constexpr int kTries = 2;
    static constexpr std::uint64_t kTriesWindowMs = 600'000;
    /// The ESC menu counts as GoBot's for this long after GoBot's own ESC.
    static constexpr std::uint64_t kOwnEscMs = 3000;

    /// What one turn came to.
    enum class Turn {
        Blind,      ///< no frame
        Nothing,    ///< no allowed popup up
        Waiting,    ///< one is up, not yet for kSteadyMs
        Blocked,    ///< one is up, with something else up beside it
        Held,       ///< one is up and may be closed, but not now: put off
        Closed,     ///< closed
        NotClosed,  ///< clicked or ESC'd, and still there
        LeftAlone,  ///< tried kTries times lately: left alone
    };
    static const char* turn_name(Turn t);

    /// What has to hold before anything is pressed. `gates` says why not now
    /// in English -- empty when it may -- and `take` takes the hold and the
    /// mouse and checks that nobody is at the machine, false when not now;
    /// `done` lets them go: straight after a click, and after an ESC once the
    /// ESC menu it may have brought up is dealt with -- within kHoldMs of
    /// `take` being asked, whatever happens. That menu, when it is first seen
    /// only once the game was let go, is closed under a second hold, asked for
    /// with `gates` and `take` as the first was and let go at its click: left
    /// up, it would be taken for the player's. `stopped` is asked the moment
    /// before the press and before the ESC menu's X: a stop -- F12, DỪNG,
    /// Ctrl+C -- that came while the fresh frame was looked at presses nothing
    /// more. Unset, everything is allowed: for tests.
    struct Guard {
        std::function<std::string()> gates;
        std::function<bool()>        take;
        std::function<void()>        done;
        std::function<bool()>        stopped;
    };
    /// What the closer does to the game. The game's own mouse and keyboard
    /// unless set; a test sets both, and nothing reaches the real desktop.
    struct Hands {
        std::function<bool(int, int)> click;    ///< a client point
        std::function<bool()>         esc;
    };
    struct Clock {
        std::function<std::uint64_t()> now_ms;
        std::function<void(int)>       sleep;
    };

    PopupCloser(InputManager& input, const PopupEyes& eyes, Grab grab);

    void set_guard(Guard g) { guard_ = std::move(g); }
    void set_hands(Hands h) { hands_ = std::move(h); }
    void set_clock(Clock c) { clock_ = std::move(c); }
    void on_report(Report r) { report_ = std::move(r); }
    /// The kinds that may be closed (popup_bit), read every turn: all of them
    /// unless set. A kind not allowed is never pressed and owns nothing on
    /// the screen -- its X is an unknown window's -- and the check-in board
    /// seen while it is not allowed keeps everything else shut, as any window
    /// does: a click or an ESC beside it may land on it.
    void set_allowed(unsigned kinds) { allowed_ = kinds & kAllPopups; }

    /// One look and, when a popup has stayed kSteadyMs and nothing forbids
    /// it, one click or one ESC, checked.
    Turn turn();

    /// What the last turn saw and did, in Vietnamese, for the tab.
    const std::string& last_vi() const { return last_vi_; }
    int closed() const { return closed_; }
    /// A popup of `kind` was up on the last turn's look.
    bool up(PopupKind kind) const { return tracks_[static_cast<int>(kind)].seen; }
    /// The last turn was about a popup of `kind`: the one it closed, tried,
    /// put off or left alone. False after a turn that saw nothing to close.
    bool last_about(PopupKind kind) const { return last_kind_ == static_cast<int>(kind); }

private:
    struct Track {
        bool          seen = false;
        ScreenRect    at;
        std::uint64_t since = 0;        ///< first seen at `at`
        std::uint64_t backoff_until = 0;
        std::vector<std::uint64_t> tries;   ///< when each try was made
        bool          said_left = false;    ///< "left alone" logged for this popup
        bool          said_blocked = false;
    };

    std::uint64_t now() const;
    void sleep(int ms);
    void say(const std::string& line);
    bool click(int x, int y);
    bool esc();
    /// The popup `want` on `scene`, at the same place as `at`, give or take.
    static const PopupSeen* same(const PopupScene& scene, PopupKind want, const ScreenRect& at);

    InputManager&    input_;
    const PopupEyes& eyes_;
    Grab             grab_;
    Guard            guard_;
    Hands            hands_;
    Clock            clock_;
    Report           report_;
    std::array<Track, kPopupKinds> tracks_{};
    std::uint64_t    own_esc_ms_ = 0;
    std::string      last_vi_;
    int              closed_ = 0;
    unsigned         allowed_ = kAllPopups;
    int              last_kind_ = -1;   ///< last_about's kind; -1 for none
    /// A turn's look, kept from turn to turn so that its 3 MB are grabbed
    /// into again rather than made anew every turn.
    Frame            turn_frame_;
};

/// Pictures of the game the popup worker keeps while "Tự đóng thông báo" is
/// on, for the owner to send: the check-in board has never been seen on the
/// live client, and these show where it is drawn, its X and whether it was
/// told. popup_debug\checkin_<time>_<why>.png, the newest kKeep of them:
///
/// - "first": the first sight of the board in a day, during its 3 s wait;
/// - "after": once after it was closed;
/// - "in15": about 15 s after each time the keeper got back in the world;
/// - "start15": about 15 s after the worker started (BẮT ĐẦU) -- a login by
///   hand has no keeper crossing;
/// - "midnight": about 00:00:30 by the PC's clock.
///
/// The last three whether the board is told or not. No threads and no clock
/// of its own: told the time at every call, so that a test runs a day of it
/// in a moment. The worker keeps what it says between turns, never between a
/// hold and its press.
class CheckinShots {
public:
    static constexpr std::uint64_t kAfterMs = 15'000;
    static constexpr int kMidnightS = 30;
    static constexpr int kKeep = 20;

    /// The worker started: `now` on the steady clock, `day` the PC's date as
    /// yyyymmdd, `crossed_in_ms` the keeper's last arrival on the same clock
    /// as `now` (0 for none) -- one well over kAfterMs before is not this
    /// worker's to picture.
    void start(std::uint64_t now, int day, std::uint64_t crossed_in_ms);
    /// What to keep now, in order: "first", "after", "in15", "start15",
    /// "midnight" -- each at most once. `second` is the second of the PC's
    /// day; `board_up` whether the last turn saw the board, `board_closed`
    /// whether it closed it.
    std::vector<std::string> due(std::uint64_t now, int day, int second, bool board_up, bool board_closed,
                                 std::uint64_t crossed_in_ms);

private:
    std::uint64_t started_ = 0;
    bool          start_kept_ = true;
    int           first_day_ = 0;       ///< the day the board's "first" was kept
    int           day_ = 0;             ///< the day the worker last saw
    std::uint64_t crossed_seen_ = 0;    ///< the arrival last pictured, or passed over
};

/// Keep `f` in `dir` as checkin_<time>_<nn>_<why>.png -- or, for the tab's
/// "Thử xem", probe_<time>_<nn>.png -- keeping the newest
/// CheckinShots::kKeep of each. The folder is made when it is not there.
/// Returns the file, or empty. Never on the window's own thread.
std::string keep_popup_frame(const std::string& dir, const Frame& f, const std::string& why, bool probe);

} // namespace gobot
