#pragma once

#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace gobot {

/// Quick quest -- "Làm nhiệm vụ nhanh" -- the way GoC does it for the
/// Vietnamese client.
///
/// GoC has two versions of this. For the English client it picks a quest out
/// of two hundred pictures of quest names, accepts it and hands it in. For the
/// Vietnamese VTC client -- the player's -- it does much less: it opens the
/// Nhiệm Vụ window, goes to the NV Phụ tab and ticks the game's own "Tự Động"
/// there, and the game does the rest. That is what this does, with GoC's own
/// pictures and GoC's own points. It never accepts, hands in, gives up or
/// chooses a quest itself, and the English version is not built: its pictures
/// are of English words, which the Vietnamese client never shows.
///
/// Two things are done differently, on purpose. GoC ticks the box whenever its
/// server says a quick quest has finished; GoBot has no server, so it looks on
/// a timer. And GoC clicks the box without looking whether it is ticked
/// already -- which may well untick one that is on, though only the live
/// client can say; GoBot reads the box first, and leaves a ticked one alone.
///
/// The window has things on it that must never be clicked: Hủy Nhiệm Vụ, which
/// gives up the quest picked in the list, the quick quest's own Accept, Give up
/// and Complete buttons in the same place, and a checkbox at the top right that
/// hides low-level quests -- drawn with the very same picture as the unticked
/// Tự Động box. So a click is only ever made at two points worked out from what
/// was found, and a point that falls on any of them, on another tab or outside
/// the window is refused rather than moved.

// ----------------------------------------------------------------- settings

/// configs/quest.ini. No file is quick quest off.
struct QuestSettings {
    bool          enabled = false;
    /// Minutes from one round to the next, before the jitter.
    int           interval_min = 5;
    /// Seconds from BẮT ĐẦU to the first round: after the trash cleaner's
    /// first pass and Tự chat's first line, so the three do not queue up.
    int           first_delay_s = 40;
    /// The key that opens and closes the window. U is GoC's own default --
    /// its label says Q, its code and the player's GoC file say U.
    std::uint32_t quest_key = 85;
    /// Failed rounds in a row before it stops itself and says so.
    int           max_failures = 3;
    /// Look less often while the box stays ticked: after three rounds in a
    /// row that find it on, the interval doubles, up to half an hour.
    bool          back_off = true;

    /// Which client: worked out from the window's tabs, or taken as said.
    enum class Skin { Auto, Vi, En };
    Skin          skin = Skin::Auto;

    // ---- [Layout]: from the window's top-left, measured on GoC's picture of
    // the window, where every part of it matches at 1.000.

    /// The title bar: the top rows, less this many columns either side, where
    /// the window's rounded corners show whatever is behind it.
    int        title_rows = 32;
    int        title_inset = 10;
    /// The four tabs -- NV Chính, Đang Tiến Hành, Hoàn Thành, NV Phụ -- and
    /// how far apart they are.
    ScreenRect tabs_area{ 10, 32, 302, 28 };
    int        tab_pitch = 76;
    /// GoC's point inside NV Phụ.
    int        tab_click_x = 274, tab_click_y = 47;
    /// Where Tự Động is looked for: the bottom hundred pixels, as GoC looks.
    ScreenRect auto_area{ 0, 360, 672, 100 };
    /// From Tự Động's top-left: GoC's click, and the box itself.
    int        auto_click_x = 11, auto_click_y = 15;
    ScreenRect auto_box{ 4, 8, 14, 14 };
    /// The pointer moves this far right after the click, as GoC's does,
    /// before the box is read again.
    int        hover_dx = 14;
    /// Never clicked: Hủy Nhiệm Vụ, and the hide-low-level-quests checkbox.
    ScreenRect forbid_abandon{ 296, 417, 80, 30 };
    ScreenRect forbid_hide_low{ 523, 37, 14, 14 };
    /// Nor where the quick quest's Accept, Give up and Complete buttons go --
    /// the area GoC's English-client flow clicks and reads them in, around Hủy
    /// Nhiệm Vụ's place. GoBot never loads their pictures, so they are kept
    /// out of by place, as Hủy Nhiệm Vụ is.
    ScreenRect forbid_buttons{ 283, 412, 103, 41 };

