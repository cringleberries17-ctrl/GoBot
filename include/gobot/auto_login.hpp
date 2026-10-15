#pragma once

#include "gobot/config.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace gobot {

/// Gets from a cold machine to a character standing in the world.
///
/// Between the two there are seven screens, none of them the game's: the
/// publisher's launcher with its list of games and its start button, then the
/// client's own launcher and its start button, a connect button, a channel
/// list, another connect button, and a character to pick. A bot that cannot
/// cross them is a bot that needs a person every time the client falls over,
/// which is the thing an unattended run cannot have.
///
/// No account and no password. The publisher's launcher, VTCZone, keeps
/// itself signed in -- it shows its list of accounts, each with a "Chơi game"
/// -- so the crossing starts at its list of games. That is where the
/// commercial client GoC starts too: it has no setting for an account, no
/// picture of a sign-in form and no code that types into one. If VTCZone is
/// showing its sign-in form anyway, the flow stops and says so.
/// Signing in is a person's job, done once with VTCZone's own "remember me",
/// and doing it here would mean keeping a password around to type.
///
/// The sequence is fixed and is written down below as a table, because it is
/// one. Which picture identifies each page belongs to the launchers and the
/// client, not to this program, so the names live in configs/login.ini; the
/// table carries the ones that ship, so a file that is missing or old still
/// crosses, and a page whose picture is not loaded says exactly that rather
/// than failing in some less helpful way.
///
/// Identifying one again, after the publisher changes a page, is a minute's
/// work: get the launcher onto the page, run
/// `gobot_cli --window <launcher> --tplsrc <set> --survey`, and write down
/// what it found.
///
/// Some things are deliberately not done here.
///
/// A security prompt is left alone. The game's own launcher can put up a
/// "Security Alert" box, which is Windows asking whether to trust a site's
/// certificate. GoC presses Yes on it. This stops and says so instead:
/// whether to trust a certificate is not a question a bot gets to answer for
/// somebody.
///
/// Nothing is retried forever. Three attempts, then it stops and asks for a
/// person. A login loop that keeps hammering a server which is refusing it is
/// indistinguishable from an attack on that server, and would be treated as
/// one. Something that a retry cannot get past -- the sign-in form, a missing
/// picture, a launcher that is not where the file says -- stops it at once.
///
/// And nothing is clicked that would not land where it is meant to. A page is
/// found in a picture the window draws of itself, which it draws just the
/// same with something over it, while a click goes to whatever is under the
/// pointer. So before each click the window is brought to the front and the
/// point is checked to be on it; when another window is there instead -- the
/// player's browser, GoBot's own -- the click is held back and the page
/// looked at again, and after half a minute of that the attempt is given up.
/// A point that is not on the window at all is an offset written wrong in
/// configs/login.ini, and that stops the crossing at once and says which.
///
/// Nor is anything of VTCZone's pressed while a game is open. "Chơi game"
/// pressed under a game that is part way in -- at Kết Nối, the channel list,
/// the character select -- puts a second launcher up for the same account,
/// the thing GoC never does: it does not go back to VTCZone while a client
/// is open. So each attempt looks at the game and the game's launcher first,
/// carries on from the furthest page of theirs that is up, and ends at once
/// when the character is already in the world. A game that stays on a screen
/// the crossing does not know is ended, as GoC's KILL_GAME_PROCESS ends one,
/// by whoever owns ending clients -- see `set_end_client` -- and the next
/// attempt starts from VTCZone.
class AutoLogin {
public:
    /// What the flow is doing, in the order it does it.
    enum class Step {
        Idle,
        StartLauncher,      ///< run the publisher's launcher
        FindLauncher,       ///< wait for its window and bring it up
        GameList,           ///< pick the game out of the account's list
        StartGame,          ///< the launcher's start button
        ClientLauncher,     ///< the client's own small launcher window
        GameWindow,         ///< wait for the game's window to exist
        Connect,            ///< "Kết Nối"
        Channel,            ///< pick a channel
        ConnectAgain,       ///< "Kết Nối" once more
        Character,          ///< pick the character
        EndClient,          ///< end a game stuck on a screen nobody knows
        InWorld,            ///< the HUD is up: done
        Blocked,            ///< a person is needed
    };
    static const char* step_name(Step s);

