#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace gobot {

/// Synthetic keyboard/mouse input aimed at the game window.
///
/// Uses SendInput with hardware scan codes rather than PostMessage: the game
/// reads input through DirectInput/raw-input, which ignores posted window
/// messages. That means input goes to whatever window has focus, so every
/// key that goes down is gated on the target window actually being
/// foreground -- a focus check is the difference between a key press landing
/// in the game and landing in the user's browser. A key-up is the exception:
/// one this manager owes, for a key it put down, goes out whatever is in
/// front, because a key left down in Windows stays down for every program
/// until something lets it go.
///
/// Every key, click, pointer move, posted character and change of focus asks
/// the "no real input" switch (gobot/no_real_input.hpp) and refuses while it
/// is on: as soon as there is a window to aim at, before anything is asked of
/// that window -- or first of all, for what goes to whatever is in front. One
/// with no window at all does nothing, as it always did, and is not refused.
class InputManager {
public:
    InputManager();
    ~InputManager();

    /// A candidate top-level window.
    struct WindowInfo {
        void*       hwnd = nullptr;
        std::string title;          ///< UTF-8, read without asking the window anything
        std::string window_class;   ///< UTF-8, likewise
        std::string process;        ///< executable file name, e.g. "game.exe"
        /// The full path of that program, UTF-8 -- what tells the game from
        /// another program of the same name (gobot/game_install.hpp). Empty
        /// when it cannot be read.
        std::string path;
    };

    /// Every visible top-level window of a usable size, excluding this
    /// process's own -- skipped by process before anything is read off them --
    /// and other copies of GoBot, by name.
    ///
    /// Untitled ones are in here too -- the game's launcher is one. Find those
    /// by class and process instead; see `find_game_window`.
    ///
    /// Title-substring matching alone is not safe to act on: an editor or
    /// browser tab named after the game matches just as well as the game, and
    /// picking wrong means typing combat keys into someone's document. The
    /// GUI shows this list and lets the user choose.
    static std::vector<WindowInfo> enumerate_windows();

    /// Locate the game window by its title, window class and/or program.
    /// Any may be empty; with all three empty nothing is found. Returns false
    /// if nothing matched.
    ///
    /// A title on its own must be the window's whole title (ASCII letters in
    /// any case): a part of it finds nothing, because an editor or browser tab
    /// named after the game contains it just as well. A program or a class
    /// lets a window in by itself; among the windows they let in, the whole
    /// title is preferred, then a title starting with the string, then one
    /// containing it. Never matches our own process.
    ///
    /// A program asked for by the game's name -- Game.exe -- is matched only
    /// when it is the game, by where it runs from (GameInstall), never by the
    /// name alone: plenty of other programs are called Game.exe.
    bool find_game_window(const std::string& title,
                          const std::string& window_class = {},
                          const std::string& process_name = {});

    /// Every visible top-level window belonging to one program, at any size.
    ///
    /// Separate from `enumerate_windows` because it answers a different
    /// question. That one offers windows a person might aim the bot at, so it
    /// throws away anything too small to be a game. This one is for finding
    /// out what a program is showing -- and when a program has crashed, what
    /// it is showing is a message box, which is small by definition.
    static std::vector<WindowInfo> windows_of(const std::string& process_name);

    /// A window's text, and its children's, joined with newlines: UTF-8, read
    /// without sending any window anything.
    ///
    /// A dialog's title says almost nothing ("Error"); what matters is the
    /// static control inside it, which is a child window with its own text.
    /// Reading both is how an error code gets read without a picture of it.
    static std::string window_text(void* hwnd);

    /// Whether Windows considers this window to have stopped pumping messages.
    static bool window_hung(void* hwnd);

    /// How a window gets named in a config file or on the command line.
    struct WindowSpec {
        std::string title;
        std::string window_class;
        std::string process;

        bool empty() const {
            return title.empty() && window_class.empty() && process.empty();
        }
    };