    // ---- [Pictures]: names in the template store.
    std::string window = "quest/1/Dlg_Quest_Neo";
    std::string window_en = "quest/0/Dlg_Quest_Neo";
    std::string auto_option = "quest/1/radio_LetterQuest_Auto.spr/0";
    std::string box_on = "quest/1/btn_QuestNeo_Radio.spr/0";
    std::string box_off = "quest/1/btn_QuestNeo_Radio.spr/1";
    /// The tabs' own pictures, in order, in the folder of each window
    /// picture: each in two looks, /0 and /1. The window's picture draws all
    /// four in their /0 look, and the live window draws whichever tab is
    /// picked -- and the one under the pointer -- in another, so each tab is
    /// matched in either look.
    std::vector<std::string> tabs = { "btn_NewQuestDlg_QuestList.spr",
                                      "btn_NewQuestDlg_ProcessingQuest.spr",
                                      "Btn_NewQuestDlg_CompleteQuest.spr",
                                      "btn_NewQuestDlg_LetterQuest.spr" };

    // ---- [Scores]
    float window_score = 0.85f;     ///< the title bar
    float tab_score = 0.85f;        ///< every tab, in its better look
    float auto_score = 0.80f;       ///< Tự Động
    float state_score = 0.80f;      ///< the box, ticked or not
    float state_margin = 0.10f;     ///< and by this much over the other look

    // ---- [Timing], in milliseconds: GoC's own waits.
    int open_hold_ms = 50;          ///< the key held, opening
    int close_hold_ms = 100;        ///< and closing
    int open_wait_ms = 500;
    int open_look_ms = 1500;        ///< then looked for every 250 ms for this long
    int tab_wait_ms = 500;
    int click_wait_ms = 300;
    int hover_wait_ms = 200;
    int close_wait_ms = 1000;       ///< for the window to go once the key is pressed
    int round_limit_ms = 10000;     ///< a round never presses anything after this

    static constexpr int kMinInterval = 1;
    static constexpr int kMaxInterval = 60;
    static int clamp_interval(int minutes);

    /// False, and nothing changed, when the file cannot be read or has no
    /// [Quest] section. Sections and keys in any case, a BOM in front
    /// ignored, and anything out of range brought into it: the file is
    /// edited by hand too.
    bool load(const std::string& path);

    /// Write one key, leaving the rest of the file -- its comments above all --
    /// as it is. The file is made, off, if it is not there; an existing one
    /// is never emptied first.
    static bool save_key(const std::string& path, const std::string& section,
                         const std::string& key, const std::string& value);
};

// ----------------------------------------------------------------- pictures

/// The pictures a round goes by, cut and checked once.
///
/// Kept as copies rather than as a whole template store: a worker that runs
/// for hours needs a dozen small pictures, not the sixty megabytes of the
/// full set.
struct QuestPictures {
    /// The window's title bar, cut from its picture: the Vietnamese client's,
    /// and the English one's, which is the same bar with other words.
    Frame title;
    Frame title_en;
    int   window_w = 0, window_h = 0;
    /// Each tab in both its looks, the Vietnamese client's and the English.
    std::vector<std::array<Frame, 2>> tabs;
    std::vector<std::array<Frame, 2>> tabs_en;
    /// Tự Động, with its box left out: the box is read on its own, so a
    /// ticked one is found as readily as one that is not.
    Frame auto_option;
    Frame box_on, box_off;
    /// What was asked for and not there, by name, for saying so.
    std::vector<std::string> missing;

    /// Everything the Vietnamese client's round needs.
    bool ready() const;
    /// The English client's window can be told apart too.
    bool knows_en() const;

    /// From a store that has them under the names in `s`.
    void load(const TemplateStore& store, const QuestSettings& s);
    /// Straight from a folder laid out like templates\: a dozen small files.
    bool load_dir(const std::string& dir, const QuestSettings& s);
    /// templates\ beside the program, else GoC's own pictures. `from` gets
    /// the folder they came from. False when neither has them all.
    bool load_best(const QuestSettings& s, std::string* from = nullptr);
};

/// What the tab says when pictures are missing, in Vietnamese: the first one
/// or two, as files under templates\.
std::string quest_missing_vi(const QuestPictures& p);

// ------------------------------------------------------------------ looking

/// The Nhiệm Vụ window, as found in one frame.
struct QuestWindow {
    bool       found = false;
    ScreenRect at;                  ///< the whole window
    float      score = 0.0f;        ///< its title bar
    /// The tab row: the worst of the four tabs, each in its better look.
    float      tabs = 0.0f;
    /// The same with the English client's tabs; -1 when not looked at.
    float      tabs_en = -1.0f;
    bool       english = false;     ///< the English client's window
    /// The best title bar seen, found or not: for a probe to say how near.
    float      best_title = 0.0f;
};