    /// The same, in Vietnamese, for the simple screen: what a player would
    /// call each step.
    static const char* step_name_vi(Step s);

    /// Why it stopped, as something the simple screen can put into words.
    /// `trouble()` says the same in English, for the log.
    enum class Why {
        None,
        Cancelled,
        NotSetUp,           ///< a page has no picture named for it
        NoPicture,          ///< a page's picture is named and not loaded
        NoLauncher,         ///< no launcher written down
        LauncherMissing,    ///< VTCZone is closed and is not where the file says
        LauncherFailed,     ///< VTCZone would not start
        LauncherNoWindow,   ///< VTCZone started, and its window never came
        SignIn,             ///< VTCZone is showing its sign-in form
        GamePage,           ///< VTCZone never showed the game or "Chơi game"
        ClientStart,        ///< the game's launcher never showed START
        SecurityAlert,      ///< the game's launcher put up a Security Alert box
        GameWindow,         ///< START was pressed and the game never opened
        Connect,            ///< no "Kết Nối"
        Channel,            ///< no channel list
        Character,          ///< no character select
        World,              ///< a character was picked and never arrived
        Covered,            ///< another window stayed over the place to click
        BadPoint,           ///< a place to click is off its window: an offset in login.ini
    };

    /// Whether another attempt could get past it. The ones that cannot -- the
    /// sign-in form, a missing picture, a prompt that is a person's to answer,
    /// an offset written wrong -- stop the flow at once rather than three
    /// times over.
    static bool worth_retrying(Why why);

    /// The Vietnamese for why it stopped, for the simple screen. `detail` is
    /// what the message names: the launcher's path, a picture, a page, a
    /// number of seconds, or the program whose window was in the way,
    /// depending on `why`. For BadPoint it is the page's key in
    /// configs/login.ini, and the message says which [Page.*] to put right.
    static std::string advice(Why why, const std::string& detail = {});

    /// The same after every attempt was used: "Đã thử 3 lần không vào lại
    /// được: <why>. Cần người kiểm tra."
    static std::string gave_up(int attempts, Why why, const std::string& detail = {});

    /// Which of the three windows a page is drawn on.
    ///
    /// The crossing is not one program. The publisher's launcher hands over to
    /// the game's launcher, which hands over to the game, and a page is only
    /// ever findable on its own window. Switching at a hard-coded step was
    /// wrong in a way that only showed when the flow resumed into the middle:
    /// skip-forward would find the channel list on the game, then look for it
    /// on the publisher's launcher and wait out the whole minute.
    enum class On {
        Publisher,      ///< VTCZone
        Client,         ///< the untitled launcher from GhostOnlineVTC.exe
        Game,           ///< Game.exe
    };
    static const char* on_name(On w);

    /// How a page turns an index into a click.
    ///
    /// Four shapes, because the client uses more than one. Most pages are a
    /// single button and ignore the index entirely. The channel list is a
    /// column of rows, so the index moves the one click down it. The character
    /// select needs two clicks: the characters stand side by side on a stage
    /// and the one you want is picked where it stands, then started with a
    /// button in the corner -- that is `Slot`, and it is how a person does it.
    ///
    /// `Step` is for a carousel that shows one entry at a time and has to be
    /// wound round with an arrow. Nothing ships using it.
    enum class Pick {
        One,    ///< one place to click; the index is ignored
        Row,    ///< a list: the index moves the click by `row_step`
        Slot,   ///< click entry `index` where it stands, then click to confirm
        Step,   ///< a carousel: press `step_dx,step_dy` index times, then click
    };
    static const char* pick_name(Pick p);
    static Pick pick_from(const std::string& word, Pick fallback);

    /// The most a page is waited for. A wait read from the file is held to
    /// it, so a number typed one digit too long cannot run the clock over.
    static constexpr int kMaxWaitS = 600;
    /// The lowest match score taken from the file. A score below it -- "0,80"
    /// reads as 0, the number stopping at the comma -- finds its picture in
    /// every frame, and the page would then be clicked wherever the best of
    /// nothing happened to be. The usual score is used instead.
    static constexpr float kMinScore = 0.5f;

    /// One page of the crossing.
    struct Page {
        Step        step = Step::Idle;
        std::string key;            ///< its name in configs/login.ini
        std::string picture;        ///< which picture marks it; empty = unidentified
        On          on = On::Publisher;
        int         click_dx = -1;  ///< where to click, from the picture's top-left.
        int         click_dy = -1;  ///< -1 means the middle of the picture
        int         wait_s = 60;
        float       score = 0.70f;