    /// Read a window spec out of one string.
    ///
    /// A plain string is a title -- the window's whole title, which is all
    /// find_game_window takes a title on its own for. But a window need not
    /// have a title -- the game's launcher does
    /// not, it is a bare #32770 dialog from GhostOnlineVTC.exe -- and the only
    /// way to name one of those is by its class and the program that put it
    /// up. So a spec may instead be comma-separated terms:
    ///
    ///   class=#32770,proc=GhostOnlineVTC.exe
    ///   title=VTCZone
    ///
    /// A string with none of those terms in it is taken whole as a title,
    /// commas included, because a window title may well contain one.
    static WindowSpec parse_spec(const std::string& text);

    bool find_game_window(const WindowSpec& spec) {
        return find_game_window(spec.title, spec.window_class, spec.process);
    }

    void  set_target_window(void* hwnd);
    void* target_window() const { return hwnd_.load(); }

    bool window_valid() const;
    bool window_focused() const;
    /// Bring the game to the front. Attaches to the input of the thread whose
    /// window is in front only when that window is another program's (see
    /// may_attach_to_foreground) and answers. Never waits for the game's own
    /// thread: one that has hung cannot hold the caller. True once the game
    /// is in front: looked for every 10 ms, kFocusMinMs at least and
    /// `wait_ms` at most -- kFocusMinMs when Windows refused outright. The
    /// UI thread, which reads F12 by polling, asks for the least. `accepted`,
    /// when given, says whether Windows took the request: false for no window
    /// to bring, a refusal, or a test forbidding real input; true when the game
    /// is in front, or Windows took the request and the game was not yet shown
    /// in front when the wait ran out -- a slow game comes forward by itself.
    bool focus_window(long long wait_ms = kFocusWaitMs, bool* accepted = nullptr);
    static constexpr long long kFocusMinMs = 50;
    static constexpr long long kFocusWaitMs = 300;
    /// Whether focus_window may attach this thread's input to the thread of
    /// the window in front: only when there is one, it is not this thread's
    /// and not this process's. For tests.
    static bool may_attach_to_foreground(std::uint32_t fg_thread, std::uint32_t fg_process,
                                         std::uint32_t our_thread, std::uint32_t our_process);

    /// Client-area size of the target window; false if unavailable.
    bool window_size(int& width, int& height) const;

    // ---- key primitives ----
    // A key goes down only while the game is in front. A key-up goes out with
    // the game in front, and whatever is in front when it is owed (send_key).
    // ↓ never goes down at all (never_pressed).
    bool key_down(std::uint32_t vk);
    bool key_up(std::uint32_t vk);
    /// Press and release, holding for a randomised human-ish duration.
    bool key_press(std::uint32_t vk, int hold_ms = -1);
    /// Whether one key event goes out, by the rule every key here keeps: down
    /// only into the game in front; up into the game in front, and wherever
    /// focus has gone when it is owed -- the key is down by this manager's
    /// doing, recorded as held or half way through a press of its own. For
    /// tests: in gobot_test the switch refuses every real key before this is
    /// asked.
    static bool key_event_allowed(bool down, bool focused, bool owed);

    bool is_held(std::uint32_t vk) const;

    /// Release every key this manager is currently holding. Called on stop and
    /// on focus loss so the character doesn't run off holding a direction.
    void release_all();

    // ---- movement ----
    bool move_left();
    bool move_right();
    bool stop_move();
    bool jump();
    bool double_jump();
    bool jump_down();       ///< drop through a platform
    bool face_left();
    bool face_right();

    bool moving_left() const  { return held(key_left_); }
    bool moving_right() const { return held(key_right_); }
    bool moving() const { return moving_left() || moving_right(); }

    // ---- actions ----
    bool use_skill(std::uint32_t vk);
    bool normal_attack();
    bool loot();