/// Look for the window: its title bar anywhere, then at the same place, give
/// or take two pixels, every one of its four tabs. The title alone is not
/// enough -- the English client's window has the same bar, and GoC's picture
/// of a bar with "Quest" on it scores about 0.91, over the bar's own 0.85, so
/// it is the tab row under the bar that tells a window from a bar alone -- and
/// the tabs alone are not either. Which client it is goes by whose tabs fit
/// better.
QuestWindow find_quest_window(const Frame& f, const QuestPictures& p, const QuestSettings& s);

/// Tự Động's box.
enum class AutoState { Unknown, Off, On };
const char* auto_state_name(AutoState s);   ///< "on", "off", "unknown"

/// Tự Động, as found in one frame.
struct AutoOption {
    bool       found = false;
    ScreenRect at;                  ///< the Tự Động picture
    float      score = 0.0f;
    AutoState  state = AutoState::Unknown;
    /// The box against each look; -1 when it could not be compared.
    float      on = -1.0f, off = -1.0f;
    int        click_x = 0, click_y = 0;   ///< GoC's point
};

/// Look for Tự Động in the bottom of `window`, and read its box.
AutoOption find_auto_option(const Frame& f, const ScreenRect& window, const QuestPictures& p,
                            const QuestSettings& s);

/// The box whose top-left is at `x`, `y`, give or take two pixels: on when it
/// is like the ticked picture and by `state_margin` more than the other, off
/// the other way round, and unknown otherwise.
AutoState read_quest_box(const Frame& f, int x, int y, const QuestPictures& p,
                         const QuestSettings& s, float& on, float& off);

/// How alike `tpl` is to the frame at `x`, `y`, in grey, over the picture's
/// opaque pixels: -1 when it does not fit or either is flat. The best of
/// every place within `radius` pixels.
float quest_likeness(const Frame& f, int x, int y, const Frame& tpl, int radius);

/// Whether a round may click at `x`, `y` with the window at `window` on a
/// client area `client_w` by `client_h`: inside both, and nowhere near Hủy
/// Nhiệm Vụ, the quick quest's buttons, the hide-low-level checkbox or the
/// other three tabs. A point that is not is refused -- never moved somewhere
/// that is. `why` gets which, in English, for the log; `why_vi` the same in
/// Vietnamese, for the tab.
bool quest_click_ok(const ScreenRect& window, int x, int y, int client_w, int client_h,
                    const QuestSettings& s, std::string* why = nullptr,
                    std::string* why_vi = nullptr);

/// Why a round must not press `quest_key`, in Vietnamese -- empty when it may.
/// GoBot's own switches are watched by GoBot itself, which cannot tell its own
/// press of one from the player's: the panic key would stop everything in the
/// middle of the round, a start/stop key would start or stop the bot. The tab
/// refuses them, but quest.ini is edited by hand too.
std::string quest_key_clash_vi(std::uint32_t quest_key, const std::vector<std::uint32_t>& own_keys);

// -------------------------------------------------------------------- round

/// How a round ended.
enum class QuestOutcome {
    None,
    TurnedOn,       ///< the box was off; clicked; now on
    AlreadyOn,      ///< the box was on; nothing clicked
    NoWindow,       ///< the key did not bring the window up, or it went
    NoAuto,         ///< no Tự Động in NV Phụ
    UnknownState,   ///< Tự Động found, its box neither like on nor like off
    ClickFailed,    ///< clicked once, and the box did not show on
    Refused,        ///< a point was not safe to click, or the input did not go
    English,        ///< the English client's window
    NoFrame,        ///< the game could not be captured
    NoPictures,
    Cancelled,      ///< stopped part way
    TooSlow,        ///< the round ran out of time
    /// Not started, because something else needs the game, or given up part
    /// way because HP fell under the potion's mark. Not a failure.
    NotNow,
};
/// "turned_on", "already_on", ... for the log and the command line.
const char* quest_outcome_name(QuestOutcome o);