        Pick        pick = Pick::One;

        /// Pick::Row and Pick::Slot -- the nth entry is this far below the
        /// first, or this far to the right of it when `row_across` is set.
        int         row_step = 0;
        bool        row_across = false;

        /// Pick::Slot -- where the FIRST entry is, from the picture's
        /// top-left. Pick::Row has no use for these because its click_dx and
        /// click_dy are the first entry; a slot page needs both, because the
        /// button that confirms the choice is somewhere else entirely. May be
        /// negative: the character stage is well to the left of the start
        /// button that the picture marks.
        int         row_dx = 0;
        int         row_dy = 0;

        /// Pick::Step -- where the "next" control is, from the picture's
        /// top-left. These offsets may be negative too.
        int         step_dx = 0;
        int         step_dy = 0;

        /// How long to wait after each press, and after choosing a slot before
        /// confirming it. Clicking "start" in the same instant as the
        /// character is picked is how you start the previous one.
        int         step_pause_ms = 1200;

        /// How many entries there are, for range-checking the index. 0 means
        /// "nobody has counted", which is the honest answer for a list whose
        /// length depends on the account; the index is then used as given.
        int         row_count = 0;

        bool identified() const { return !picture.empty(); }
    };

    struct Settings {
        /// The publisher's launcher, and its window's title. Where VTCZone
        /// installs itself unless told otherwise; started only when it is not
        /// already running.
        std::string launcher_path = "C:\\Program Files\\VTCZone\\VTCZone.exe";
        std::string launcher_window = "VTCZone";

        /// The game's own launcher -- the one with the start button on it.
        ///
        /// Found by its class and the program it belongs to, not by its title,
        /// because it has no title. It is a plain Win32 dialog of class #32770
        /// put up by GhostOnlineVTC.exe, and looking for it by name was the
        /// reason the flow pressed the publisher's button and then sat waiting
        /// for a window that was already on the screen in front of it.
        ///
        /// It is also a different program from the game: GhostOnlineVTC.exe
        /// puts up the launcher, Game.exe is what plays.
        std::string client_window;      ///< its title, usually empty
        std::string client_class = "#32770";
        std::string client_process = "GhostOnlineVTC.exe";

        std::string game_window = "GhostOnlineVTC";   ///< the game's own window title
        std::string game_class;
        std::string game_process;

        /// What VTCZone shows when it is signed out: the blue "ĐĂNG NHẬP"
        /// button of its sign-in form. Seen while the flow waits for one of
        /// VTCZone's own pages, with none of them up, and still there a few
        /// seconds later (see kSignInHoldMs), it stops the flow: nothing on
        /// that form is clicked and nothing is typed. No retry follows -- the
        /// form will still be there. Empty turns the check off, and the flow
        /// then waits the page out.
        std::string signin_picture = "login_page";
        float       signin_score = 0.80f;

        /// Which channel and which character, both counting from zero.
        ///
        /// Both are a position in a list on screen, not a name, because the
        /// client gives nothing else to go on: the channel rows say "Kênh 1"
        /// through "Kênh 8" in a font this project does not read, and the
        /// character slots are portraits. Where each entry sits is measured
        /// once and written down in [Page.channel_list] / [Page.character];
        /// these two are the index into it.
        int  channel_index = 0;
        int  character_index = 0;
        int  game_slot = 0;
        int  attempts = 3;

        /// Press the healing keys the moment the character appears.
        ///
        /// Worth doing and easy to leave out. A character logging back in after
        /// a disconnect arrives exactly where it died, which is where the thing
        /// that killed it still is -- and the macro does not start until the
        /// flow has finished tidying up. The helper this was taken from does
        /// the same thing and calls it emergency protection, which is fair.
        bool  heal_on_arrival = true;
        std::uint32_t heal_hp_key = 0;
        std::uint32_t heal_mp_key = 0;

        std::vector<Page> pages = default_pages();

        /// The file still had what an older GoBot kept for typing an account
        /// into VTCZone: a username=, a password= or a [Page.login_page].
        /// Only that they are there is noted -- their values are not read, not
        /// kept and not written anywhere, and the file is left as it is.
        bool stale_keys = false;

