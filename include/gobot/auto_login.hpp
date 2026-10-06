#pragma once

#include "gobot/config.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace gobot {

/// Gets from a cold machine to a character standing in the world.
///
/// Between the two there are nine screens, none of them the game's: the
/// publisher's launcher, its login page, its list of games, its start button,
/// then the client's own launcher, its start button, a connect button, a
/// channel list, another connect button, and a character to pick. A bot that
/// cannot cross them is a bot that needs a person every time the client falls
/// over, which is the thing an unattended run cannot have.
///
/// The sequence is fixed and is written down below as a table, because it is
/// one. What is *not* fixed is which picture identifies each page -- those
/// belong to the publisher's launcher, they differ between installs and
/// versions, and this project has no business guessing at them. Each step
/// therefore names a picture, the names live in configs/login.ini, and a step
/// whose picture has not been identified yet says exactly that rather than
/// failing in some less helpful way.
///
/// Identifying one is a minute's work: get the launcher onto the page, run
/// `gobot_cli --window <launcher> --tplsrc <set> --survey`, and write down
/// what it found. The flow then crosses that page for good.
///
/// Two things are deliberately not done here.
///
/// A captcha is left alone. If one appears the flow stops and says so; it does
/// not try to read it, and it never will. That is somebody asking whether a
/// person is there, and the honest answer is no.
///
/// Nothing is retried forever. Three attempts, then it stops and asks for a
/// person. A login loop that keeps hammering a server which is refusing it is
/// indistinguishable from an attack on that server, and would be treated as
/// one.
class AutoLogin {
public:
    /// What the flow is doing, in the order it does it.
    enum class Step {
        Idle,
        StartLauncher,      ///< run the publisher's launcher
        FindLauncher,       ///< wait for its window and bring it up
        LoginPage,          ///< type the account in and submit
        GameList,           ///< pick the game out of the account's list
        StartGame,          ///< the launcher's start button
        ClientLauncher,     ///< the client's own small launcher window
        GameWindow,         ///< wait for the game's window to exist
        Connect,            ///< "Kết Nối"
        Channel,            ///< pick a channel
        ConnectAgain,       ///< "Kết Nối" once more
        Character,          ///< pick the character
        InWorld,            ///< the HUD is up: done
        Blocked,            ///< a person is needed
    };
    static const char* step_name(Step s);

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
        std::string launcher_path;      ///< the publisher's launcher executable
        std::string launcher_window;    ///< its window title

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

        std::string game_window;        ///< the game's own window title
        std::string game_class;
        std::string game_process;

        std::string username;
        /// Protected with the Windows account's own key, so the file is of no
        /// use on another machine or to another user. See `protect`/`unprotect`.
        std::string password_protected;

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

        std::vector<Page> pages;

        bool load(const std::string& path);
        bool save(const std::string& path) const;

        /// Pages whose picture nobody has identified yet.
        std::vector<const Page*> unidentified() const;
    };

    /// Encrypt a string so that only this Windows user on this machine can read
    /// it back, and render it as text for a config file. Empty on failure.
    ///
    /// This is not a secret keeping scheme, and is not claimed as one: anything
    /// running as this user can call `unprotect`. What it is for is that an
    /// account password should not be sitting in a text file in plain sight,
    /// where it gets copied into a bug report, a screenshot or a backup along
    /// with everything else in the folder.
    static std::string protect(const std::string& plain);
    static std::string unprotect(const std::string& stored);

    AutoLogin();

    void set_settings(const Settings& s);
    Settings settings() const { return settings_; }

    /// Where the pictures come from. The launcher's pages are not in the
    /// game's own set.
    void set_templates(TemplateStore* store) { store_ = store; }

    /// Called as the flow moves, for logging and for the UI.
    void on_step(std::function<void(Step, const std::string&)> fn) { report_ = std::move(fn); }

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

    /// Ask it to stop at the next opportunity.
    void cancel() { cancelled_.store(true); }

    /// Run the whole thing. Blocks until the character is in the world, a
    /// person is needed, or it was cancelled.
    bool run();

    Step step() const { return step_.load(); }

    /// Why it stopped, when it stopped badly.
    std::string trouble() const { return trouble_; }

    /// The default table: every page of the crossing, in order, with no
    /// pictures named. Written out on first run so there is something to fill
    /// in rather than a file to invent.
    static std::vector<Page> default_pages();

private:
    bool   wait_for_page(const Page& page, ScreenRect& found);
    void   say(Step s, const std::string& detail);
    bool   stop_now() const { return cancelled_.load(); }
    /// Sleep in short slices so cancelling does not wait out a whole page.
    bool   nap(int ms);

    Settings           settings_;
    std::string        halt_at_;
    TemplateStore*     store_ = nullptr;
    InputManager       input_;
    ScreenCapture      capture_;
    std::atomic<Step>  step_{Step::Idle};
    std::atomic<bool>  cancelled_{false};
    std::string        trouble_;
    std::function<void(Step, const std::string&)> report_;
};

} // namespace gobot