struct QuestResult {
    QuestOutcome outcome = QuestOutcome::None;
    /// The window is GoBot's to close: this round opened it, or found it open
    /// where an earlier round had left it.
    bool         opened_by_us = false;
    /// A window that was GoBot's to close could not be seen to close.
    bool         left_open = false;
    /// The key went in and nothing GoBot knows came up: whatever it opened,
    /// if anything, may still be open.
    bool         maybe_open = false;
    /// The round saw the game at least once, so what it says of the window is
    /// news -- one that never looked leaves what was known as it was.
    bool         looked = false;
    int          keys_pressed = 0;
    int          clicks = 0;
    QuestWindow  window;
    AutoOption   before;            ///< Tự Động before any click
    AutoOption   after;             ///< and after the one click, when there was one
    /// For NotNow and Refused: why, in Vietnamese.
    std::string  why;
    /// The whole round in one line of English, with its scores and points.
    std::string  log;
    /// The last frame looked at, to keep when the round went wrong.
    Frame        last;

    /// The box is ticked, and the window is not left open.
    bool done() const;
    /// A round that went wrong, counted towards stopping.
    bool failed() const;
};

/// What became of a round, for the tab: "đã bật ô Tự Động", "không mở được
/// cửa sổ Nhiệm Vụ (đã bấm U)" -- `key` is the quest key, named.
std::string quest_result_vi(const QuestResult& r, const std::string& key);
/// The fixed words the quest_trouble alert gives as its reason.
std::string quest_trouble_why(const QuestResult& r);

/// One round: open the window if it is not, go to NV Phụ, tick Tự Động if it
/// is off, and close the window again if this round opened it.
///
/// At most two key presses and two clicks, and nothing after round_limit_ms.
/// Every key and click is asked about first -- a stop, F12, the keeper
/// starting a crossing, HP falling under the potion's mark -- and nothing at
/// all is pressed after one, not even the key that would close the window:
/// whoever stopped it has taken over, and the potion is not kept waiting. The
/// box is clicked at most once a round, and only once a look has shown it
/// off. A window the player had open stays open.
class QuestRound {
public:
    /// What the round does to the game, one function each: the game's own
    /// input in the GUI, a pretend game in a test.
    struct Hands {
        std::function<bool(Frame&)>                       grab;
        std::function<bool(std::uint32_t vk, int hold_ms)> key;
        std::function<bool(int x, int y)>                 click;
        std::function<bool(int x, int y)>                 move;
        std::function<std::uint64_t()>                    now;
        std::function<void(int ms)>                       sleep;
        /// Asked before every key, click and move: true stops the round there.
        std::function<bool()>                             stop;
        /// Asked as often: true when HP has fallen under what the HP potion
        /// fires at, which the round's hold keeps off. The round ends there as
        /// for a stop, pressing nothing more, but as not now rather than as a
        /// failure; a window it opened is left for the next round to close.
        std::function<bool()>                             low_hp;
        /// Asked as often: true when the game is no longer the window in
        /// front -- the player has gone to another one. The round ends there
        /// as for low HP: not now, since nothing in the game went wrong, and
        /// a player who often switches windows would otherwise have quick
        /// quest stop itself, with an alert, over nothing.
        std::function<bool()>                             behind;
    };
    using Report = std::function<void(const std::string&)>;

    QuestRound(const QuestPictures& pictures, const QuestSettings& settings, Hands hands);

    /// Each step, in English, as it happens: for the log.
    void on_report(Report r) { report_ = std::move(r); }
    /// From any thread. A round that is cancelled stays cancelled.
    void cancel() { cancelled_ = true; }
    /// An earlier round opened the window and could not see it shut: a window
    /// found open is taken for that one -- dealt with and closed again -- and
    /// not for the player's, which would stay open all run. A window the
    /// player opened since is closed with it; there is no telling them apart.
    void adopt_open_window(bool yes) { adopt_ = yes; }

    QuestResult run();

private:
    /// Why a round stops part way, once it has: kept, so that a stop stays a
    /// stop.
    enum class Halt { None, Stop, LowHp, Behind };
    bool halted();
    /// The outcome for a round that halted: cancelled for a stop, not now for
    /// HP. A window that was GoBot's to close may be open still.
    void set_halted(QuestResult& r, const std::string& where);
    bool wait(int ms);                  ///< false when halted meanwhile
    bool look(Frame& f);
    bool press(QuestResult& r, int hold_ms);
    bool click(QuestResult& r, const ScreenRect& window, int x, int y, const char* what);
    /// Close a window that is GoBot's to close, on a fresh look that shows it
    /// open. True when a halt ended it part way, which is then the round's
    /// outcome rather than the caller's: a stop is not a failure.
    bool close(QuestResult& r);
    void note(QuestResult& r, const std::string& part);
    std::uint64_t clock();