        bool load(const std::string& path);
        /// Write the whole file out. Only ever for a file that is not there:
        /// configs/login.ini is written in by hand, and a change to one key
        /// goes through IniFile::set_in_place so the rest of it survives.
        bool save(const std::string& path) const;

        /// Whether there is no file at `path` at all -- the one case `save`
        /// is for. `load` failing is not that: a file a virus scan or a sync
        /// is holding for a moment will not open either, and writing the
        /// table over it would throw away somebody's measurements and notes.
        static bool file_missing(const std::string& path);

        /// Pages whose picture nobody has identified yet.
        std::vector<const Page*> unidentified() const;

        /// Pages whose picture is named and is in `store` -- the ones the
        /// crossing can actually recognise.
        std::size_t recognised(const TemplateStore& store) const;

        /// Whether the launcher is where `launcher_path` says.
        bool launcher_found() const;

        /// VTCZone installed somewhere other than `launcher_path` says, and
        /// seen running this session: its program's own path instead, said
        /// once in the log, so that it can be started again if it is closed.
        /// `look` asks the running processes now; without it, only what was
        /// seen last (see running_launcher). True when it changed the path.
        bool adopt_running_launcher(bool look = true);
    };

    /// Where the running VTCZone's program is, asked of the process when
    /// `look` is true -- and remembered for the rest of the session -- else
    /// as last seen. Empty when it has not been seen running.
    static std::string running_launcher(bool look = true);

    /// The pictures the crossing matches against, from wherever they are.
    ///
    /// One way for every caller, so the GUI's keeper and the command line
    /// cannot cross differently: `from` when given -- a folder of PNGs or the
    /// other helper's assembly, for measuring with `--tplsrc` -- and otherwise
    /// the best set on this machine, which in a package is the staged
    /// `templates\` beside the executable. False when nothing loaded.
    static bool load_pictures(TemplateStore& store, const std::string& name_map_ini,
                              const std::string& from = {});

    /// Of `all`, only the pictures the crossing looks for: its pages' and the
    /// sign-in form's. For keeping while a keeper runs for hours -- the whole
    /// set is over a thousand pictures and some sixty megabytes, and the
    /// crossing needs seven of them.
    static TemplateStore pictures_for(const Settings& settings, const TemplateStore& all);

    /// The names of those pictures, sorted, each once. Two settings that give
    /// the same list are crossed with the same pictures, so a keeper that
    /// reads the file again only loads pictures again when this changed.
    static std::vector<std::string> picture_names(const Settings& settings);

    /// What VTCZone's window shows, as far as the crossing cares.
    enum class Publisher {
        Unknown,    ///< none of its pages, nor the sign-in form
        Ready,      ///< the game, or "Chơi game": signed in
        SignIn,     ///< the sign-in form, and none of its pages
    };
    /// One look at a frame of VTCZone's window. The pages win: a launcher that
    /// shows "Chơi game" is signed in, whatever else it has on it.
    static Publisher look_at_publisher(const Frame& vtczone, const TemplateStore& store,
                                       const Settings& settings);

    /// How long VTCZone's sign-in form has to stay up before the crossing
    /// stops for it, and how long a VTCZone this flow started itself gets
    /// first, counted from its start: one that remembers its account can show
    /// the form while it signs itself in, and stopping then would stop a
    /// crossing that was about to go through.
    static constexpr int kSignInHoldMs  = 5000;
    static constexpr int kSignInGraceMs = 30000;

    /// Whether the form, up for `up_ms`, is to be stopped for. `started_ms`
    /// is how long ago this flow started VTCZone, or negative when VTCZone
    /// was running already.
    static bool signin_settled(long long up_ms, long long started_ms);

    /// Whether a window is the Security Alert box the game's launcher can put
    /// up -- a #32770 dialog with that title.
    static bool is_security_alert(const std::string& window_class, const std::string& title);

    /// The window a page `on` it would be looked for in, or null. Picks the
    /// same window `InputManager::find_game_window` would, without a word in
    /// the log when there is none -- for a look taken every few seconds.
    static void* window_of(const Settings& settings, On which);

    /// VTCZone's window when it is minimised, among `process`'s windows, or
    /// null. `window_of` never finds one: it looks among windows big enough
    /// to aim at, and a minimised window is a sliver parked off the screen.
    /// GoC restores a minimised VTCZone before it clicks, and so does the
    /// flow -- which needs this to know there is one.
    static void* minimised_window(const Settings& settings, const std::string& process);

