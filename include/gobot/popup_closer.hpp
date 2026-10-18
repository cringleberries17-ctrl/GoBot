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
///   found and nothing else is up;
/// - the game's ESC menu, only when GoBot's own ESC brought it up a moment
///   before: its X. The player's own ESC menu may be on its way to Thoát Game.
///
/// The check-in board ("Sự kiện và điểm danh") is not on the list yet: no
/// frame of it has been taken from the live client, and its close button is
/// not known to be where a picture would say.
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
};
constexpr int kPopupKinds = 3;
/// "the check-in notice", for the log.
const char* popup_kind_name(PopupKind k);
/// "thông báo nhận quà điểm danh", for the screen.
const char* popup_kind_vi(PopupKind k);

/// One allowed popup on one frame.
struct PopupSeen {
    PopupKind  kind = PopupKind::CheckinNotice;
    ScreenRect at;                  ///< the popup
    float      score = 0.0f;        ///< its picture; for the notice, its words
    /// Its own close control was found, at x, y. The monster book without it
    /// is closed with ESC, when nothing else is up.
    bool       control = false;
    int        x = 0, y = 0;
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
    /// The monster book, GoC's inven/1/monsterf, its rounded edge left off.
    static constexpr float kBookScore = 0.85f;
    static constexpr int   kBookInset = 5;
    /// Where it is looked for first: its title box, "Từ Điển Bách Khoa Yêu
    /// Quái", a sixth of the picture and a tenth of the cost of a search with
    /// all of it; the whole picture is then matched only where that says.
    static constexpr int   kBookKeyX = 60, kBookKeyY = 20, kBookKeyW = 56, kBookKeyH = 196;
    /// Where its X is looked for: not measured on the live client yet, so
    /// round the book's top-right corner.
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
    /// milliseconds rather than a search of the whole screen.
    std::vector<PopupSeen> popups(const Frame& f, bool with_menu = true, const ScreenRect* only_at = nullptr) const;
    /// What else is up, given the popups found on the frame, in English for
    /// the log -- and in Vietnamese into `vi`, for the screen, when given.
    std::vector<std::string> blockers(const Frame& f, const std::vector<PopupSeen>& popups,
                                      std::vector<std::string>* vi = nullptr) const;

    /// The monster book's title box is somewhere in `area`: some of the book
    /// is still up there, even with something drawn over the rest of it.
    bool book_title_in(const Frame& f, const ScreenRect& area) const;

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

    /// One look and, when a popup has stayed kSteadyMs and nothing forbids
    /// it, one click or one ESC, checked.
    Turn turn();

    /// What the last turn saw and did, in Vietnamese, for the tab.
    const std::string& last_vi() const { return last_vi_; }
    int closed() const { return closed_; }

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
};

} // namespace gobot
