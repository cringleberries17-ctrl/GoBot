#pragma once

#include "gobot/bag.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace gobot {

/// Saying something in the game's chat every so often: what the player wrote
/// in the Tự chat tab, one line at a time, on a timer.
///
/// The commercial client does this, and the way it does it is copied here. It
/// opens the chat line with Enter, checks with a picture that the line really
/// is open, clears it, presses one key per character -- letters, digits and
/// spaces only -- presses Enter and checks the line closed again. Everything
/// else that presses keys is held off for the length of a line, and for as
/// long after as the line may still be open (ChatDoubt).
///
/// The part worth being careful about is that the chat line is the only place
/// a letter is a letter. Anywhere else the letters are skill keys, and Enter
/// answers whatever dialog is up -- a party invitation, or the trash bin's
/// question. So nothing here presses a key before a fresh look at the screen
/// shows the line in the state the key expects, and Enter goes only on a look
/// just taken that shows no dialog. A dialog that comes up in the fraction of
/// a second between that look and the key is the one thing no look can rule
/// out.

/// How a line gets into the chat line once it is open.
///
/// Vk is what the commercial client does, and the default: one key press per
/// character, as a key code with its scan code, which is the way the game is
/// known to take keys. It can only type what has a key of its own -- A to Z,
/// 0 to 9 and space -- so Vietnamese is typed without its marks. The others
/// are here so that the live test can find out whether the game takes text
/// another way: the game ignored Unicode input in the trash bin's box, but a
/// chat line is a different kind of box. They are chosen in configs/chat.ini.
///
/// There is no way through the clipboard. One was tried, and a paste the game
/// got round to late would post whatever the player had copied -- a password,
/// say -- to the channel; nothing of GoBot's reads or writes the clipboard.
enum class ChatMethod {
    Vk,         ///< one key press per character, key code and scan code
    Unicode,    ///< SendInput with KEYEVENTF_UNICODE, one UTF-16 unit at a time
    WmChar,     ///< WM_CHAR posted to the game's window, one unit at a time
    ImeChar,    ///< WM_IME_CHAR posted to the game's window, one unit at a time
};

/// "vk", "unicode", "wmchar", "imechar".
const char* chat_method_name(ChatMethod m);
/// The other way round, case and blanks ignored; false for anything else --
/// "paste" among them.
bool chat_method_from(const std::string& text, ChatMethod& out);

/// configs/chat.ini.
struct ChatSettings {
    bool        enabled = false;
    /// Seconds from one line to the next, before the jitter.
    int         interval_s = 120;
    /// The lines in turn, or one at random -- never the same one twice running.
    bool        random_order = false;
    /// How far each interval may stray either way, as a percentage, so that
    /// lines do not go out on a clock somebody could set a watch by.
    int         jitter_pct = 15;
    /// Seconds from the start to the first line. The commercial client sends
    /// at once; a moment's grace lets the player see the bot start fighting
    /// before it starts typing.
    int         first_delay_s = 15;
    /// Characters a line may have; longer ones are cut.
    int         max_length = 60;
    /// Take the marks off Vietnamese letters for the other methods too. Vk
    /// always does, having no keys for them; the others are there to find out
    /// whether the game takes the marks, so they keep them unless told not to.
    bool        strip_diacritics = false;
    ChatMethod  method = ChatMethod::Vk;
    /// Why `method` is vk when the file asks for something else -- a method
    /// no longer offered, paste, or no method at all -- in Vietnamese, for
    /// the tab and the command line. Empty when the file's method was taken.
    std::string method_note;
    /// Backspaces pressed once the line is open, before typing: whatever was
    /// left in it goes. The commercial client presses six.
    int         clear_backspaces = 6;
    /// The chat bar, open: the commercial client's own picture of it, which is
    /// what its check goes by. Its layout -- the divider between the two boxes,
    /// the bar's left edge and the field -- is measured from that picture, so
    /// another picture has to be laid out the same way.
    std::string open_picture = "inven/0/dialog_chat_res3.spr/0";
    /// The field as it looks while the line is shut: light grey, empty.
    std::string closed_picture = "inven/0/dialog_chat_res3.spr/1";
    /// How alike the field has to be to `closed_picture` for the line to count
    /// as shut. It is 0.988 on every frame saved from the live client.
    float       closed_score = 0.90f;
    /// How far from the field's left edge, in pixels, an open line with
    /// nothing in it shows something drawn: its caret. Enter, which sends
    /// whatever is in the line, is only pressed to close a line that shows no
    /// more than this -- or no more than it showed just after it was cleared.
    /// Not measured on the live client yet; gobot_cli --chatcheck with the
    /// line open and empty prints it as ink=.
    int         empty_ink = 12;
    /// Lines that failed one after another before Tự chat stops itself.
    int         max_failures = 3;
    /// What gobot_cli --chat-once types: something short and plain, for the
    /// live test, set here so that the test cannot be handed other words.
    std::string test_line = "test";
    /// What to say, as written, in order. Empty lines are not kept.
    std::vector<std::string> lines;
    /// For each line, whether it is ticked "Chỉ khi chưa có tổ đội": not said
    /// while the player is in a party -- GoC's "Party Request: only send when
    /// NOT in a party", for the advert that asks people to join. As long as
    /// `lines`, from [Lines] lineN_only_alone=true beside lineN; a line without
    /// that key is not ticked.
    std::vector<bool> only_alone;
    /// Whether line `i` is ticked so; false for a line that is not there.
    bool alone_only(int i) const {
        return i >= 0 && static_cast<std::size_t>(i) < only_alone.size() && only_alone[static_cast<std::size_t>(i)];
    }