    /// Where a click at a client point of a window would go.
    enum class Landing {
        Lands,          ///< on the window, or one of its own children
        OffClient,      ///< outside its client area: an offset gone wrong, not a window in the way
        Minimised,      ///< the window is minimised, or gone
        OffScreen,      ///< on no monitor: the window is dragged part off the screen
        Covered,        ///< another window is over the point
    };

    /// Where a click at client point `x`,`y` of `window` would go: onto it
    /// when the point is inside its client area, on a screen, and the window
    /// there is `window` or one of its own children. When something else is
    /// there, `in_the_way` names its program -- by file name, never by title,
    /// which could be anything from a chat to a document -- or "?" when that
    /// cannot be told. It is left empty otherwise: no other window is to blame
    /// for a point that is off the window or off the screen.
    static Landing landing(void* window, int x, int y, std::string* in_the_way = nullptr);
    /// The same, as yes or no: whether the click lands on it.
    static bool lands_on(void* window, int x, int y, std::string* in_the_way = nullptr) {
        return landing(window, x, y, in_the_way) == Landing::Lands;
    }

    /// Whether nothing else is over any part of `window`'s client area. For
    /// trusting a frame read off the screen, which holds whatever lies over
    /// the window rather than the window itself. Every window above it in the
    /// order windows are drawn is asked -- visible, not minimised, not cloaked,
    /// not click-through, and not one of its own popups -- and one that lies
    /// over the client area anywhere on a screen is in the way: `in_the_way`
    /// names its program, as `landing` does, or "taskbar" for Windows' own.
    /// Off every screen does not count against it -- what is read there is
    /// black, and no page or HUD is found in black -- neither a part of the
    /// window nor a window over only that part, as long as one of five points
    /// across it is on a screen; a window dragged part off the screen, or
    /// taller than the screen, is still looked at.
    static bool uncovered(void* window, std::string* in_the_way = nullptr);
    /// The first window above `window` that lies over its client area, by the
    /// rules above; null when there is none.
    static void* over(void* window);

    /// A rectangle on the screen as Windows gives one: left, top, right,
    /// bottom, in pixels.
    struct Box { long left = 0, top = 0, right = 0, bottom = 0; };
    /// Whether a window drawn at `drawn` lies over a client area at `client`:
    /// whether the two meet on one of `screens`. Past every screen's edge
    /// nothing is seen by anybody, the game's frame included, so a window
    /// there is in nobody's way -- one kept on top past the bottom edge, say,
    /// over the part of a game taller than the screen. A `taskbar` with no
    /// more than kHiddenBarPx of itself on a screen is hidden: set to hide
    /// itself, it leaves that strip at the edge to be brought back with, and
    /// the rest of it lies past the edge. It counted against a game reaching
    /// that edge, and the crossing waited on a taskbar nobody could move.
    static bool lies_over(const Box& drawn, const Box& client, const std::vector<Box>& screens,
                          bool taskbar = false);
    static constexpr long kHiddenBarPx = 3;

    /// Where the HUD's "Level" plate -- hpmp_indicator2 -- sits on the game,
    /// on every client size, and how near it and how alike it has to be.
    static constexpr int   kLandmarkX = 44;
    static constexpr int   kLandmarkY = 4;
    static constexpr int   kLandmarkSlack = 4;
    static constexpr float kLandmarkScore = 0.75f;

    /// Whether the HUD's landmark is in a picture of the game where it always
    /// sits, give or take kLandmarkSlack. `landmark` may be null or empty: no.
    static bool plate_at(const Frame& game, const Frame* landmark);

    /// Whether a picture of the game shows the character in the world: the
    /// HP and MP bars found as a pair (SessionManager::hud_present), or the
    /// HUD's landmark where it always sits.
    ///
    /// Either will do. The bars alone missed the HUD on most frames with the
    /// bag open -- the MP bar was taken from a panel beside it -- and a
    /// crossing that took a game in the world for one stuck on a screen it
    /// does not know would end it. The landmark, the HUD's "Level" plate, was
    /// found at 0.85 or better on all but one of a thousand saved frames of
    /// the world; it is part of the HUD, which no screen of the login draws.
    /// `landmark` may be null or empty: the bars only.
    static bool in_world(const Frame& game, const Frame* landmark);