    /// Refuses, and says so: chat goes through ChatSender (gobot/auto_chat.hpp).
    ///
    /// This used to press Enter and type blind. Letters typed into a chat line
    /// that did not open are skill keys, and an Enter pressed on a dialog
    /// answers it, so a line is only ever typed once a look at the screen has
    /// shown it open -- which needs pictures and frames this class has not got.
    bool send_chat(const std::string& text, std::uint32_t chat_key = 13);

    // ---- typing into the game's chat line ----

    /// One key press for typing: `vk` down -- with Shift down before it when
    /// `shift` -- held `hold_ms`, and up again. Only while the game is in front.
    ///
    /// Not recorded among the keys held. The hunter calls release_all() every
    /// time it stops walking, from its own thread, and that let go of a Shift
    /// held for a capital between Shift going down and the letter. So Shift
    /// and the key go down in one SendInput call, which nothing can get
    /// between, and come up in another, sent whether or not the game is still
    /// in front, so that neither is left held.
    ///
    /// `how` is how each key goes: as its key code with its scan code (Vk),
    /// as typed keys always have; or as GoC's own client sends them, the scan
    /// code alone (Scancode: no key code, KEYEVENTF_SCANCODE) -- chat.ini's
    /// inject, for the live test of why the game took about half of the chat
    /// line's keys on 2026-10-20.
    enum class KeyInject { Vk, Scancode };
    bool type_key(std::uint32_t vk, bool shift, int hold_ms, KeyInject how = KeyInject::Vk);
    /// One event of type_key's, in SendInput's own terms: the key code, the
    /// scan code and the flags it is sent with. For tests.
    struct KeyEvent {
        std::uint16_t vk = 0;
        std::uint16_t scan = 0;
        std::uint32_t flags = 0;
    };
    static KeyEvent typed_key_event(std::uint32_t vk, bool down, KeyInject how);

    /// One UTF-16 unit as Unicode input (KEYEVENTF_UNICODE), down and up, only
    /// while the game is in front.
    bool type_unit(wchar_t unit);

    /// One UTF-16 unit posted to the game's window as WM_CHAR, or as
    /// WM_IME_CHAR when `ime`. Only while the game is in front: the chat line
    /// it is meant for was opened by a key, and is only open while it is.
    bool post_char(wchar_t unit, bool ime);

    // Nothing here pastes. The clipboard is the player's, and a paste the game
    // got round to after it was put back would post what the player had
    // copied -- a password, say -- to the chat.

    /// Whether Caps Lock is on.
    static bool caps_lock_on();

    // ---- pointer ----
    //
    // For the windows the game does not draw: the publisher's launcher, its
    // login page, its channel list. Those are ordinary desktop windows with
    // ordinary buttons, and clicking them is the only way through to the game.
    //
    // Coordinates are inside the target window's client area, like everything
    // else here, so a flow written against one position of the window still
    // works when it opens somewhere else.

    /// Move the pointer to a client-area point of the target window.
    bool move_pointer(int x, int y);

    /// Move there and click, checking nothing -- neither focus nor what is
    /// under the point. For the publisher's windows, which are being brought
    /// to the front, and a click is how they get there. A point of the game
    /// goes through click_on_game.
    bool click_at(int x, int y, bool right_button = false);

    /// How a click on the game went.
    enum class GameClick {
        Clicked,        ///< down and up went out on the game
        NoWindow,       ///< no game aimed at, or its window gone
        Refused,        ///< the "no real input" switch is on
        OffClient,      ///< the point is outside the client area
        OffScreen,      ///< the point is on no screen
        NotInFront,     ///< the game is not in front
        Covered,        ///< another window is over the game there
        Moved,          ///< the game's window moved while the pointer went there
        Stopped,        ///< the caller's stop said so
        Failed,         ///< Windows did not take the input
    };
    /// A few words for the log: "the game is not in front".
    static const char* game_click_name(GameClick c);