    static constexpr int kMinInterval = 30;     ///< anything shorter is spam
    static constexpr int kMaxInterval = 3600;
    static constexpr int kMaxLines = 10;
    static constexpr int kMaxJitter = 30;
    /// The most max_length may be set to. The game's own limit is not known
    /// yet; the live test measures it.
    static constexpr int kLongest = 120;

    /// False, and nothing changed, when the file cannot be read or has no
    /// [Chat] section. Values out of range are brought into it. Sections and
    /// keys are read in any case, numbers in base ten whatever their leading
    /// zeros, a decimal comma as a point, and a BOM in front of the first line
    /// does not hide it -- nor does Notepad's "Unicode" (UTF-16): the file is
    /// edited by hand too. A method it does not offer is typed with vk, and
    /// logged once.
    bool load(const std::string& path);

    /// Write one key of [Chat], leaving the rest of the file as it is -- its
    /// comments, its BOM, its line ends and its encoding. The file is made,
    /// off, if it is not there. Like save_lines, in one move over the old file.
    static bool save_key(const std::string& path, const std::string& key,
                         const std::string& value);
    /// Put these lines in [Lines], in place of what was there, in one write:
    /// the worker reads the file while the tab writes it, and must never find
    /// half a list. None of them ticked "Chỉ khi chưa có tổ đội".
    static bool save_lines(const std::string& path, const std::vector<std::string>& lines);
    /// The same, with each line's tick from `only_alone` -- lineN_only_alone=true
    /// beside each ticked lineN, in the same write, so a tick never lands on
    /// another line. A line with no tick given is not ticked.
    static bool save_lines(const std::string& path, const std::vector<std::string>& lines,
                           const std::vector<bool>& only_alone);
    /// The ticks for `lines`, as the box was edited from `before` and its
    /// ticks: a line whose words are the same keeps its tick wherever it moved;
    /// a new line, or one whose words changed, starts unticked. Two lines of
    /// the same words take that text's ticks in turn, its own place first.
    static std::vector<bool> carry_ticks(const std::vector<std::string>& before,
                                         const std::vector<bool>& before_ticks,
                                         const std::vector<std::string>& lines);

    /// A box's text, one line each, as the lines to keep: trimmed, the empty
    /// ones left out, at most kMaxLines.
    static std::vector<std::string> split_lines(const std::string& text);
    /// The other way, for the box.
    static std::string join_lines(const std::vector<std::string>& lines);

    /// The interval as it is used: within kMinInterval and kMaxInterval.
    static int clamp_interval(int seconds);
};

/// A line as it will be typed, and what had to be done to it to get there.
struct ChatText {
    std::string typed;          ///< UTF-8, what goes into the game
    int         chars = 0;      ///< characters in it
    bool        stripped = false;   ///< marks taken off letters
    bool        dropped = false;    ///< characters left out, having no key
    bool        cut = false;        ///< shortened to the longest a line may be
    bool empty() const { return typed.empty(); }
    bool changed() const { return stripped || dropped || cut; }
};