    /// Nothing is up. See `furthest_up`.
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    /// Where to go on from: the furthest page at or after `first` that `up`
    /// says is on screen now, or kNone.
    ///
    /// A page whose picture also marks an earlier page is never taken on the
    /// strength of its picture alone. Both connect steps are the same button,
    /// so seeing it proves only that the first has been reached; taking it for
    /// the second would walk past the channel list. And nothing goes past
    /// `halt_at`: a page beyond it counts as that page, which is the one a
    /// person asked to be left on.
    static std::size_t furthest_up(const std::vector<Page>& pages, std::size_t first,
                                   const std::function<bool(std::size_t)>& up,
                                   const std::string& halt_at = {});

    /// The first page the crossing may wait for, carry on from or click,
    /// given whether the game's launcher (`client_up`: its window, or
    /// GhostOnlineVTC.exe running) and the game (`game_up`: its window, or
    /// Game.exe running) are up: the first page drawn on the game while the
    /// game is up, else the first page on the launcher while that is up,
    /// else the first page of all.
    ///
    /// Nothing of VTCZone's is ever touched while the game or its launcher is
    /// open. Its pages stay drawn in its own picture of itself under them,
    /// and "Chơi game" pressed then starts a second launcher for the account.
    static std::size_t first_allowed_page(const std::vector<Page>& pages, bool client_up,
                                          bool game_up);

    /// Where an attempt carries on from: the furthest page `up` says is on
    /// screen among those `first_allowed_page` allows, or the first of those
    /// when none is. Never a page of a window that may not be touched.
    static std::size_t resume_from(const std::vector<Page>& pages, bool client_up, bool game_up,
                                   const std::function<bool(std::size_t)>& up,
                                   const std::string& halt_at = {});

    AutoLogin();

    void set_settings(const Settings& s);
    Settings settings() const { return settings_; }

    /// Where the pictures come from. The launcher's pages are not in the
    /// game's own set.
    void set_templates(TemplateStore* store) { store_ = store; }

    /// The HUD's landmark to tell the world by (see in_world). Without one
    /// the crossing loads hpmp_indicator2 itself, once, from the same places
    /// as every other picture -- the staged ones first.
    void set_landmark(const Frame& landmark) {
        landmark_ = landmark;
        landmark_loaded_ = true;
    }

    /// Called as the flow moves, for logging and for the UI.
    void on_step(std::function<void(Step, const std::string&)> fn) { report_ = std::move(fn); }

    /// How to end a game that came up and then stayed on a screen the
    /// crossing does not know -- not one of its pages, and not the world --
    /// and how long to wait after it before the next attempt.
    ///
    /// Ending clients belongs to the keeper (CrashWatch), not to the crossing,
    /// so whoever runs the crossing hands it in: the GUI's keeper and the
    /// command line pass one that ends the game and its launcher the way a
    /// crashed client is ended. With none set such an attempt just fails, as
    /// any other does, and nothing is ended. Nor is anything ended after the
    /// last attempt: what is on the screen then is for a person to look at.
    void set_end_client(std::function<bool()> end, int cooldown_s) {
        end_client_ = std::move(end);
        end_cooldown_s_ = cooldown_s;
    }

    /// What the crossing does to the desktop, one function each: the real
    /// thing unless set. A test sets them to a pretend desktop of its own
    /// windows and pictures, which is how the crossing's decisions are tried
    /// without VTCZone or the game -- and without a click, a change of focus
    /// or a program started anywhere on the real one.
    struct Hands {
        std::function<void*(On)>                   find;      ///< see window_of
        /// A picture of that window. Taken as the window's own picture of
        /// itself, which is good whatever lies over it.
        std::function<bool(On, void*, Frame&)>     grab;
        std::function<bool(void*)>                 focus;     ///< to the front
        std::function<bool(int, int)>              click;     ///< a client point of the window aimed at
        std::function<bool(const std::string&)>    running;   ///< is that program running
        std::function<bool(const std::string&)>    start;     ///< start that program
        std::function<void(void*)>                 minimise;  ///< put that window away
    };
    void set_hands(Hands h) { hands_ = std::move(h); }