    /// Whether a click at client point `x`,`y` of `game` would land on the
    /// game: its window there and not minimised, the point inside the client
    /// area and on a screen, the game in front unless `need_front` is false,
    /// and the game itself under the point. Only asks the desktop. `why`,
    /// when given, is set only when it would not.
    static bool would_reach_game(void* game, int x, int y, GameClick* why = nullptr,
                                 bool need_front = true);

    /// Click a client-area point of the game, only where it lands on the game.
    ///
    /// would_reach_game is asked before the pointer moves and again after the
    /// pause on arrival, and `stop` before each; the press and the release
    /// carry the point. It brings nothing forward. `need_front` false is only
    /// for the bag's back-out, where the game on top at that point is enough.
    /// With the "no real input" switch on it is refused once, as soon as a
    /// window is aimed at; with no window it does nothing and refuses nothing.
    /// False unless the press and the release both went out; `why`, when
    /// given, says how it went. `presses` 2 is a double-click: a second press
    /// and release at the same point, well inside the double-click time, the
    /// game asked about again before it.
    bool click_on_game(int x, int y, bool right_button = false,
                       const std::function<bool()>& stop = {}, GameClick* why = nullptr,
                       bool need_front = true, int presses = 1);

    /// Press at one point, move to another with the button held, let go.
    ///
    /// How an item goes from a bag slot to the trash can. The pointer travels
    /// in steps rather than jumping: a game sees the drag only through the
    /// moves it is sent, and one that teleports from slot to target looks like
    /// a click in two places. Coordinates are client-area, like `click_at`.
    bool drag(int from_x, int from_y, int to_x, int to_y);

    /// drag(), only where it lands on the game, as click_on_game clicks: both
    /// points asked of would_reach_game before the pointer moves, the first
    /// again after the pause on arrival, and `stop` before each. The press
    /// carries the first point and the release the second. Once the button
    /// is down nothing stops it: it always ends with the release. Refused
    /// once with a window aimed at and the "no real input" switch on; with no
    /// window it does nothing and refuses nothing.
    bool drag_on_game(int from_x, int from_y, int to_x, int to_y,
                      const std::function<bool()>& stop = {}, GameClick* why = nullptr);

    /// Type a string into whatever has focus, one character at a time.
    ///
    /// Sent as Unicode rather than as key codes, so what arrives does not
    /// depend on the keyboard layout that happens to be active -- the same
    /// method `send_chat` already uses for the same reason.
    ///
    /// Not gated on the game window being foreground: the fields this types
    /// into belong to the publisher's launcher.
    bool type_text(const std::string& utf8);

    // ---- key bindings ----
    void set_left_key(std::uint32_t vk)  { key_left_ = vk; }
    void set_right_key(std::uint32_t vk) { key_right_ = vk; }
    void set_up_key(std::uint32_t vk)    { key_up_ = vk; }
    void set_down_key(std::uint32_t vk)  { key_down_ = vk; }
    void set_jump_key(std::uint32_t vk)  { key_jump_ = vk; }
    void set_attack_key(std::uint32_t vk){ key_attack_ = vk; }
    void set_loot_key(std::uint32_t vk)  { key_loot_ = vk; }

    std::uint32_t left_key() const  { return key_left_; }
    std::uint32_t right_key() const { return key_right_; }
    std::uint32_t jump_key() const  { return key_jump_; }

    /// Randomised inter-action delay, in ms, drawn from [min, max].
    void set_delay_range(int min_ms, int max_ms);
    int  random_delay() const;

    /// While one is alive on a thread, that thread's clicks and drags on the
    /// game (click_on_game, drag_on_game) pause as `press_ms` says -- the
    /// pauses as they are at 80, scaled, each with a floor under which the
    /// game is known to miss a press. 0 or 80: as ever. Per thread, so the
    /// hunter's, the revive's and every other worker's clicks keep their own
    /// pace: the bag jobs' "Tốc độ bấm nút" (CleanSettings::press_ms).
    class Pace {
    public:
        explicit Pace(int press_ms);
        ~Pace();
        Pace(const Pace&) = delete;
        Pace& operator=(const Pace&) = delete;
    private:
        int was_;
    };