/// What will be typed for `line`. Composed first (NFC), so a line pasted with
/// its marks as separate characters comes out the same as one typed; then,
/// for Vk or when `strip` is set, the marks are taken off -- "đồ" becomes
/// "do" -- and for Vk anything but A-Z, a-z, 0-9 and space is left out, as
/// the commercial client leaves it out. Tabs and line breaks become spaces,
/// runs of spaces one, the ends are trimmed and the line is cut to
/// `max_length` characters. Empty when nothing typeable is left.
ChatText prepare_chat(const std::string& line, ChatMethod method, bool strip, int max_length);

/// One key press of a line typed as key codes.
struct ChatKey {
    std::uint32_t vk = 0;
    bool          shift = false;
};

/// The keys for an ASCII line from prepare_chat: a letter's key with Shift
/// held when its case is not what Caps Lock gives, a digit's key and Space,
/// never with Shift. Anything else is left out.
std::vector<ChatKey> chat_keys(const std::string& ascii, bool caps_lock);

/// When the next line goes, and which.
///
/// One line per interval, in turn or at random. A line put off -- the game
/// was busy, a dialog was up -- keeps its turn; so does one that failed,
/// tried again after a pause. The next interval counts from the end of the
/// last attempt, so a slow line does not bring the one after it closer.
class ChatSchedule {
public:
    explicit ChatSchedule(std::uint32_t seed = std::random_device{}());

    /// From the beginning: the first line first_delay_s from `now`, nothing
    /// sent, no failures.
    void start(std::uint64_t now, const ChatSettings& s);
    /// The settings changed while running. The turn is kept, within however
    /// many lines there are now; a shorter interval applies to the line
    /// already waiting.
    void update(const ChatSettings& s);

    bool          due(std::uint64_t now) const { return count_ > 0 && now >= due_; }
    std::uint64_t due_at() const { return due_; }
    /// The line whose turn it is, from 0; -1 with no lines.
    int           turn() const { return count_ > 0 ? turn_ : -1; }

    /// It went: the next line's turn, an interval (with its jitter) from now.
    void sent(std::uint64_t now);
    /// It did not: its turn kept, tried again `backoff_ms` from now.
    void failed(std::uint64_t now, std::uint64_t backoff_ms);
    /// Not now: its turn kept, tried again `ms` from now. Not a failure.
    void postpone(std::uint64_t now, std::uint64_t ms);
    /// Nothing in it can be typed: the next line's turn, a moment from now.
    void skip(std::uint64_t now);

    int sent_count() const { return sent_; }
    int failures() const { return failures_; }

    /// The gap the next sent line will be followed by: the interval, give or
    /// take the jitter, never under kMinInterval. Public for testing.
    std::uint64_t draw_gap_ms();

private:
    int  after(int index);

    std::mt19937  rng_;
    int           count_ = 0;
    bool          random_ = false;
    int           interval_s_ = 120;
    int           jitter_pct_ = 15;
    int           turn_ = 0;
    std::uint64_t due_ = 0;
    std::uint64_t last_end_ = 0;
    int           sent_ = 0;
    int           failures_ = 0;
};

/// What Tự chat does with the line whose turn it is when that line is ticked
/// "Chỉ khi chưa có tổ đội" (ChatSettings::only_alone) -- GoC's party-request
/// lines, which its queue passes over in a party ("Skipping party-request
/// message #N (already in party)") and goes on to the next.
enum class AloneTurn {
    Send,       ///< not ticked, or the party HUD showed the player alone: as any line
    Skip,       ///< in a party, or the HUD unread: the next line's turn
    Wait,       ///< and no other line could go instead: this one again a minute on
};
/// `alone`: the party HUD showed the player alone. A party seen is not alone,
/// and neither is a HUD that could not be read: a missed advert is better
/// than one sent in a party. `typeable[i]`: line i has something left to type
/// once prepare_chat is done with it; one with nothing is passed over anyway,
/// and is no line to go instead.
AloneTurn alone_turn(const std::vector<bool>& only_alone, const std::vector<bool>& typeable, int index,
                     bool alone);