    /// Cross as far as this page and stop, leaving it on the screen.
    ///
    /// For measuring. The channel list and the character select are two
    /// screens that appear for about a second and a half in the middle of a
    /// crossing, which is not long enough to photograph one and work out where
    /// its entries are. This parks the client on the screen instead.
    ///
    /// Give it a page key from configs/login.ini -- `channel_list`, say. An
    /// empty string, the default, means run the whole crossing.
    void halt_before(std::string page_key) { halt_at_ = std::move(page_key); }

    /// Ask it to stop at the next opportunity -- a click already on its way
    /// included: one asked for after the page was found is not clicked, nor
    /// its window brought forward. It stays asked: a flow is made for one
    /// crossing, and a stop that comes before run() still stops it.
    void cancel() { cancelled_.store(true); }

    /// Run the whole thing. Blocks until the character is in the world, a
    /// person is needed, or it was cancelled.
    bool run();

    Step step() const { return step_.load(); }

    /// Why it stopped, when it stopped badly: in English for the log, and as
    /// a reason the simple screen can say in Vietnamese.
    std::string trouble() const { return trouble_; }
    Why why() const { return why_; }
    std::string why_detail() const { return why_detail_; }
    /// The Vietnamese for that, wrapped as `gave_up` when every attempt was
    /// used on something another attempt might have got past.
    std::string advice() const;
    int attempts_made() const { return attempts_made_; }

    /// Whether a click is being held back right now because another window
    /// is over the place to click, and that window's program (as `lands_on`
    /// names it). For an `on_step` callback, which runs on the flow's own
    /// thread, to say so while the flow waits.
    bool holding() const { return holding_; }
    const std::string& held_by() const { return held_by_; }
    /// Whether that hold is for a window that cannot be seen at all -- a page
    /// or the HUD awaited behind another window -- rather than for a click.
    bool hiding() const { return hidden_; }
    /// The Vietnamese for that: what is in the way, by its program, or that
    /// the window is minimised or off the screen when `by` is empty.
    static std::string hidden_advice(const std::string& by);

    /// The default table: every page of the crossing, in order, with the
    /// pictures and the measured geometry that ship in configs/login.ini.
    /// Written out when that file is missing, so it crosses as written.
    static std::vector<Page> default_pages();

private:
    /// How a wait for a page ended.
    enum class Seen {
        Page,       ///< it is up, at `found`
        Later,      ///< a page after it is up instead: go on from there
        Barred,     ///< its window may not be touched now: go on from `later`
        Stopped,    ///< something a person must see to; `why_` says what
        TimedOut,   ///< not seen for its wait_s; this attempt is over
    };

    /// How one click went.
    enum class Click {
        Done,       ///< clicked
        Held,       ///< not clicked: something else is there; `held_by_` says what
        BadPoint,   ///< not clicked: the point is off the window's client area
        Stopped,    ///< not clicked: a stop was asked for, or a Security Alert is up
    };

    /// The game and its launcher, as one look finds them: their windows, or
    /// null, and whether each is up at all -- a window, or its program
    /// running without one yet.
    struct Ahead {
        void* game = nullptr;
        void* client = nullptr;
        bool  game_up = false;
        bool  client_up = false;
    };
    Ahead  ahead_now();

    /// What an attempt finds before it touches VTCZone; see cross().
    enum class FirstLook { Nothing, Client, Game, InWorld, Stopped };
    FirstLook first_look(const Ahead& ahead);