    const QuestPictures& pictures_;
    QuestSettings        settings_;
    Hands                hands_;
    Report               report_;
    std::atomic<bool>    cancelled_{false};
    Halt                 halt_ = Halt::None;
    bool                 adopt_ = false;
    std::uint64_t        started_ = 0;
    int                  client_w_ = 0, client_h_ = 0;
};

/// A look that presses nothing: what the tab's Thử xem and gobot_cli
/// --quest-probe report.
struct QuestProbe {
    QuestWindow window;
    AutoOption  option;
    /// Whether a round would click where it would have to -- NV Phụ, and Tự
    /// Động -- or refuse: a probe that finds everything is not enough when a
    /// point falls where nothing may be clicked.
    bool        tab_ok = false;
    bool        click_ok = false;
    std::string vi;                 ///< what was seen, in Vietnamese
    std::string log;                ///< the same in English, with the numbers
};
QuestProbe probe_quest(const Frame& f, const QuestPictures& p, const QuestSettings& s);

/// Keep `f` in `dir` as <time>_<what>.png, or probe_<time>.png for a probe,
/// keeping the newest twenty of each. Returns the file, or empty.
std::string keep_quest_frame(const std::string& dir, const Frame& f, const std::string& what,
                             bool probe);

// ------------------------------------------------------------------- timing

/// When the next round is due.
///
/// The first one first_delay_s after the start, then one every interval_min,
/// give or take a tenth. Three rounds in a row that find the box ticked
/// already double the interval each time, up to half an hour -- the option
/// may well stay on all night -- and a round that finds it off goes back to
/// interval_min. Failures in a row are counted here too.
class QuestSchedule {
public:
    explicit QuestSchedule(std::uint32_t seed = std::random_device{}());

    void start(std::uint64_t now, const QuestSettings& s);
    /// The settings changed while running. A shorter interval applies to the
    /// round already waiting.
    void update(const QuestSettings& s);

    bool          due(std::uint64_t now) const { return due_ != 0 && now >= due_; }
    std::uint64_t due_at() const { return due_; }

    /// Not now: looked at again `ms` from now.
    void postpone(std::uint64_t now, std::uint64_t ms);
    /// A round soon -- the keeper got back in, the character was revived --
    /// unless one is due sooner.
    void soon(std::uint64_t now, std::uint64_t ms);
    /// A round ended at `now`: the next one an interval from then.
    void done(std::uint64_t now, const QuestResult& r);

    std::uint64_t last_end() const { return last_end_; }
    int           failures() const { return failures_; }
    int           on_streak() const { return on_streak_; }
    /// The interval as it stands, the back-off included, before the jitter.
    std::uint64_t interval_ms() const;
    /// The gap after a round: interval_ms() give or take a tenth. Public for
    /// testing.
    std::uint64_t draw_gap_ms();

    static constexpr std::uint64_t kLongest = 30ull * 60'000;

private:
    std::mt19937  rng_;
    int           interval_min_ = 5;
    bool          back_off_ = true;
    std::uint64_t due_ = 0;
    std::uint64_t last_end_ = 0;
    int           failures_ = 0;
    int           on_streak_ = 0;
};

/// Everything that decides whether a round may start now.
struct QuestGateInput {
    bool        window = false;     ///< the game's window is there
    bool        keeper = false;     ///< the keeper is crossing back in
    float       hp_pct = -1.0f;     ///< -1: the HP bar is not on screen
    int         hp_floor = 60;      ///< under this a round waits
    std::string held_by;            ///< somebody else's hold on everything
    bool        trash = false;      ///< a trash pass, or the bin left in doubt
    bool        chat = false;       ///< the chat line may be open
    bool        resting = false;    ///< the last round ended under a minute ago
    bool        user_active = false;    ///< somebody is at the mouse or keyboard
    /// The game is the window in front. A timed round waits until it is: the
    /// player may be busy in another window, and a round that brought the game
    /// forward would take their keys -- Tự chat's timed lines wait the same.
    bool        focused = false;
    /// Asked for with Làm ngay 1 lần, by a person at GoBot's window: that
    /// round may bring the game forward itself, as the trash tab's buttons do.
    bool        asked = false;
};

/// Whether a round may start, and if not why, in Vietnamese.
struct QuestGate {
    bool        go = false;
    std::string why;
};
QuestGate quest_gate(const QuestGateInput& in);