/// How long a ticked line waits when every line that could go is ticked and
/// the player is in a party, so the HUD is not read every second.
constexpr std::uint64_t kAloneWaitMs = 60'000;

/// What the chat bar shows.
enum class ChatBarState { Unknown, Closed, Open };
/// In Vietnamese, for the screen: "đang đóng", "đang mở", "không thấy".
const char* chat_bar_name(ChatBarState s);

/// The chat bar, as found in one frame.
///
/// Found the way the commercial client finds it, read out of its own code: in
/// the bottom-left 401 by 145 pixels of the screen, a strip of the picture of
/// the open bar -- the divider between its two boxes -- and 77 pixels left of
/// that another strip, the bar's left edge, both in the picture's own colours.
/// Those two look the same whether the line is open or not. What tells them
/// apart is the field to the right of the divider: while the line is open it
/// is the dark grey of the picture, 49,49,49, and while it is shut it is light
/// grey -- the closed picture, which every frame saved from the live client
/// shows, matched at 0.988.
struct ChatBar {
    ChatBarState state = ChatBarState::Unknown;
    bool       found = false;       ///< the bar's two strips are there
    int        x = 0, y = 0;        ///< the divider strip's top-left, in the frame
    ScreenRect field;               ///< the inside of the field, where text goes
    int        dark = 0;            ///< pixels of the open field's own grey in it
    /// How alike the field is to the closed picture; -1 when not looked for.
    float      closed_score = -1.0f;
    /// How far from its left edge something other than that grey is drawn in
    /// the field -- the caret and whatever has been typed -- in pixels; only
    /// meaningful while the line is open.
    int        ink = 0;
    std::string trouble;            ///< why it could not be looked for at all
};

/// Whether `store` has the pictures to find the bar with.
bool can_look_for_chat(const TemplateStore& store, const ChatSettings& s);
ChatBar look_chat_bar(const Frame& f, const TemplateStore& store, const ChatSettings& s);

/// What counts as a dialog for a chat line, which must never get an Enter:
/// any dialog with the two buttons -- the bin's among them -- a party
/// invitation, the death dialog, and the bin holding an item, whose dialog is
/// one click away. Read from the files the features that answer them read.
struct ChatDialogs {
    BagLayout             bag;              ///< where the bin is, from trash.ini
    PartyInvite::Settings invite;           ///< the invitation's picture, from party.ini
    std::string           death = "hoisinh_confirm";   ///< the revive routine's first picture
    float                 death_score = 0.80f;

    /// From configs/trash.ini, party.ini and routines.ini. A file that is not
    /// there leaves what it would have set at its default.
    void load(const std::string& trash_ini, const std::string& party_ini,
              const std::string& routines_ini);
    /// The dialog up on `f`, by name, in Vietnamese; empty when none. Its
    /// four looks -- the two buttons, the invitation, the death dialog, the
    /// bin -- run side by side: one after another they take about a quarter
    /// of a second, and the frame the next Enter goes on is that much older.
    std::string up(const Frame& f, const TemplateStore& store) const;
    /// Whether `store` has the pictures to see the two buttons by. Without
    /// them there is no knowing whether a dialog is up.
    static bool can_check(const TemplateStore& store);
};

/// Everything that decides whether a line may be started now.
struct ChatGateInput {
    bool        window = false;     ///< the game's window exists
    bool        focused = false;    ///< and it is in front
    bool        keeper = false;     ///< the keeper is logging back in
    std::string held_by;            ///< the reason for somebody else's hold
    bool        trash = false;      ///< a trash pass, its hold, or a stop's doubt
    bool        user_active = false;    ///< the mouse or keyboard was used just now

    /// The rest comes from a look at the screen. Without one only the above
    /// is judged; with one, this is what it showed.
    bool         looked = false;
    bool         hud = false;       ///< the HP and MP bars are on screen
    float        hp_pct = -1.0f;    ///< -1 when there is no reading
    int          hp_floor = 50;     ///< under this a line waits
    std::string  dialog;            ///< the dialog that is up, or empty
    ChatBarState bar = ChatBarState::Unknown;
};

