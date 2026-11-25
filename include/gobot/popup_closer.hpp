#pragma once

#include "gobot/auto_chat.hpp"
#include "gobot/bag.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/react_wait.hpp"
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
/// A popup is closed only once it has stayed in one place for 3 seconds -- the
/// check-in board only 30 s after the character's arrival in the world, and
/// never from 23:55 to 00:03 by the PC's clock (board_due) -- and
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
    /// The check-in board told only at kCheckinBoardCoveredScore, where it
    /// was seen whole a moment ago: the game's lines of news drawn over its
    /// title (PopupEyes::board_covered_at). Its X is not looked for.
    bool       covered = false;
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
    /// The same from the program's one set (TemplateStore::shared_best),
    /// held for as long as these eyes look with it.
    void load(std::shared_ptr<const TemplateStore> store, const std::string& configs_dir);

    /// The pictures every look needs are there; `why` gets the first missing.
    bool ready(std::string* why = nullptr) const;

    /// One look. Finds the allowed popups first, and looks for anything else
    /// only when one is up: nothing is closed on a look that has not ruled
    /// all of it out, and most looks find nothing.
    PopupScene look(const Frame& f) const;

    /// The allowed popups alone, without what else is up. For the tab's
    /// "Thử xem" too, which then says what else is up with blockers().
    /// Without `with_menu` the ESC menu is not looked for: it is closed only
    /// straight after GoBot's own ESC, and the worker's look, made all run
    /// long, is the cheaper for leaving it out. With `only_at`, the notice and
    /// the book are looked for only round that rectangle, a few pixels either
    /// way: a popup's own place, looked at again under the hold, in a few
    /// milliseconds rather than a search of the whole screen -- and the
    /// check-in board only as its title, within kCheckinRelookSlack of where
    /// a board at `only_at` has it: a 296x46 search. `kinds` (popup_bit) are
    /// the kinds looked for at all. Without `goc_book`, GoC's picture of the
    /// book's first page -- the dearest search of a look that finds nothing --
    /// is left out, unless the live book's tab was found without its X: the
    /// worker's idle looks leave it to every PopupCloser::kGocBookEvery-th.
    std::vector<PopupSeen> popups(const Frame& f, bool with_menu = true, const ScreenRect* only_at = nullptr,
                                  unsigned kinds = kAllPopups, bool goc_book = true) const;
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
    /// The check-in board that was at `board` (PopupSeen::at) is still there,
    /// its title within kSlack of where that board has it at
    /// kCheckinBoardCoveredScore rather than kCheckinBoardScore: the game
    /// draws its lines of news over the board's title bar, and one look in
    /// which they cover it is no sign the board went (2026-11-17). Only for a
    /// board the closer saw whole there a moment ago; `out`, when given, gets
    /// it as covered.
    bool board_covered_at(const Frame& f, const ScreenRect& board, PopupSeen* out = nullptr) const;