/// What a round is told as it is called.
struct QuestCall {
    /// Asked for with Làm ngay 1 lần, a moment ago: the game may be brought
    /// forward for it. A timed round never does that, nor one asked for that
    /// had to wait longer than kAskFocusMs -- by then the person who asked
    /// may be typing in another window, and would have their keys taken.
    bool asked = false;
    /// An earlier round opened the window and could not see it shut, and no
    /// look since has shown it shut: QuestRound::adopt_open_window.
    bool ours_open = false;
};

/// Quick quest for the length of a run, or for one round asked for: the
/// schedule, the gates, the failures in a row and the stop they lead to.
///
/// No threads and no clock: every call is told the time, and the rounds, the
/// gates and the alert come through hooks, so a test can run an hour of it in
/// a moment. A round that cannot start now waits half a minute; one asked for
/// with a button looks again every second, for a minute and a half at most.
///
/// It also remembers whether a window an earlier round opened may still be
/// open, for the next round to close: otherwise that round would take it for
/// the player's and leave it, and one that never shuts would never count as
/// trouble.
class QuestRun {
public:
    struct Hooks {
        /// What the rest of the bot is doing, now.
        std::function<QuestGateInput()> gates;
        /// One round. NotNow, with why, when it could not start after all,
        /// or had to give way part through.
        std::function<QuestResult(const QuestCall&)> round;
        /// GuiApp::alert.
        std::function<void(const std::string& event, const std::string& text)> alert;
        /// A line for the tab, in Vietnamese.
        std::function<void(const std::string& vi)> say;
        /// The time a round ended, on step()'s clock: a real round takes a few
        /// seconds, and the next is counted from its end. step()'s own time
        /// when not set.
        std::function<std::uint64_t()> now;
    };

    explicit QuestRun(std::uint32_t seed = std::random_device{}());
    void set_hooks(Hooks h) { hooks_ = std::move(h); }

    /// A run, from its beginning.
    void start(std::uint64_t now, const QuestSettings& s);
    void update(const QuestSettings& s);
    /// One round as soon as nothing is in the way, outside the timer.
    void ask_once(std::uint64_t now);
    bool asked() const { return asked_; }
    /// The keeper got back in, or the character was revived.
    void soon(std::uint64_t now);
    /// Every tenth of a second or so.
    void step(std::uint64_t now);

    bool running() const { return running_; }
    bool stopped_itself() const { return stopped_; }
    /// Why, in Vietnamese, for the tab.
    const std::string& stopped_why() const { return stopped_why_; }
    /// The same in English, for the log.
    const std::string& stopped_log() const { return stopped_log_; }
    /// When the next timed round is due; 0 for none.
    std::uint64_t next_at() const;
    bool has_last() const { return has_last_; }
    const QuestResult& last() const { return last_; }
    const QuestSchedule& schedule() const { return schedule_; }
    /// Whether a window a round opened may still be open. Carried from one run
    /// to the next by whoever keeps them -- a stop in the middle of a round
    /// ends the run with the window open.
    bool ours_open() const { return ours_open_; }
    void set_ours_open(bool yes) { ours_open_ = yes; }

    static constexpr std::uint64_t kPostponeMs = 30'000;
    static constexpr std::uint64_t kRestMs = 60'000;
    static constexpr std::uint64_t kSoonMs = 30'000;
    static constexpr std::uint64_t kAskMs = 90'000;
    /// How long after Làm ngay 1 lần the round may still bring the game
    /// forward. After that it waits for the game to be in front, as a timed
    /// round does.
    static constexpr std::uint64_t kAskFocusMs = 5'000;

private:
    void wait(std::uint64_t now, const std::string& why);
    void stop_itself(const std::string& why, const std::string& why_en);
    void say_now_and_then(std::uint64_t now, const std::string& why);

    Hooks         hooks_;
    QuestSettings settings_;
    QuestSchedule schedule_;
    bool          running_ = false;
    bool          stopped_ = false;
    std::string   stopped_why_;
    std::string   stopped_log_;
    bool          ours_open_ = false;
    bool          asked_ = false;
    std::uint64_t asked_until_ = 0;
    std::uint64_t asked_at_ = 0;
    std::uint64_t retry_at_ = 0;
    std::string   waiting_for_;
    std::uint64_t said_at_ = 0;
    std::string   said_;
    bool          has_last_ = false;
    QuestResult   last_;
};

} // namespace gobot