    bool   cross();
    Seen   await(std::size_t index, ScreenRect& found, std::size_t& later);
    /// Click a client point of the window `input_` is aimed at -- only after
    /// bringing that window to the front and making sure the click lands on it.
    Click  click(int x, int y);
    /// Each window's picture, now. Quiet: a window that is not up is just
    /// left out, because this is asked every second while a page is awaited.
    /// So is a picture read off the screen while its window is not in front
    /// and clear, which shows whatever lies over the window instead; `behind`
    /// says which were left out for that.
    void   look_around(Frame (&frames)[3], bool (&behind)[3]);
    /// One window's picture, and whether it can be trusted to show that
    /// window: one the window drew of itself always can, one read off the
    /// screen only while the window is in front and nothing is over it.
    bool   grab(On which, void* hwnd, Frame& out, bool& trusted);
    bool   up_anywhere(std::size_t index, const Frame (&frames)[3]) const;
    /// Whether any page drawn on the game is in `frame`.
    bool   game_page_in(const Frame& frame) const;
    /// The landmark set or loaded for this crossing -- loaded the first time
    /// it is wanted -- or null when there is none.
    const Frame* landmark();
    /// in_world, by that landmark.
    bool   world_in(const Frame& frame);
    /// Whether a picture of the game `hwnd` shows the world, trusted or not:
    /// one that cannot be trusted as a whole -- something over another part
    /// of the game -- still shows the landmark truly where nothing is over
    /// the corner it sits in. Only the landmark is looked for then, in that
    /// corner; the bars are looked for over more of the picture than that.
    bool   world_on(void* hwnd, const Frame& frame, bool trusted);
    /// Whether nothing is over the corner of the game where the landmark sits.
    bool   plate_clear(void* hwnd);
    /// While the picture of the window a wait is on cannot be trusted, say
    /// what is over it -- as `holding()` does for a held click, so the screen
    /// can name it while the flow waits -- and stop saying so once it can.
    void   note_hidden(Step during, On which, bool hidden);

    /// What the game shows, looked at after an attempt failed on one of its
    /// pages or on the way into the world.
    enum class GameNow {
        Gone,       ///< no game at all
        InWorld,    ///< the character is in the world after all
        OnPage,     ///< one of the crossing's pages is up: the next attempt goes on from it
        Stuck,      ///< up on a screen the crossing does not know, or with no window at all
        Unknown,    ///< up, and its picture cannot be trusted to be the game's
    };
    GameNow judge_game();
    bool   security_alert_up() const;
    /// Stop for a Security Alert box, if one is up. True when it stopped.
    bool   stop_for_alert();
    bool   stop(Why why, const std::string& english, const std::string& detail = {});
    void   fail(Why why, const std::string& english, const std::string& detail = {});
    void   say(Step s, const std::string& detail);
    bool   stop_now();
    /// Sleep in short slices so cancelling does not wait out a whole page.
    bool   nap(int ms);

    // Through the hands when they are set, else the real desktop.
    void*  find(On which);
    /// The game's window when it is minimised -- which `find` never sees --
    /// or null. Always null on a pretend desktop.
    void*  minimised_game();
    /// Bring a minimised game back, as a minimised VTCZone is brought back.
    /// True when there was one; false too when stopped meanwhile.
    bool   restore_game(Step during);
    bool   bring_forward();
    bool   running(const std::string& program);
    /// Put VTCZone away once the game is up, as GoC does: once an attempt.
    void   put_vtczone_away();

    Settings           settings_;
    std::string        halt_at_;
    TemplateStore*     store_ = nullptr;
    Frame              landmark_;               ///< see set_landmark
    bool               landmark_loaded_ = false;
    InputManager       input_;
    On                 aimed_at_ = On::Publisher;   ///< where `input_` points
    /// One per window: each keeps its own bitmap and its own idea of whether
    /// that window will draw itself or has to be read off the screen.
    ScreenCapture      capture_[3];
    std::atomic<Step>  step_{Step::Idle};
    std::atomic<bool>  cancelled_{false};
    std::string        trouble_;
    Why                why_ = Why::None;
    std::string        why_detail_;
    int                attempts_made_ = 0;
    std::function<void(Step, const std::string&)> report_;
    Hands              hands_;
    std::function<bool()> end_client_;
    int                end_cooldown_s_ = 15;

    bool               holding_ = false;        ///< see holding()
    std::string        held_by_;
    Landing            held_where_ = Landing::Lands;    ///< why the last click was held
    /// Set by note_hidden: `holding_` is for a picture that cannot be
    /// trusted, not for a held click.
    bool               hidden_ = false;
    /// For each window, what was over it when its last picture could not be
    /// trusted -- the program in front, or over one of the points looked at
    /// -- or empty: minimised, or nothing of it on a screen.
    std::string        behind_by_[3];
    /// Set when this run started VTCZone itself, for the sign-in form's grace.
    bool               started_vtczone_ = false;
    std::chrono::steady_clock::time_point vtczone_started_{};
    /// VTCZone has been put away this attempt, now that the game is up.
    bool               vtczone_put_away_ = false;
    /// When the window awaited was last brought forward to be looked at.
    std::chrono::steady_clock::time_point brought_forward_{};
};

} // namespace gobot