/// Whether a line may be started, and if not, when to look again and why --
/// in Vietnamese, for the tab.
struct ChatGate {
    bool          send = false;
    std::uint64_t wait_ms = 0;
    std::string   why;
};
ChatGate chat_gate(const ChatGateInput& in);

/// One line into the game's chat: open it, check, type, check, send, check.
///
/// Every Enter is pressed only on a fresh look at the screen that shows no
/// dialog, and the first only on one that shows the line shut. Typing starts
/// only once the line is seen open and cleared, every key goes only on a look
/// taken just before it that still shows the line open, and the line is sent
/// only when the field shows that what was typed arrived. Anything
/// unexpected, a stop, or the game losing the front, and what was typed is
/// rubbed out and the line closed again empty, rather than sent half-done or
/// left open for the skill keys to be typed into. A line is only closed with
/// Enter while the field shows nothing in it but the caret: Enter sends
/// whatever is there. Backspace goes only into a line a look has just shown
/// open: with it shut, or out of sight, it is the game's.
///
/// The line is believed shut only on two looks, and after an Enter that may
/// have opened it, only on a look taken long enough after it for the game to
/// have shown it. Whenever the sender cannot show the line shut and empty, it
/// says so (Result::left_open), and whoever runs it keeps everything else
/// that presses keys held off until a look does: see ChatDoubt.
class ChatSender {
public:
    using Grab = std::function<bool(Frame&)>;
    using Report = std::function<void(const std::string&)>;
    /// The dialog up on this frame, by name, in Vietnamese; empty when none.
    using DialogCheck = std::function<std::string(const Frame&)>;
    /// Asked between keys: a reason to stop the line now, or empty.
    using Guard = std::function<std::string()>;
    /// Asked once, just before the first Enter: a reason not to start the
    /// line after all -- somebody touched the mouse or keyboard since the
    /// screen was last judged -- or empty.
    using Ready = std::function<std::string()>;

    ChatSender(InputManager& input, const TemplateStore& store, Grab grab);

    void set_settings(const ChatSettings& s) { settings_ = s; }
    void set_dialog_check(DialogCheck d) { dialog_ = std::move(d); }
    void set_guard(Guard g) { guard_ = std::move(g); }
    void set_ready_check(Ready r) { ready_ = std::move(r); }
    void on_report(Report r) { report_ = std::move(r); }
    /// What an empty line showed when another sender cleared it (its
    /// Result::empty_ink), for closing that line later. -1 for not known.
    void set_empty_ink(int px) { empty_ink_ = px; }
    /// Where to keep the screen when a look goes wrong, as the trash cleaner
    /// keeps its; the newest thirty. Empty keeps none.
    void set_debug_dir(const std::string& dir) { debug_dir_ = dir; }
    /// When everything else was held off for this line, on the hands' clock
    /// (now_ms() unless a test's own). The line's report of how long it took
    /// then says how long the potions waited, which starts before the line.
    void set_held_since(std::uint64_t ms) { held_since_ = ms; }