private:
    /// The best of the close buttons in `area`, by any of their pictures.
    bool close_button_in(const Frame& f, const ScreenRect& area, int& x, int& y) const;

    const TemplateStore*  store_ = nullptr;
    /// store_'s owner, when loaded from the shared set: blockers() searches
    /// it on every turn that sees a popup.
    std::shared_ptr<const TemplateStore> held_;
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
    /// machine -- puts the popup off this long; so does a click or an ESC
    /// that left it up, every time, without limit (2026.11.24, the owner:
    /// "Kiểm tra các phần khác xem có phần nào không làm được thì dừng không,
    /// cần thay cơ chế này bằng 1 cơ chế khác"). Until then two tries in ten
    /// minutes (kTries, kTriesWindowMs, gone) and it was left alone -- the
    /// check-in board left over the game until the player closed it.
    static constexpr std::uint64_t kBackoffMs = 30'000;
    /// Something else up beside it puts it off this long: telling what else
    /// is up is the dearest part of a look, and a bag left open for an hour
    /// is not looked at again every second.
    static constexpr std::uint64_t kBlockedMs = 5000;
    /// How long a click or an ESC is given to close it, by the clock from the
    /// press: the owner's 30 s for slow PCs (2026-11-15, react_wait.hpp --
    /// "Nếu màn hình phản ứng luôn thì thực hiện tiếp bước tiếp theo, còn nếu
    /// màn hình không phản ứng gì thì sẽ dừng xử lý"), 1.5 s before. The first
    /// look that finds it gone ends the check at once; the hold is let go
    /// within kHoldMs all the same, so only this worker waits, looking, a stop
    /// asked at every look. Nothing is pressed meanwhile but the ESC menu's X
    /// GoBot's own ESC brought up (kOwnEscMs).
    static constexpr int kCheckMs = kReactMs;
    static constexpr int kCheckLookMs = 250;
    /// Once GoBot's ESC is seen to have brought up the ESC menu -- one ESC
    /// does one thing, and that was it -- the popup is watched only this much
    /// longer, as the whole check was before 2026-11-15, not the rest of
    /// kCheckMs.
    static constexpr int kAfterMenuMs = 1500;
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
    /// The ESC menu counts as GoBot's for this long after GoBot's own ESC:
    /// the whole of the check after it, and a little over. It was 3 s while
    /// the check was 1.5 s; with the check at 30 s for slow PCs, a menu a slow
    /// game draws 4 s after the ESC is still that ESC's one thing, and taken
    /// for the player's it was left up -- blocking every close after -- with
    /// the book under it called closed (review 2026-11-15). A menu the player
    /// had open under the popup is told apart by what is left of the popup,
    /// not by the clock (PopupEyes::still_partly_in).
    static constexpr std::uint64_t kOwnEscMs = static_cast<std::uint64_t>(kCheckMs) + 3000;
    /// The worker's wait between turns (next_look_ms): a second while
    /// anything is up -- a popup waiting its 3 s, put off, tried -- and, after
    /// a look that saw nothing, 2 s, or 3 s in light mode ("Nhẹ máy"): most
    /// looks find nothing, and on a 1-core PC that runs the game too each one
    /// is time the game does not get (2026-11-16). A popup still stays put
    /// kSteadyMs from its first sight, so it goes 3 to 6 s after it comes up
    /// rather than 3 to 4 s; the board's 30 s and its night hold are counted
    /// as before.
    static constexpr int kLookMs = 1000;
    static constexpr int kIdleLookMs = 2000;
    static constexpr int kIdleLookLightMs = 3000;
    /// GoC's picture of the monster book's first page is looked for on every
    /// look while anything is up, and otherwise only on every this-many-th
    /// look in a row that saw nothing -- the first of them included -- or at
    /// once when the live book's tab is found without its X
    /// (PopupEyes::popups' goc_book). It is the largest picture searched
    /// (2026-11-16).
    static constexpr int kGocBookEvery = 5;
    /// A check-in board seen again where it was, no later than this after the
    /// first look that missed it -- whole or covered -- is the same board
    /// (time with no look at all, the game behind another window, is not
    /// counted): its first
    /// sight -- the start of its 30 s (board_due) -- is kept, and so is what
    /// was said about it. The game's lines of news drawn over its title made
    /// one look miss it on 2026-11-17, and its 30 s started over; seen again a
    /// few seconds later, it waited another 30, and the run was stopped before
    /// its ESC. Short: a look cannot tell a board it missed from one the
    /// player closed with its X, and one the player opened again a while
    /// later to do the check-in is a new board, given its own 30 s to be read
    /// (review of 2026.11.17c). A new arrival in the world since it was last
    /// seen, or a board GoBot closed, starts afresh as before.
    static constexpr std::uint64_t kBoardGapMs = 15'000;
    /// The board's title told at the lower bar (kCheckinBoardCoveredScore)
    /// only within this of the board's last sight whole at that place: news
    /// that cover its title for longer than this leave it unseen.
    static constexpr std::uint64_t kBoardGraceMs = 60'000;

    /// What one turn came to.
    enum class Turn {
        Blind,      ///< no frame
        Nothing,    ///< no allowed popup up
        Waiting,    ///< one is up, not yet for kSteadyMs -- the board: not yet board_due
        Blocked,    ///< one is up, with something else up beside it
        Held,       ///< one is up and may be closed, but not now: put off
        Closed,     ///< closed
        /// Clicked or ESC'd, and still there: tried again kBackoffMs on, as
        /// often as it takes. (Turn::LeftAlone -- two tries in ten minutes,
        /// then nothing -- is gone since 2026.11.24.)
        NotClosed,
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
    /// `arrived_ms`: the character's last arrival in the world -- the keeper's
    /// crossing -- on now_ms's clock, 0 when there was none; unset, none.
    /// `local_second`: the second of the PC's day, 0 to 86399, or negative for
    /// none. Unset, the PC's own (local_second_of_day) -- but none at all on a
    /// pretend clock (now_ms set), which has no time of day. Both for the
    /// check-in board's ESC (board_due).
    struct Clock {
        std::function<std::uint64_t()> now_ms;
        std::function<void(int)>       sleep;
        std::function<std::uint64_t()> arrived_ms;
        std::function<int()>           local_second;
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

    /// Whether the check-in board may have its ESC now -- the owner,
    /// 2026-11-15: "Sau khi chọn nhân vật xong và vào tới màn hình game thì sẽ
    /// chờ 30 giây rồi mới ấn ESC để tắt thông báo. Trường hợp qua ngày thì chờ
    /// tới 00:03 mới ấn ESC." All of, at `at` on the closer's clock: it has
    /// stayed at its place kSteadyMs since `since`, its first sight there --
    /// kept through looks that miss it for up to kBoardGapMs (2026.11.17c);
    /// kCheckinBoardLoginWaitMs have passed since the later of that sight and
    /// `arrived`, the character's arrival in the world (0: not known -- a login
    /// by hand -- and the sight stands for it, the board coming up right at
    /// the arrival); and the PC's clock, `second` of its day (negative: not
    /// known), is not in the night hold (checkin_night_hold). Only the board:
    /// the notice's Xác Nhận and the book keep their kSteadyMs. A wait here is
    /// no try and no backoff -- the board is looked at every turn and pressed
    /// at the first that it is due.
    static bool board_due(std::uint64_t at, std::uint64_t since, std::uint64_t arrived, int second);

    /// How long the worker waits after a turn that came to `last` before the
    /// next: kIdleLookMs after one that saw nothing (kIdleLookLightMs with
    /// `light`), kLookMs after any other.
    static int next_look_ms(Turn last, bool light);

    /// What the last turn saw and did, in Vietnamese, for the tab.
    const std::string& last_vi() const { return last_vi_; }
    int closed() const { return closed_; }
    /// A popup of `kind` was up on the last turn's look.
    bool up(PopupKind kind) const { return tracks_[static_cast<int>(kind)].seen; }
    /// The last turn was about a popup of `kind`: the one it closed, tried or
    /// put off -- the board too while it waits its 30 s or the night hold.
    /// False after a turn that saw nothing to close.
    bool last_about(PopupKind kind) const { return last_kind_ == static_cast<int>(kind); }
    /// The closer's last look at the game: the turn's own, or -- after a
    /// press -- the last that watched for the popup to go. The worker's HUD
    /// gate and its pictures of the board read it, rather than a copy of
    /// every grab of their own (2026-11-16). Only from the worker's thread,
    /// between turns or from the guard's calls within one.
    const Frame& last_look() const { return turn_frame_; }

private:
    struct Track {
        bool          seen = false;
        ScreenRect    at;
        std::uint64_t since = 0;        ///< first seen at `at`
        std::uint64_t backoff_until = 0;
        /// Clicks or ESCs made on this sighting, for the log's "try N" --
        /// counted only: none is the last since 2026.11.24, when the cap of two
        /// in ten minutes went.
        int           tries = 0;
        bool          said_blocked = false;
        /// The board's wait (board_due) last logged for this sighting: 0 none,
        /// 1 the 30 s after the arrival, 2 the night hold.
        int           said_wait = 0;
        /// The board only (kBoardGapMs): `at` and `since` stand for a
        /// sighting a look that missed it does not end -- when it was last
        /// seen at `at`, last seen whole there (not covered), and the first
        /// look since its last sight that missed it.
        bool          placed = false;
        std::uint64_t last_seen = 0;
        std::uint64_t whole_seen = 0;
        std::uint64_t missed_at = 0;
        /// When the board was seen again after a look that missed it, 0 for
        /// not. No ESC until it has been back kSteadyMs: the look that found
        /// it again came after looks that saw nothing, and so most often
        /// without GoC's picture of the monster book (kGocBookEvery) -- a
        /// book opened meanwhile would get the ESC (review of 2026.11.17c).
        std::uint64_t back_at = 0;
        /// The board's reasons for no ESC once it was due, each logged once a
        /// sighting (the Why bits); and the gate last said for it.
        unsigned      said_why = 0;
        std::string   said_gate;
        /// Looks under the hold in a row that did not find the board again:
        /// from the second, it is put off kBlockedMs -- a hold every second
        /// would keep the potion keys waiting for nothing.
        int           relook_misses = 0;
    };
    /// Why a due board got no ESC on a turn, for Track::said_why.
    enum Why : unsigned {
        kWhyGap     = 1u << 0,      ///< out of sight for a look or more, and seen again
        kWhyCovered = 1u << 1,      ///< told only as covered
        kWhyOther   = 1u << 2,      ///< another popup of the list up beside it
        kWhyRelook  = 1u << 3,      ///< not found again under the hold
        kWhyBlind   = 1u << 4,      ///< no frame under the hold
        kWhyTake    = 1u << 5,      ///< the hold not taken (Guard::take)
    };
    /// `line` said once a sighting of the board, for `why`.
    void say_board(Track& tr, unsigned why, const std::string& line);

    std::uint64_t now() const;
    /// The second of the PC's day for board_due: Clock::local_second, else
    /// none (-1) on a pretend clock, else the PC's own.
    int local_second() const;
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
    /// Looks in a row that saw nothing, modulo kGocBookEvery: GoC's book
    /// picture is looked for when it is 0.
    int              idle_looks_ = 0;
    /// A turn's look -- after a press, the last look that watched for the
    /// popup to go, swapped in -- kept from turn to turn so that its 3 MB are
    /// grabbed into again rather than made anew every turn (last_look).
    Frame            turn_frame_;
};

/// Pictures of the game the popup worker keeps while "Tự đóng thông báo" is
/// on, for the owner to send: the check-in board has never been seen on the
/// live client, and these show where it is drawn, its X and whether it was
/// told. popup_debug\checkin_<time>_<why>.png, the newest kKeep of them:
///
/// - "first": the first sight of the board in a day, during its wait;
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