    /// Hard gates used by higher layers (e.g. suspend combat during a dialog).
    void set_block_movement(bool b) { block_movement_ = b; if (b) stop_move(); }
    void set_block_attack(bool b)   { block_attack_ = b; }
    bool movement_blocked() const { return block_movement_.load(); }
    bool attack_blocked() const   { return block_attack_.load(); }

    /// Whether synthetic keystrokes reach this machine at all.
    ///
    /// Injects a key nobody uses and immediately asks Windows whether it is
    /// down. If its own keystroke does not come back, nothing this class sends
    /// is reaching anything, and every key is being discarded silently.
    ///
    /// The usual cause is not an anticheat but Windows' own integrity rule:
    /// input injected by a process cannot reach a window belonging to a more
    /// privileged one, and a game started from an elevated shell is exactly
    /// that. Running elevated fixes it. The two look identical from here,
    /// which is worth remembering before concluding anything.
    ///
    /// Takes about 60 ms. Cache it; do not call it in a loop.
    static bool injection_works();

    /// Map a VK to its scan code via the active keyboard layout.
    static std::uint16_t scan_code(std::uint32_t vk);

    /// A key GoBot never presses, whatever setting asks for it: ↓, which
    /// standing on a gate takes the character to another map -- the one key
    /// in this game that does (the player, 2026-11-01). A key box asked for
    /// it refuses it; one already set to it presses nothing. By the low byte,
    /// which is all of a key code that reaches the game: a file's 0x10028 is
    /// Down too.
    static bool never_pressed(std::uint32_t vk) { return (vk & 0xFFu) == 0x28u; }
    /// Parse a key as a person or a file writes it into a VK, 0 for none. One
    /// letter or digit is that key ("Z"; "1" is the 1 key, not key code 1); a
    /// name ("F10", "Space", "Numpad3"); or a key code, as 0x and hex ("0x5A")
    /// or as two or more decimal digits ("90"). Whatever key_name() writes
    /// reads back as the same key.
    static std::uint32_t parse_key(const std::string& text);
    static std::string   key_name(std::uint32_t vk);

private:
    /// One key event, under key_event_allowed's rule. `owed`: this key-up
    /// ends a press or tap begun here (key_press, face_left/right, jump_down),
    /// so it goes out even when another thread's release_all() crossed the key
    /// off the held list in between.
    bool send_key(std::uint32_t vk, bool down, bool owed = false);
    bool held(std::uint32_t vk) const;
    void sleep_jitter(int base_ms) const;
    /// Thread-safe uniform draw from [min_ms, max_ms]. Every use of rng_ goes
    /// through here or random_delay(); the generator is shared across threads.
    int  jitter(int min_ms, int max_ms) const;

    std::atomic<void*> hwnd_{nullptr};
    std::atomic<bool>  said_never_{false};     ///< a ↓ refused has been said in the log

    std::uint32_t key_left_   = 37;
    std::uint32_t key_right_  = 39;
    std::uint32_t key_up_     = 38;
    std::uint32_t key_down_   = 40;
    std::uint32_t key_jump_   = 17;
    std::uint32_t key_attack_ = 32;
    std::uint32_t key_loot_   = 90;

    std::atomic<bool> block_movement_{false};
    std::atomic<bool> block_attack_{false};

    mutable std::mutex                             held_mutex_;
    std::unordered_map<std::uint32_t, bool>        held_keys_;

    mutable std::mutex         rng_mutex_;
    mutable std::mt19937       rng_;
    int                        delay_min_ = 40;
    int                        delay_max_ = 90;
};

} // namespace gobot