    /// Stop the line at the next key, rub out what was typed and close the
    /// line. A sender that is cancelled stays cancelled.
    void cancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_.load(); }

    /// What the sender does to the game, one function each. The game's own
    /// input unless set; a test sets them to a pretend game.
    struct Hands {
        /// One key press: down, held `hold_ms`, up, with Shift around it.
        std::function<bool(std::uint32_t vk, bool shift, int hold_ms)> key;
        /// Text by one of the other methods: one UTF-16 unit at a time.
        std::function<bool(const std::wstring& text, ChatMethod how)> text;
        std::function<bool()> focused;
        std::function<bool()> caps_lock;
        /// Waiting, in milliseconds. A test makes it instant.
        std::function<void(int ms)> sleep;
        /// Milliseconds on a steady clock: now_ms() unless set. A test's
        /// moves with its sleep.
        std::function<std::uint64_t()> now;
    };
    void set_hands(Hands h) { hands_ = std::move(h); }

    struct Result {
        bool        sent = false;
        /// Nothing was pressed: the screen was not ready for the first Enter.
        /// `wait_ms` says when to look again, `trouble` why.
        bool        not_now = false;
        /// Stopped part way for something that is not the line's own trouble:
        /// HP falling, a dialog coming up, another hold, the game losing the
        /// front, the bar going out of sight, a stop. Whatever was typed has
        /// been rubbed out. To be tried again `wait_ms` from now, and not
        /// counted as a failure: in a long fight these come one after another.
        bool        aborted = false;
        std::uint64_t wait_ms = 0;
        bool        opened = false;     ///< the line was seen open
        int         typed = 0;          ///< characters whose keys went
        /// A dialog came up while the line was open. What was typed has been
        /// rubbed out and no Enter pressed; the line may still be open.
        std::string dialog;
        /// The line may still be open, with nothing in it or with what could
        /// not be rubbed out -- or may yet open, from an Enter the game was
        /// slow to take, or is out of sight. Whatever runs the sender keeps
        /// everything that presses keys held off until a look shows it shut.
        bool        left_open = false;
        /// Stop Tự chat, a person should look: the line would not close, or
        /// did not look like either picture after an Enter.
        bool        stop = false;
        /// What the field showed once cleared, before typing: an empty line's
        /// caret. -1 when not measured, or when it showed more than
        /// ChatSettings::empty_ink. For set_empty_ink.
        int         empty_ink = -1;
        std::string trouble;            ///< why it was not sent, in Vietnamese
        ChatBar     last;               ///< the last look at the bar
    };

    /// Say `line`. It has to have come from prepare_chat with this sender's
    /// method.
    Result send(const ChatText& line);

    /// Rub out `rub_out` characters and close a line left open, unless a
    /// dialog is up. True when the line is seen shut afterwards. Whatever the
    /// line still shows past an empty line's caret is rubbed out too -- a few
    /// rounds at most -- and the line is left open rather than sent with it.
    bool close_left_open(int rub_out);

    /// What a line left in doubt shows on a fresh look: shut (on two looks),
    /// open with nothing in the way of closing it, open with a dialog up or
    /// the game not in front, or not to be seen. Presses nothing.
    enum class LeftOpen { Shut, Closable, Busy, Unseen };
    /// `why` gets the reason it is not Shut or Closable, in Vietnamese.
    LeftOpen check_left_open(std::string* why = nullptr);

    /// One look at the bar.
    ChatBar look();

    /// Keep `f` in the debug folder as <time>_<n>_<what>.png, and log the
    /// numbers `bar` was judged by, when given.
    void keep(const Frame& f, const char* what, const ChatBar* bar = nullptr);

private:
    bool key(std::uint32_t vk, bool shift, int hold_ms);
    /// Enter, with when it went and whether it was pressed to open the line
    /// -- on a line not seen open -- for believe_shut.
    bool enter(bool opening, int hold_ms);
    bool text(const std::wstring& t, ChatMethod how);
    bool focused();
    bool caps_lock();
    std::uint64_t clock_ms();
    /// Waits, a little at a time; false when cancelled meanwhile.
    bool wait(int ms);
    /// Waits whether cancelled or not: for rubbing out and closing, which a
    /// stop needs done.
    void pause(int ms);
    int  jitter(int lo, int hi);

    /// A fresh look: the bar and the dialog on one frame. False when the
    /// screen could not be captured.
    bool look_both(ChatBar& bar, std::string& dialog, Frame* keep = nullptr);
    /// A fresh look at the bar alone, which is quick -- the dialogs are what
    /// takes the time. Into `into` when given, to keep and to save copying.
    bool look_bar(ChatBar& bar, Frame* into = nullptr);
    /// `bar`, a look just taken, shows the line shut: whether to believe it.
    /// Only if one look more agrees -- a single frame has been seen showing
    /// the line shut while it was not -- and, after an Enter that may have
    /// opened the line, only on a look taken long enough after it for the
    /// game to have shown it. `bar` ends as the last look.
    bool believe_shut(ChatBar& bar);
    /// Backspace `n` times, quickly; false when the game lost the front.
    bool rub_out(int n);
    /// Whether the open line shows nothing in it: nothing drawn at all, or no
    /// more than the caret did once it was cleared.
    bool shows_empty(const ChatBar& bar) const;
    /// After a stop, a loss of focus or anything unexpected: rub out `rub`
    /// characters -- only into a line a look has just shown open -- and close
    /// the line with Enter if it is open, empty and no dialog is up. Sets
    /// `r.left_open` when it could not show it shut.
    void tidy(Result& r, int rub);
    void say(const std::string& line) const { if (report_) report_(line); }

    InputManager&        input_;
    const TemplateStore& store_;
    Grab                 grab_;
    ChatSettings         settings_;
    DialogCheck          dialog_;
    Guard                guard_;
    Ready                ready_;
    Report               report_;
    std::string          debug_dir_;
    Hands                hands_;
    std::atomic<bool>    cancelled_{false};
    std::mt19937         rng_{ std::random_device{}() };
    /// How far the field showed something drawn on it once cleared, before
    /// typing: an empty line's caret. Only kept when it is no more than
    /// ChatSettings::empty_ink. -1 until measured.
    int                  empty_ink_ = -1;
    bool                 entered_ = false;      ///< an Enter went
    bool                 opening_enter_ = false;    ///< the last, on a line not seen open
    std::uint64_t        enter_ms_ = 0;         ///< when, on clock_ms()
    std::uint64_t        held_since_ = 0;       ///< see set_held_since; 0 for not given
};

/// A chat line that may have been left open, held on to until it is seen shut.
///
/// With the line open, every key the bot presses goes into it -- a skill key
/// is a letter, a potion a digit -- and an Enter would send them. So whenever
/// a line's sender cannot show the line shut and empty (Result::left_open),
/// everything else that presses keys stays held off, for as long as it takes,
/// until a look shows the line shut. Meanwhile a line seen open, with no
/// dialog up, the game in front and nobody at the mouse or keyboard, is
/// closed: what is in it rubbed out, never sent. One out of sight is waited
/// on, however long -- there is no knowing what it holds. It ends too when
/// the game the line was open in has gone, and with it the line.
///
/// This decides; whoever runs it holds and lets go, through the hooks.
class ChatDoubt {
public:
    struct Hooks {
        /// Everything else that presses keys held off. Asked on every step,
        /// so that a hold somebody else ended meanwhile is taken back.
        std::function<void()> hold;
        /// Let go, once, the doubt over; with why, in Vietnamese.
        std::function<void(const std::string& why)> release;
        /// The game the line was open in is still the one there.
        std::function<bool()> same_game;
        /// Runs `close` -- which rubs the line out and shuts it -- once the
        /// mouse is free and nobody has touched it or the keyboard for a
        /// while; false, without running it, otherwise.
        std::function<bool(const std::function<bool()>& close)> when_quiet;
        /// Why it is still held, in Vietnamese: said each step it is.
        std::function<void(const std::string&)> say;
    };

    ChatDoubt() = default;
    explicit ChatDoubt(Hooks hooks) : hooks_(std::move(hooks)) {}
    void set_hooks(Hooks hooks) { hooks_ = std::move(hooks); }

    /// A line's result: the doubt begins, or goes on, when it says the line
    /// may be open; `rub` is what the line may hold that was put there by
    /// it. First looked at a second from `now`, by when an Enter it pressed
    /// has long had time to show.
    void note(const ChatSender::Result& r, int rub, std::uint64_t now);
    bool active() const { return active_; }
    /// Somebody else's from here -- whoever stopped Tự chat -- with nothing
    /// let go and nothing pressed.
    void hand_over() { active_ = false; }
    /// While active, about every 100 ms: keeps the hold, and once a second
    /// looks with `sender`, and closes the line with it when it can. True
    /// when this step ended the doubt.
    bool step(ChatSender& sender, std::uint64_t now);

private:
    void end(const std::string& why);

    Hooks         hooks_;
    bool          active_ = false;
    std::uint64_t next_ms_ = 0;
    int           empty_ink_ = -1;  ///< what the line showed once cleared, if measured
    int           rub_ = 0;
};

/// Programs that turn typed letters into Vietnamese -- Unikey, EVKey, OpenKey
/// -- running now, by their executables' names. Their Vietnamese mode turns
/// "dd" into "đ" in whatever is typed, the bot's keys included.
std::vector<std::string> vietnamese_typers_running();

/// The keyboard layout of the thread that owns `hwnd`, low word only:
/// 0x0409 is English (US). 0 when it cannot be told.
std::uint16_t window_keyboard_layout(void* hwnd);

} // namespace gobot
