#pragma once

#include "gobot/auto_chat.hpp"
#include "gobot/bag.hpp"
#include "gobot/bot_brain.hpp"
#include "gobot/calibrator.hpp"
#include "gobot/camera_tracker.hpp"
#include "gobot/navigator.hpp"
#include "gobot/command_executor.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/framing.hpp"
#include "gobot/front_keeper.hpp"
#include "gobot/game_state.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/macro.hpp"
#include "gobot/map_file.hpp"
#include "gobot/map_mapper.hpp"
#include "gobot/map_table.hpp"
#include "gobot/minimap_tracker.hpp"
#include "gobot/net_discovery.hpp"
#include "gobot/opcode_registry.hpp"
#include "gobot/packet_capture.hpp"
#include "gobot/packet_eyes.hpp"
#include "gobot/packet_world.hpp"
#include "gobot/pathfinder.hpp"
#include "gobot/protocol_analyzer.hpp"
#include "gobot/protocol_handler.hpp"
#include "gobot/routine.hpp"
#include "gobot/crash_watch.hpp"
#include "gobot/dpi.hpp"
#include "gobot/party.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/party_hud.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/player_list.hpp"
#include "gobot/potion_stock.hpp"
#include "gobot/vitals_reader.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/hotkeys.hpp"

#include <array>
#include <atomic>
#include <thread>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ImFont;

namespace gobot {

class AutoLogin;

/// The GUI front-end.
///
/// Owns the whole pipeline and draws it. Panels mirror the workflow rather
/// than the class diagram: you cannot run the bot until the protocol is
/// mapped, so Capture and Protocol come before Bot and Skills.
///
/// Everything the panels read is a snapshot pulled once per frame — the
/// capture and bot threads keep running while ImGui draws, and reaching into
/// their state mid-frame would tear.
class GuiApp {
public:
    GuiApp();
    ~GuiApp();

    GuiApp(const GuiApp&) = delete;
    GuiApp& operator=(const GuiApp&) = delete;

    /// Whether the process is elevated. Capture needs it; the GUI shows a
    /// banner when it is missing. Set before initialise().
    void set_elevated(bool e) { elevated_ = e; }

    /// Our own top-level window. Needed to tell whether we are sitting on top
    /// of the game, which breaks screen capture. Set before initialise().
    void set_main_window(void* hwnd) { main_hwnd_ = hwnd; }

    /// The faces the simple screen is drawn in: one for text and a heavier one
    /// for headings and the start button, both with Vietnamese in them.
    /// ImGui's own font is ASCII only and draws every "ế" as a question mark.
    /// Null keeps the default. Set before the first draw.
    void set_fonts(ImFont* text, ImFont* strong) { text_font_ = text; strong_font_ = strong; }

    /// Load configs, find the game window. Call once before the first draw.
    void initialise();

    /// Draw one frame.
    void draw();

    /// Called from the Win32 message loop each iteration, before draw().
    void tick();

    bool wants_quit() const { return quit_; }

private:
    // ---- panels ----
    void draw_menu_bar();
    void draw_macro_panel();
    void draw_vitals_panel();
    void draw_mapper_panel();
    void draw_dashboard();
    void draw_capture_panel();
    void draw_protocol_panel();
    void draw_bot_panel();
    void draw_skills_panel();
    void draw_heal_panel();
    void draw_log_panel();
    void draw_status_bar();

    // ---- the shape the commercial client uses ----
    //
    // Its window is one tall column: a run bar across the top, then a block
    // that always shows the character and what is around it, then the tabs.
    // Copied deliberately. The arrangement is the part of that client people
    // actually learned, and a tool that does the same job with the controls
    // somewhere else is a tool they have to learn twice.
    //
    // Its own window is 975 x 1164 -- portrait, meant to stand beside the game
    // rather than cover it.
    void draw_run_bar();
    void draw_player_block();
    void draw_nearby_monsters();

    /// One feature: a switch, a name, a line saying what it does, and its own
    /// settings folded away underneath.
    ///
    /// This is the shape the other helper uses, and copying it is deliberate.
    /// Its window is a list of these -- fishing, mining, buffs, auto-login --
    /// each with a toggle and an expander. Nothing is behind a tab, so the
    /// answer to "what can this thing do, and what is currently on?" is the
    /// first screen rather than fourteen of them.
    ///
    /// `on` may be null for a feature that is run rather than switched on.
    ///
    /// Returns true on the frame the switch was changed, so the caller can push
    /// the change where it has to go. The first version returned nothing, and
    /// every switch in the list was therefore inert: it flipped a field in the
    /// front-end's own copy of the config and the running engine never heard
    /// about it. The switch moved, and nothing happened.
    bool draw_feature(const char* name, bool* on, const char* what,
                      const std::function<void()>& settings);

    /// Push the front-end's copy of the configuration into whatever is running.
    void apply_bot_config();
    void apply_macro_config();

    /// The list of them. Only features that actually work: see draw_feature's
    /// note on why the half-built ones are not here at all.
    void draw_feature_list();

    void draw_profile_panel();     ///< class, weapon, and the range preset
    void draw_attack_panel();      ///< what it hits and how it stands to hit it
    void draw_hunt_settings();     ///< the hunter's knobs, shared with Radar
    /// Whether the protocol map can place the character. Without that the
    /// packet bot has no idea where it is, and Attack uses the hunter.
    bool packet_bot_can_see_self() const;
    void draw_buff_panel();
    void draw_waypoints_panel();
    void draw_area_panel();
    void draw_monsters_panel();
    void draw_routines_panel();    ///< the click sequences, and a dry run

    /// routines.ini, re-read when it changes rather than every frame.
    std::vector<Routine> routines_cache_;
    std::uint64_t        routines_mtime_ = 0;       ///< last write time seen
    std::uint64_t        routines_checked_ms_ = 0;  ///< when it was last stat'd

    void draw_session_panel();     ///< the watcher, auto-login, and stopping
    void draw_keys_panel();        ///< the keys the game itself is set to
    void draw_tools_panel();       ///< everything that exists to take the game apart

    /// What has not been built, listed in one place off the main screen.
    ///
    /// Not as tabs, and not as switches. A panel of controls that look live and
    /// do nothing costs somebody an evening finding out; a tab that says "not
    /// built" costs them a click to learn nothing. One honest list, out of the
    /// way, under Help.
    void draw_not_built_list();
    bool show_not_built_ = false;

    // ---- protocol discovery sub-panels ----
    void draw_autopilot();
    void draw_framing_editor();
    void draw_detect_section();
    void draw_opcode_table();
    void draw_packet_inspector();
    void draw_field_analyzer();
    void draw_layout_editor();
    void draw_suggestions();

    // ---- actions ----
    void refresh_interfaces();
    void start_capture();
    void stop_capture();
    void start_bot();
    void stop_bot();
    void toggle_bot();
    void run_detection();
    void apply_framing(const FramingSpec& spec);
    void save_protocol();
    void save_bot_config();
    void reload_configs();
    void find_game_window();
    /// Point everything that looks at or presses into the game at `hwnd`: the
    /// keyboard and mouse, the HP/MP reader, the monster detector and the map
    /// mapper -- and, when it is another window than before, forget what was
    /// learned from the old one: the detector's plates and where it last saw
    /// the character (it forgets those itself), where the view was, the map's
    /// edge, and a walk steered by them. Every way the game's window is chosen
    /// goes through here: the pickers, Tìm lại, BẮT ĐẦU's look, Find game
    /// window, the setup's look and the keeper's reattach. Done piecemeal, the
    /// detector kept grabbing a window that had closed and served its last
    /// frame forever -- the hunter walking and hitting at monsters of a game
    /// that was gone -- and a window picked on the Capture tab left the HP/MP
    /// reader on the old one.
    void retarget(void* hwnd);
    /// How Windows shows the game (gobot/dpi.hpp): enlarged, above 100% display
    /// scale, when the game does not handle DPI itself -- and no reader can work
    /// on a game shown like that -- and what would cure it. Looked at on every
    /// change of window and every few seconds, since the scale can be changed
    /// under a running GoBot.
    void check_game_scaling();
    /// Sets the game program's high-DPI override so Windows stops enlarging it,
    /// from the next start of the game. What to tell the player, in Vietnamese.
    std::string fix_game_scaling();
    /// Takes a high-DPI override off GoBot's own exe, which holds GoBot to
    /// Windows' scaling (threads_view_percent() over 100), from GoBot's next
    /// start. What to tell the player, in Vietnamese.
    std::string fix_own_scaling();
    /// InputManager::find_game_window with these, and on success retarget() to
    /// what it found. False, with nothing changed, when nothing matched.
    bool find_and_retarget(const std::string& title, const std::string& window_class,
                           const std::string& process);
    /// Join a worker's thread from the window's own thread, answering meanwhile
    /// what other threads send to this thread's window -- nothing posted, so no
    /// click or key is handled from in here. A worker that sends the window
    /// something and waits for the answer -- a title read off it, the focus
    /// handed on while attached to this thread's input -- would otherwise wait
    /// on a thread that is waiting on it, and neither would ever go on: GoBot
    /// "Not Responding", and the rest of F12 never done.
    static void join_pumping(std::thread& t);
    /// Everything that acts on the game, stopped as F12, DỪNG, a failed revive
    /// and closing GoBot stop it: first every job is told -- its flag set, the
    /// pass, round, line, invite or crossing under way cancelled -- with nothing
    /// waited for, every key let go of, and everything that presses keys
    /// through the coordinator held off; only then is each one waited for, and
    /// every key let go of again. Told one at a time and waited for in between,
    /// a slow one -- an invite of eight seconds, a quest round, a crossing --
    /// kept the rest pressing and clicking meanwhile, with the window that reads
    /// F12 frozen. `with_bot`: the packet bot and a walk to a saved place as
    /// well -- F12, a failed revive, closing GoBot; DỪNG has never stopped
    /// those two. True when the trash worker was stopped with the bin's dialog
    /// maybe still open, and said so (stop_trash).
    bool stop_jobs(bool with_bot);

    void on_packets(const std::vector<GamePacket>& packets);

    /// Confirm the framing once live traffic proves it tiles cleanly.
    void check_framing_health();

    // ---- autopilot ----
    /// Drives the whole setup with no input: find the game's own socket, pick
    /// the adapter that carries it, capture, record, detect framing, then
    /// analyse what the opcodes mean. Advanced one step per frame from tick().
    enum class Auto {
        Idle,
        FindGame,
        FindConnection,
        StartCapture,
        Recording,
        Detecting,
        Collecting,
        Analysing,
        Done,
        Failed,
    };

    void auto_start();
    void auto_cancel();
    void auto_step();
    const char* auto_stage_label() const;
    float auto_progress() const;

    Auto          auto_stage_ = Auto::Idle;
    std::uint64_t auto_stage_started_ms_ = 0;
    std::string   auto_error_;
    std::string   auto_detail_;

    std::vector<ProcessConnection> game_connections_;
    std::uint32_t                  game_pid_ = 0;

    /// Raw bytes per stream. The framing detector needs one direction of one
    /// connection; keeping them separate means we can simply pick the busiest
    /// rather than asking which connection is the game.
    std::map<std::uint64_t, Bytes> flow_samples_;
    std::map<std::uint64_t, std::string> flow_labels_;

    /// Traffic volume per direction. The client->server half is the tell for
    /// whether the character actually did anything: it is a trickle of
    /// keepalives when standing still, and a steady stream when moving. A
    /// capture of a stationary character contains nothing to infer from, and
    /// saying that is far more useful than reporting no results.
    std::uint64_t bytes_to_client_ = 0;
    std::uint64_t bytes_to_server_ = 0;

    std::map<std::uint16_t, OpcodeObservations> observations_;
    std::vector<SemanticGuess>                  suggestions_;
    bool                                        analysis_ran_ = false;

    /// How long autopilot records before detecting, and collects before
    /// analysing. Long enough to be representative, short enough to sit
    /// through.
    static constexpr std::uint64_t kRecordMs  = 25'000;
    static constexpr std::uint64_t kCollectMs = 40'000;

    // ---- pipeline ----
    AppConfig  app_config_;
    BotConfig  bot_config_;

    GameState                        state_;
    OpcodeRegistry                   registry_;
    std::unique_ptr<ProtocolHandler> protocol_;
    InputManager                     input_;
    ExecutionCoordinator             coord_;
    PathFinder                       pathfinder_;
    std::unique_ptr<CommandExecutor> executor_;
    std::unique_ptr<BotBrain>        brain_;
    /// You and every monster from the game's own frames: on_packets hands it
    /// every frame, UDP and unpacked ones too, on the capture thread.
    /// Declared before the capture, so that it outlives the capture's thread.
    PacketWorld                      packet_world_;
    PacketCapture                    capture_;

    // ---- keyboard macro ----
    MacroConfig                  macro_config_;
    std::unique_ptr<MacroEngine> macro_;
    bool                         macro_running_ = false;

    void start_macro();
    void stop_macro();
    void toggle_macro();
    void save_macro_config();

    /// Bring the game to the front before something starts pressing keys.
    ///
    /// Keys only ever go to a focused window, so whatever just took focus from
    /// the game by being clicked has to give it back or nothing happens. False
    /// when Windows refused -- it will not hand the foreground to a process
    /// that does not already own it, which is what pressing a hotkey from some
    /// third window looks like.
    bool focus_game_to_start(const char* what);

    /// Compact game-window chooser, shown inline on the panels that need a
    /// target rather than only on the Capture tab. Picking the window is a
    /// prerequisite for the macro, so it cannot live behind Advanced.
    void draw_window_picker_inline();

    // ---- parties ----
    /// Answering invitations, and sending them to whoever asks in chat.
    ///
    /// Run on a thread of its own because both halves are slow in the way that
    /// would make the window stutter if they were not: each costs a capture,
    /// and inviting somebody costs several seconds of clicking and waiting for
    /// the client to catch up.
    void start_party();
    /// `wait` false: told to stop -- the flag set, what is under way cancelled
    /// -- and not waited for; stop_jobs tells every job first and only then
    /// waits for each. stop_trash then returns false.
    void stop_party(bool wait = true);
    void party_loop();
    void draw_party_panel();

    std::thread       party_thread_;
    std::atomic<bool> party_running_{false};
    std::atomic<bool> party_stop_{false};
    std::atomic<int>  party_accepted_{0};
    std::atomic<int>  party_invited_{0};

    // ---- keeping the client alive ----
    /// Notices the client has fallen over and crosses back in. `oneshot`: for
    /// "Vào game ngay" alone (simple_keeper.cpp).
    void start_keeper(bool oneshot = false);
    void stop_keeper(bool wait = true);     ///< `wait` as for stop_party
    void keeper_loop();
    /// How a crossing of the keeper's came about: what it says, and whether
    /// it is a restart by itself (counted already by keeper_take_restart).
    enum class Crossing {
        Cold,       ///< BẮT ĐẦU with the game closed: a start, not a restart
        Restart,    ///< the client found gone, or ended by the keeper
        Key,        ///< "Vào game ngay", with the game up: nothing ended for it
    };
    enum class Crossed {
        In,         ///< in the world
        Cancelled,  ///< the login key pressed again: the keeper goes on
        Stopped,    ///< the keeper was told to stop
        GaveUp,     ///< a person is needed -- said and alerted: the keeper stops
    };
    /// One crossing on the keeper's thread, for keeper_loop and the login key
    /// alike: login.ini read afresh into `login` (and `store`, when it names
    /// other pictures), the keeper's checks, the flow made, registered for a
    /// stop or the key to cancel, and run. A client it ends on a screen it
    /// does not know is a restart (keeper_take_restart) -- and only a keeper
    /// the player ticked "Tự vào lại game" for ends one.
    Crossed run_crossing(Crossing how, CrashWatch& watch, const CrashWatch::Settings& watch_settings,
                         AutoLogin::Settings& login, TemplateStore& store);
    /// max_restarts, the one rule: a restart is every time the game was
    /// closed and had to be entered again -- a client the keeper found gone
    /// or ended, or one a crossing ended for being stuck, however that
    /// crossing began. True, and counted, while one more is allowed; false,
    /// said and alerted, when they are used up. The keeper's thread only.
    bool keeper_take_restart(const CrashWatch::Settings& watch_settings);
    /// The keeper's own lines, to gobot.log and the panel. The crossing's
    /// steps are logged by the crossing itself, so those only go to the panel.
    void keeper_note(const std::string& line);
    /// Stopping for a person: English for the log and the panel, Vietnamese
    /// for the simple screen, where it stays after the keeper has gone.
    void keeper_give_up(const std::string& english, const std::string& vi);
    void draw_keeper_panel();

    std::thread       keeper_thread_;
    std::atomic<bool> keeper_running_{false};
    std::atomic<bool> keeper_stop_{false};
    std::atomic<int>  keeper_restarts_{0};   ///< counted by keeper_take_restart, from 0 at start_keeper
    /// A client ended by a crossing, and counted as a restart then: the next
    /// look that finds it gone does not count it again. The keeper's thread.
    bool              keeper_closed_counted_ = false;
    /// The macro held by tick() for a death the packets read, with no reader
    /// of the bars running; tick() lets it go.
    bool              packet_dead_hold_ = false;

    /// What either thread last did, for the panels to show. Guarded because
    /// they are written from a worker and read from the drawing thread.
    mutable std::mutex       worker_mutex_;
    std::vector<std::string> party_log_;
    std::vector<std::string> keeper_log_;
    std::string              keeper_state_ = "not watching";
    void say_party(const std::string& line);
    void say_keeper(const std::string& line);

    /// The keeper in Vietnamese, for the simple screen: what it is doing now,
    /// and what last stopped it -- kept after it has stopped, so the reason is
    /// still there to read. keeper_log_ is the English, as is gobot.log.
    std::string              keeper_vi_;
    std::string              keeper_problem_vi_;
    void say_keeper_vi(const std::string& now, const std::string& problem = {});

    /// The login the keeper is in the middle of, so stopping it can cancel
    /// the login instead of waiting minutes for it to finish by itself.
    std::mutex keeper_flow_mutex_;
    AutoLogin* keeper_flow_ = nullptr;

    /// Set by the keeper once it is back in the world. The game it logged in
    /// to is a new process with a new window, and everything here still points
    /// at the one that went away; tick() points it all at the new one. Also
    /// set while a cold start waits and the game is found up already.
    std::atomic<bool> keeper_reattach_{false};
    /// True when the game window was found; says so in the log once a run.
    bool reattach_game();
    bool reattach_missed_ = false;      ///< the last reattach found no window
    /// The keeper was started by a cold start (simple_cold_start_, which only
    /// the drawing thread may read) and the ticked jobs have not started yet.
    /// The keeper's own copy of it; cleared by tick() once they have, and by
    /// stop_keeper().
    std::atomic<bool> keeper_cold_start_{false};

    // ---- reviving ----
    /// Watches for the death panel and revives on the spot with a revive
    /// item, the way GoC does: see Reviver. Three tries for each death; when
    /// they run out the bot stops -- the player's rule, rather than revive in
    /// the village for 2% EXP.
    void start_revive();
    void stop_revive(bool wait = true);     ///< `wait` as for stop_party
    void revive_loop();
    void say_revive(const std::string& line);
    /// The revive's tries ran out: stop every job, let go of every key, and
    /// say why in red until the next BẮT ĐẦU. On the drawing thread, from
    /// tick(): stopping joins the revive worker that asked for it.
    void stop_after_revive();

    std::thread              revive_thread_;
    std::atomic<bool>        revive_running_{false};
    std::atomic<bool>        revive_stop_{false};
    std::atomic<int>         revive_count_{0};
    std::vector<std::string> revive_log_;          ///< guarded by worker_mutex_
    std::atomic<bool>        revive_gave_up_{false};   ///< set by the worker; see stop_after_revive
    std::string              revive_gave_up_why_;      ///< guarded by worker_mutex_
    int                      revive_seen_ = 0;         ///< revive_count_ as tick() last saw it
    /// When the HP reading first showed 0 -- dead with the HUD up -- for
    /// re-casting the buffs once it is above 0 again; 0 when it is not 0.
    std::uint64_t            hp_zero_since_ms_ = 0;

    // ---- trash cleaning ----
    /// Opens the bag every so often and gets rid of what is on the list. One
    /// thread for both the timed passes and the ones asked for by a button;
    /// started for a button press alone, it does that one pass and ends.
    void start_trash();                   ///< the timed passes, for a run
    void request_trash(bool look_only);   ///< one pass now
    /// True when it ended a hold the bin may still need, and said so. `wait`
    /// as for stop_party.
    bool stop_trash(bool wait = true);
    void trash_loop();
    void say_trash(const std::string& line);

    std::thread              trash_thread_;
    std::atomic<bool>        trash_running_{false};
    std::atomic<bool>        trash_stop_{false};
    std::atomic<bool>        trash_oneshot_{false};   ///< here for one asked-for pass
    std::atomic<int>         trash_request_{0};       ///< 1 clean now, 2 look only, 3 charms now, 4 buy potions now, 5 a trial of it
    std::atomic<int>         trash_thrown_{0};        ///< items got rid of, all runs
    std::vector<std::string> trash_log_;              ///< guarded by worker_mutex_
    std::mutex               trash_cleaner_mutex_;
    BagCleaner*              trash_cleaner_ = nullptr; ///< the pass under way, to cancel
    /// Held for a trash pass, for the revive sequence and for the party
    /// worker's clicks, so no two drive the mouse at once: a click on the
    /// death dialog in the middle of a drag lets the item go wherever the
    /// pointer was.
    std::mutex               game_mouse_mutex_;
    std::atomic<bool>        trash_pass_{false};      ///< a pass is driving the bag
    /// A pass could not give an item back -- the bin's dialog may be open
    /// with its box taking keys -- so everything that presses them stays held
    /// off until the bag shows nothing waiting.
    std::atomic<bool>        trash_hold_{false};
    /// A stop ended that hold with nothing left to watch the bin, so nothing
    /// is started again until the game is seen with nothing waiting. Nor does
    /// the executor press anything meanwhile: it presses only while the packet
    /// bot runs, which that stop stopped, and it is paused besides.
    std::atomic<bool>        trash_left_held_{false};
    /// The game has been seen with nothing waiting since that stop: clears
    /// trash_left_held_ and lets the executor go on. From keys_may_start,
    /// from a trash worker's own look, and from watch_left_held.
    void bin_seen_clear();
    /// A fresh look at the game shows no dialog up and nothing waiting in the
    /// bin, by `store`'s pictures, which have to be there to tell the bin's
    /// dialogs by. When not, `why` -- if given -- gets why not, in
    /// Vietnamese.
    bool bin_looks_clear(const TemplateStore& store, std::string* why);
    /// From tick(): while a stop's doubt is left and no trash worker is
    /// running to look, bin_looks_clear every couple of seconds, so the doubt
    /// ends by itself once the player has answered the bin's dialog -- what
    /// waits on it goes on, and the executor's pause for it ends. What the
    /// stop stopped, the packet bot among it, waits for BẮT ĐẦU.
    /// The look runs on a thread of its own, with pictures of its own: it
    /// takes a quarter of a second, too long to hold the window up for.
    void watch_left_held();
    TemplateStore            left_held_store_;          ///< the look's own
    bool                     left_held_store_loaded_ = false;   ///< by the look
    std::future<bool>        left_held_look_;           ///< a look under way
    std::uint64_t            left_held_looked_ms_ = 0;
    /// One more for every stop that leaves the bin in doubt. A look's answer
    /// counts only for the doubt it was started for.
    std::atomic<unsigned>    left_held_gen_{0};
    unsigned                 left_held_look_gen_ = 0;
    /// Whether something that presses keys may start: the hunter, the macro,
    /// the packet bot, walking to a place, the calibration, or the simple
    /// screen's BẮT ĐẦU. Always, unless trash_left_held_ is set; then only
    /// once a fresh look at the game shows no dialog up and nothing waiting
    /// in the bin, which clears it. The same for a chat line Tự chat was
    /// stopped with in doubt, until a look shows it shut; and not while a
    /// Tự chat worker that was told to quit is still on its way out, as it
    /// may yet leave one. Says why not, on whichever screen is up. Every one
    /// of those starts goes through here, so that no way of starting one --
    /// F10, F11, a button on either screen -- gets round it.
    bool keys_may_start();

    /// Hold every other part of the bot off for `reason`, unless something
    /// else holds it already: one worker's hold is not another's to take
    /// over, nor theirs to end. Asked and taken under one lock, so two
    /// workers cannot both find it free.
    bool take_hold(const std::string& reason);
    /// End the hold, if it is still `reason`'s. While stop_jobs is under way
    /// it is the stop's from then on instead, and ends with the stop.
    void end_hold(const std::string& reason);
    std::mutex               hold_mutex_;
    bool                     stopping_ = false;     ///< stop_jobs under way; under hold_mutex_

    // ---- saying things in chat ----
    /// Says the lines in configs/chat.ini in the game's chat, one at a time on
    /// a timer, with every other part of the bot held off while a line is
    /// typed -- and for as long after as the chat line may be open, until a
    /// look shows it shut. One thread for the timed lines and for the chat
    /// tab's two buttons; started for a button alone, it does that and ends.
    void start_chat();                    ///< the timed lines, for a run
    void request_chat(bool send_one);     ///< look at the chat bar, or say one line now
    /// Stops the worker. A line half typed stops at its next key and is left
    /// in the game's chat line as it is -- nothing more is pressed into it; a
    /// look at whether the line is shut takes up to a second: with `wait`
    /// false this returns at once and the worker finishes on its own, joined
    /// by tick() or the next start, so the window does not freeze.
    ///
    /// `quit` is the player taking over -- DỪNG, F12, closing GoBot: a chat
    /// line still in doubt then is handed over (chat_handover_) rather than
    /// waited on, and nothing more is pressed into it. Without it -- Tự chat
    /// unticked -- the worker stays until it sees the line shut, holding
    /// everything off until then, and clears and closes the line itself once
    /// nobody has been at the mouse or keyboard for a second.
    void stop_chat(bool quit = true, bool wait = true);
    void chat_loop();
    void say_chat(const std::string& line);
    /// A line whose sending Enter Tự chat is about to press -- said, or on
    /// its way however late the game takes that Enter -- for the party worker
    /// (ChatSender::on_send_pressed): read back off the chat it is our own
    /// words, not somebody asking for an invitation. Kept in memory, and in
    /// SaidLately's file for a party watcher in another process: this GoBot
    /// started again, gobot_cli --autoparty (R8bR-1).
    void remember_said(const std::string& typed);
    /// The lines said in the last half hour, each with when, on now_ms()'s
    /// clock: for PartyChat::without_own. This run's, and those in
    /// SaidLately's file -- a run of GoBot before this one, gobot_cli --chat.
    std::vector<std::pair<std::string, std::uint64_t>> said_lately();
    /// When the last of those lines' sending Enter went down, a Gửi thử's and
    /// another process's among them, on now_ms()'s clock; 0 for none in the
    /// last half hour.
    std::uint64_t last_said_ms();

    std::thread              chat_thread_;
    std::atomic<bool>        chat_running_{false};
    std::atomic<bool>        chat_stop_{false};
    std::atomic<bool>        chat_quit_{false};       ///< stop_chat's `quit`
    /// A one-off, or a worker stopping, turned into a run: it starts over.
    std::atomic<bool>        chat_restart_{false};
    /// Held for turning a worker into a run, and by the worker on its way
    /// out, so that a run asked for then is not lost with the worker.
    std::mutex               chat_life_mutex_;
    std::atomic<bool>        chat_oneshot_{false};    ///< here for one asked-for look or line
    std::atomic<int>         chat_request_{0};        ///< 1 look at the bar, 2 say one line
    std::atomic<int>         chat_sent_{0};           ///< lines said this run
    /// When the next line is due, on now_ms()'s clock; 0 when none is.
    std::atomic<std::uint64_t> chat_next_ms_{0};
    /// When Tự chat's due line was last put off only because the trash worker
    /// had a job under way (a pass, the charms, the shop), on now_ms()'s clock;
    /// 0 when no line waits on it. The trash worker starts no timed job while
    /// this is set or a line is due -- the player's order: chat, then the bag,
    /// then the potions -- for kChatFirstMaxMs at most.
    std::atomic<std::uint64_t> chat_waits_trash_ms_{0};
    /// A line waits while HP is under this: what the HP potion fires at, or
    /// 50 when that is lower. Kept by tick() from the macro's settings, which
    /// belong to the drawing thread.
    std::atomic<int>         chat_hp_floor_{50};
    std::vector<std::string> chat_log_;               ///< guarded by worker_mutex_
    std::mutex               chat_sender_mutex_;
    ChatSender*              chat_sender_ = nullptr;  ///< the line under way, to cancel
    std::mutex               chat_said_mutex_;
    /// What was said, and when on now_ms()'s clock; guarded by chat_said_mutex_.
    std::vector<std::pair<std::string, std::uint64_t>> chat_said_;

    /// The game's chat line may be open: Tự chat could not show it shut and
    /// empty. With it open every key pressed goes into it -- a skill key is a
    /// letter, a potion a digit -- so the executor is paused while this is
    /// set, and the 'auto chat' hold kept on while a worker watches it.
    std::atomic<bool>        chat_doubt_{false};
    /// A worker is watching that doubt, with its hold on. Without one -- the
    /// worker was stopped by the player -- nothing that presses keys starts
    /// until a look shows the line shut (keys_may_start, watch_chat_doubt).
    std::atomic<bool>        chat_doubt_watched_{false};
    /// Set by a worker stopped with the line in doubt, as the last thing it
    /// does with it: tick() then stops everything else that presses keys and
    /// lets the worker's hold go.
    std::atomic<bool>        chat_handover_{false};
    /// One more for every doubt handed over. A look's answer counts only for
    /// the doubt it was started for.
    std::atomic<unsigned>    chat_doubt_gen_{0};
    unsigned                 chat_doubt_look_gen_ = 0;
    std::future<bool>        chat_doubt_look_;          ///< a look under way
    std::uint64_t            chat_doubt_looked_ms_ = 0;
    TemplateStore            chat_doubt_store_;         ///< the look's own
    bool                     chat_doubt_store_loaded_ = false;   ///< by the look
    /// The doubt is over: the executor goes on, unless the bin's doubt keeps it.
    void chat_seen_shut();
    /// `looks` fresh looks at the game, a frame or two apart, all show the
    /// chat line shut -- or there is no game, so no line to type into. When
    /// not, `why` -- if given -- gets why not, in Vietnamese.
    bool chat_line_looks_shut(const TemplateStore& store, int looks, std::string* why);
    /// From tick(): while a doubt is left with no worker watching it, a look
    /// every couple of seconds, on a thread of its own, so the doubt ends by
    /// itself once the player has shut the line -- what waits on it goes on,
    /// and the executor's pause for it ends. What the stop stopped, the packet
    /// bot among it, waits for BẮT ĐẦU.
    void watch_chat_doubt();

    /// The executor's buffs and potions paused while either doubt -- the bin's
    /// or the chat line's -- is left, and going again once neither is.
    void update_executor_pause();
    std::mutex               pause_mutex_;

    // ---- vitals ----
    /// Reads HP and MP off the on-screen bars. The only live game state
    /// available without decoding the protocol or reading the process.
    VitalsReader vitals_;
    bool         vitals_running_ = false;
    bool         picking_hp_ = false;      ///< next click samples the HP colour
    bool         picking_mp_ = false;
    int          vitals_zoom_ = 2;
    bool         vitals_show_full_frame_ = false;

    void apply_vitals_config();
    void start_vitals();
    void stop_vitals();
    void autodetect_bar(BarSpec& bar, const char* which);

    // ---- radar: seeing the monsters and going after them ----

    /// The game's own bitmap font, so plates are read rather than guessed at.
    /// Empty when the glyph set is not installed, and everything still works.
    ///
    /// Declared before the detector so that it outlives it: the detector reads
    /// the font from its own thread, and that thread stops when the detector
    /// is destroyed, which happens in reverse order of declaration.
    GlyphFont               font_;

    MobDetector             detector_;
    DetectorTuning          detect_tuning_;
    /// The hunter's eyes: the detector's plates, or the packet world's while
    /// "Vị trí từ gói tin" is ticked and the packets are healthy. Declared
    /// before the hunter, so that it outlives it.
    std::unique_ptr<PacketEyes> packet_eyes_;
    std::unique_ptr<Hunter> hunter_;
    HunterConfig            hunt_config_;
    bool                    detector_running_ = false;
    bool                    hunting_ = false;
    bool                    radar_tuning_open_ = false;
    bool                    radar_show_rejects_ = false;

    /// How much of the game's client area our own window is sitting on top of.
    ///
    /// This game refuses PrintWindow, so frames come off the screen — which
    /// means anything covering the game is captured instead of the game. It is
    /// the single most confusing way for the radar to fail, so it is measured
    /// and said out loud rather than left to produce nonsense.
    float overlap_fraction() const;
    void  move_clear_of_game();

    // ---- which map we are in ----
    MinimapTracker minimap_;
    MapBook        map_book_;
    char           map_name_[64] = "";
    std::uint64_t  map_fp_ = 0;

    // ---- where we are, and getting somewhere ----
    CameraTracker              camera_;
    EdgeAnchor                 anchor_;
    PlaceBook               places_;
    NavigatorConfig            nav_config_;
    std::unique_ptr<Navigator> navigator_;
    char                       place_name_[48] = "";

    void draw_movement_section();
    void draw_input_warning();
    void relaunch_elevated();

    /// Whether synthetic keystrokes reach the machine. Checked at startup and
    /// on demand: it changes when the game's anticheat starts and stops.
    bool input_injection_ok_ = true;
    void save_places();

    void draw_map_section();
    void draw_local_radar(const Nameplate* me, const std::vector<Nameplate>& plates,
                          const Nameplate* target, int frame_w, int frame_h);
    void save_map_book();

    void draw_radar_panel();
    void start_detector();
    void stop_detector();
    /// Refuses, and says why, while no character name is typed: the hunter
    /// finds you by your name alone, so without one there is no "you" to
    /// walk from -- F11, BẮT ĐẦU and the full screen's switch alike.
    void start_hunt();
    void stop_hunt();
    void toggle_hunt();
    void save_hunt_config();
    /// Whether a character name is typed, more than spaces.
    bool hunt_named() const {
        return hunt_config_.character_name.find_first_not_of(" \t") != std::string::npos;
    }

    /// Draw a region of the last captured frame and return where it landed, so
    /// the caller can overlay on it and map clicks back to pixels.
    struct FrameView {
        bool  drawn = false;
        float ox = 0.0f, oy = 0.0f;       ///< top-left, in screen coordinates
        float scale = 1.0f;
        int   x0 = 0, y0 = 0;             ///< frame pixel at that corner
    };
    FrameView draw_frame_region(const Frame& f, int x0, int y0, int w, int h,
                                float scale, const char* id);

    // ---- map mapper ----
    std::unique_ptr<MapMapper> mapper_;
    MinimapSpec                minimap_spec_;
    bool                       mapper_running_ = false;
    char                       map_label_[64] = "";

    /// Calibration: the next click inside the minimap view samples the colour
    /// under the cursor as the marker colour.
    bool picking_marker_ = false;

    // ---- game map files ----
    MapFile                  map_file_;
    std::string              map_dir_;
    std::vector<std::string> map_stems_;
    char                     map_filter_[64] = "";
    int                      map_tile_size_ = 8;
    int                      map_layer_mask_ = 3;
    std::string              map_status_;
    void draw_mapfile_panel();

    // ---- protocol calibration ----
    std::unique_ptr<ProtocolCalibrator> calibrator_;
    std::vector<FieldFinding>           findings_;
    bool                                calib_ran_ = false;
    char                                calib_cast_key_[24] = "1";
    bool                                calib_held_ = false;   ///< "calibrating" held for a run
    void draw_calibrate_panel();
    void apply_findings();
    /// The Calibrate tab's run: the game brought to the front -- clicking the
    /// button put GoBot's own window there, and the calibration refuses a game
    /// that is not in front, so it could never start -- and everything else
    /// that presses keys held off for its twenty seconds, under the
    /// "calibrating" hold. Refused while hunting, walking to a place or the
    /// packet bot runs: they steer the character too. False, and said why,
    /// when it did not start.
    bool start_calibration();
    /// Stopped and waited for, and its hold let go: Abort, stop_jobs (F12,
    /// DỪNG, a failed revive, closing GoBot), the trash and chat holds, and
    /// tick() once a run has ended by itself.
    void stop_calibration();

    FramingSpec framing_;
    bool        framing_verified_ = false;
    /// [Framing] as protocol.ini has it, kept when Autopilot applies the
    /// framing it detected -- which never has the 0x0081 rule, and which
    /// push_framings takes the rule back from.
    FramingSpec configured_framing_;

    /// What the client sends is framed differently -- the opcode sits at offset
    /// 2 coming down and offset 6 going up. A capture given only `framing_`
    /// decodes the server's half and grinds the client's half into resync
    /// noise, a byte at a time.
    FramingSpec client_framing_;
    bool        has_client_framing_ = false;

    /// Hand the capture both directions' framing. Use instead of calling
    /// `capture_.set_framing()` directly, so the client half is never dropped.
    /// The server's unpacks every 0x0081, so that the frames inside -- the
    /// map's monster table among them -- reach the packet world whole.
    void push_framings();

    // ---- positions from the packets ("Vị trí từ gói tin") ----
    /// The capture the positions come from, started by start_hunt whenever
    /// packet_positions is not Off: the game's own TCP and UDP, by its
    /// process (its window's), with every frame unpacked and none filtered
    /// out. A capture already running -- the Protocol tab's, Autopilot's, the
    /// packet bot's -- is used as it is. One that will not start is said once
    /// in the log, and the hunt goes on from the screen.
    void start_position_capture();
    /// Stops the capture start_position_capture started, unless the packet
    /// bot, Autopilot, a recording or the calibration is using it now: then
    /// it stays, and positions_tick stops it once they are done. Never one
    /// somebody else started.
    void stop_position_capture();
    /// From tick(): while hunting with packet_positions not Off, a comparison
    /// a second, the "Positions" line every 10 s, and the capture kept
    /// attached; the capture stopped once hunting is not.
    void positions_tick();
    /// One look at the packets beside the screen, for the next line.
    void packet_watch_sample(std::uint64_t now);
    /// The "Positions" line in gobot_gui.log: the packet world's counts, the
    /// capture's, what the packets say beside what the screen shows, and
    /// where the hunter's positions came from.
    void packet_watch_tick();
    /// A line in gobot_gui.log for each map entry the packets have had since
    /// the last look: the place and where it put the character.
    void note_map_entries();
    /// One line in gobot_gui.log of where the packets have the character and
    /// which map, `when` -- after a revive, after logging back in -- so the
    /// event can be read against the position; "no capture" with none.
    void note_where(const char* when);
    /// (map, area) as the game names the place, for the log: its .map file's
    /// stem t<map>_s<area>, the name the game's list of places (tb_Map.tbl)
    /// gives that theme and stage, and the file's size. That a map entry's
    /// (map, area) is tb_Map's (theme, stage) is inferred -- GoC names its
    /// files by it -- and these lines are how a hunt checks it.
    std::string place_text(std::uint16_t map, std::uint16_t area);
    /// Whether world `x`, `y` lies on (map, area)'s .map file, with 64 px to
    /// spare: 1 on it, 0 off it, -1 not known (no file).
    int on_place(std::uint16_t map, std::uint16_t area, float x, float y);
    /// The capture started again when the game's process, connections or UDP
    /// ports change, or its TCP goes quiet on it (should_reattach); one that
    /// would not start is tried again, at most every 10 s -- and one the
    /// capture itself refused, only for a new game link or after
    /// position_backoff_ms_.
    void position_reattach(std::uint64_t now);
    /// Say `what` in the log, unless it was the last thing said about the
    /// capture for positions: a capture that cannot start is said once.
    void position_say(const std::string& what);
    /// Whether another part reads the capture now: the packet bot, Autopilot
    /// part-way, a recording or the calibration. The capture for positions is
    /// then neither stopped nor started again from under it.
    bool capture_shared();

    bool          position_capture_ = false;    ///< the running capture is the one started for positions
    /// The capture was stopped by hand (stop_capture): not started again for
    /// positions until the next hunt starts.
    bool          position_capture_off_ = false;
    /// What the last start found of Npcap, for the reason PacketEyes gives.
    std::atomic<bool> position_npcap_ok_{true};
    std::string   position_said_;               ///< the last thing position_say said
    GameLink      position_link_;               ///< what the capture for positions was built from
    std::uint64_t position_tried_ms_ = 0;       ///< the last start, or try at one
    /// The last start got as far as the capture, and the capture refused it:
    /// tried again at once only for a new game link, otherwise once
    /// position_backoff_ms_ has gone by, which doubles at every refusal up to
    /// kPositionBackoffMaxMs. A new hunt tries afresh.
    bool          position_start_failed_ = false;
    std::uint64_t position_backoff_ms_ = GameLink::kReattachEveryMs;
    static constexpr std::uint64_t kPositionBackoffMaxMs = 600'000;
    std::uint64_t position_checked_ms_ = 0;     ///< the last look at the game's sockets
    std::uint64_t packet_watch_ms_ = 0;         ///< the last "Positions" line; 0 before the first
    std::uint64_t packet_sample_ms_ = 0;
    std::uint64_t packet_entry_said_ms_ = 0;    ///< the last map entry note_map_entries said, by its time
    /// The game's list of places, read once at the first place_text; and each
    /// place's .map size in px by its stem, 0 x 0 for a file not found.
    MapTable                                    map_table_;
    bool                                        map_table_tried_ = false;
    std::map<std::string, std::pair<int, int>>  place_sizes_;
    /// Since when a "logged in" line is owed (note_where): set as the keeper
    /// brings the game back, said by positions_tick once the capture follows
    /// the new game and has the character, or kWherePendingMs on. 0: none.
    std::uint64_t where_pending_ms_ = 0;
    static constexpr std::uint64_t kWherePendingMs = 60'000;
    std::vector<ViewComparison> packet_looks_;  ///< since the last line
    std::vector<int>            packet_target_gaps_;   ///< |dx from packets - dx from the screen|, the target's
    PacketEyes::Stats           packet_eyes_seen_{};   ///< PacketEyes' stats at the last line

    // ---- UI state ----
    bool quit_ = false;
    int  active_tab_ = 0;

    /// Hides the panels that only matter while reverse-engineering the
    /// protocol. They are most of the tabs and none of what you need to run a
    /// macro, and leaving them in the way made the app read as broken.
    bool advanced_ = false;

    std::vector<Interface> interfaces_;
    int                    selected_interface_ = -1;

    std::vector<InputManager::WindowInfo> windows_;
    bool                                  window_list_loaded_ = false;

    char server_ip_[64]   = "";
    char server_port_[16] = "";
    char bpf_filter_[256] = "";
    char window_title_[128] = "GhostOnline";

    bool  capture_running_ = false;
    bool  bot_running_ = false;
    bool  elevated_ = true;
    void* main_hwnd_ = nullptr;
    /// The window retarget() last pointed everything at, to tell a new window
    /// from the same one chosen again.
    void* followed_window_ = nullptr;
    /// check_game_scaling's last look, when it was, and what would cure it --
    /// with the game's program, for the cures done by hand.
    WindowScaling game_scaling_{};
    std::uint64_t game_scaling_looked_ms_ = 0;
    ScaleCure     game_scaling_cure_ = ScaleCure::None;
    std::string   game_scaling_program_;
    /// GoBot held to Windows' scaling: whether an override on its own exe is
    /// what holds it (looked at with the game), and whether it was taken off.
    bool          own_override_found_ = false;
    bool          own_scaling_fixed_ = false;

    /// The packet bot to be stopped, asked for off the drawing thread -- a
    /// death read off HP, on the capture thread; the game's traffic gone quiet,
    /// on the capture's watchdog -- and done by tick(), where it is started and
    /// stopped, and where "Keep botting after death" is read. Stopped on those
    /// threads, a stop landing inside a start was lost, and the bot ran on a
    /// dead connection.
    std::atomic<bool> bot_died_{false};
    std::atomic<bool> traffic_lost_{false};

    // ---- protocol discovery state ----
    /// Raw server->client bytes held for the framing detector. Capped: the
    /// detector only reads the first few hundred KB and an unbounded buffer
    /// would grow for the whole session.
    std::mutex                mutex_;
    std::vector<std::uint8_t> raw_sample_;
    bool                      recording_ = false;
    static constexpr std::size_t kMaxSample = 512 * 1024;

    std::vector<FramingScore> detect_results_;
    bool                      detect_ran_ = false;

    struct OpcodeStat {
        std::uint64_t count = 0;
        std::uint64_t to_server = 0;
        std::uint64_t to_client = 0;
        std::size_t   last_size = 0;
        std::size_t   min_size = SIZE_MAX;
        std::size_t   max_size = 0;
    };
    std::map<std::uint16_t, OpcodeStat> opcode_stats_;

    /// Frames from inside 0x0081 or over UDP, which on_packets counts and
    /// keeps out of every table and of the packet bot. The packet world,
    /// which reads them by their own rules, has them all.
    std::uint64_t held_back_frames_ = 0;

    /// Recent payloads per opcode, for the inspector and the field analyzer.
    std::map<std::uint16_t, std::deque<Bytes>> samples_;
    static constexpr std::size_t kMaxSamplesPerOpcode = 256;

    int  selected_opcode_ = -1;      ///< -1 = none
    bool opcode_filter_mapped_only_ = false;

    // ---- layout editor ----
    char layout_name_[64]   = "";
    char layout_fields_[512] = "";
    int  layout_semantic_   = 0;
    char layout_repeat_field_[64] = "";

    // ---- log panel ----
    int  log_level_filter_ = 2;      ///< index into the level combo
    bool log_autoscroll_ = true;

    // ---- the simple screen ----
    //
    // What a player sees first. One screen, in Vietnamese, in the order things
    // have to happen: get ready, choose what the bot does, start it. It drives
    // the same engines as everything else; the full window is one click away
    // behind "Nâng cao", and has a button back. Lives in simple_view.cpp.
    void draw_simple_view();
    void simple_checklist();
    void simple_vitals();
    void simple_jobs();
    void simple_start_block();
    void simple_window_list();

    /// Everything the player ticked, on or off. One button and one key.
    void simple_start();
    void simple_stop();
    void simple_toggle();
    /// What simple_start starts once there is a game to start it on.
    void simple_start_jobs();
    /// BẮT ĐẦU with the game closed and "Tự vào lại game" ticked: the keeper
    /// opens the game and logs in, as GoC does from a missing window, and
    /// everything else that is ticked starts once it is back in the world.
    bool simple_cold_start_ = false;
    bool simple_running() const {
        return macro_running_ || hunting_ || party_running_ || (keeper_running_ && !keeper_oneshot_) ||
               revive_running_ || (trash_running_ && !trash_oneshot_) ||
               (chat_running_ && !chat_oneshot_ && !chat_stop_) ||
               (quest_running_ && !quest_oneshot_) || popups_running();
    }

    /// The tabs. Fighting is what the whole screen used to be.
    void simple_fight_tab();
    void simple_party_tab();
    void simple_login_tab();
    void simple_trash_tab();
    void simple_chat_tab();

    /// The chat tab's copy of configs/chat.ini, and the box its lines are
    /// edited in.
    ChatSettings      simple_chat_;
    bool              simple_chat_loaded_ = false;
    char              simple_chat_lines_[4096] = "";
    int               simple_chat_interval_ = 120;
    std::uint64_t     simple_chat_tested_ms_ = 0;   ///< the last Gửi thử, for its thirty seconds
    /// What the tab warns of, worked out every few seconds rather than every
    /// frame: a look at every process running is not free.
    std::uint64_t     simple_chat_checked_ms_ = 0;
    std::string       simple_chat_typers_;          ///< Unikey and the like, running
    std::uint16_t     simple_chat_layout_ = 0;      ///< the game's keyboard layout

    /// The trash tab's copy of configs/trash.ini and of the pictures, and the
    /// bag as last photographed for picking from.
    CleanSettings     simple_clean_;
    TrashList         simple_trash_;
    bool              simple_clean_loaded_ = false;
    TemplateStore     simple_store_;
    bool              simple_store_loaded_ = false;
    Frame             simple_bag_frame_;
    BagView           simple_bag_view_;
    bool              simple_bag_shot_ = false;
    std::vector<int>  simple_bag_match_;   ///< per slot: the list item it is, or -1
    int               simple_pick_slot_ = -1;
    void simple_rematch_bag();

    /// The client's item icons, for choosing what to throw away beforehand.
    ItemCatalog       simple_catalog_;
    bool              simple_catalog_loaded_ = false;
    std::string       simple_pick_id_;     ///< the icon whose menu is open
    /// That icon's drawing in other colours, which go with it when it is ticked.
    std::vector<std::string> simple_pick_kin_;
    /// Read the list again: pictures cut from the bag, and the icons ticked.
    void simple_reload_trash();
    /// Write the icons ticked to [Items] in configs/trash.ini.
    void simple_save_ids();

    /// The simple screen's own switches, kept in [Simple] in configs/app.ini
    /// and written key by key so the rest of that file is left as it is.
    struct SimpleOptions {
        bool keep_alive = false;   ///< watch the client; log back in when it goes
        bool revive = false;       ///< press the revive button when the character dies
        bool refocus = true;       ///< keep the game in front while the bot runs
    };
    SimpleOptions simple_opts_;
    void load_simple_options();
    void save_simple_option(const char* key, bool value);

    /// Bring the game back to the front during a run whenever something else
    /// has taken it -- at once, GoBot's own window included, but never in the
    /// middle of a click (FrontKeeper). The keys only land while the game is
    /// in front, so a notification that steals focus at 2am otherwise stops
    /// the bot until morning. Called from tick().
    void keep_game_in_front();
    /// The keeper's crossing is under way: VTCZone, the launcher and the game
    /// are its own to bring forward.
    bool keeper_crossing();
    FrontKeeper   front_keeper_;
    std::uint64_t front_said_ms_ = 0;      ///< when the log last said the game was brought back
    int           front_brought_ = 0;      ///< and how often since
    bool          front_refusal_said_ = false;
    bool          front_hung_said_ = false;    ///< the log said a window was not answering
    /// The key a key box took while GoBot had the keys, and when: down until
    /// it reads up (for up to kSettleMaxMs), so the game does not get it.
    std::uint32_t front_key_ = 0;
    std::uint64_t front_key_ms_ = 0;

    /// The party tab's copy of the rules: read once, written key by key.
    Party::Settings simple_party_;
    bool            simple_party_loaded_ = false;
    char            simple_accept_list_[1024] = "";
    char            simple_invite_list_[1024] = "";
    char            simple_black_list_[1024] = "";
    char            simple_keyword_[32] = "";

    /// What the login tab last saw: how many of the crossing's pages have
    /// their picture loaded, and whether VTCZone is signed in. Taken off the
    /// drawing thread every few seconds while the tab is up, because loading
    /// the pictures takes seconds and a look at VTCZone is a capture and a
    /// few matches -- too slow for every frame.
    enum class VtcZone { Unknown, NotRunning, SignedIn, SignInForm };
    struct LoginLook {
        bool        done = false;       ///< a look has finished
        std::size_t pages = 0;          ///< in the table
        std::size_t recognised = 0;     ///< of them, with their picture loaded
        VtcZone     vtczone = VtcZone::Unknown;
    };
    void simple_login_look();
    LoginLook                login_seen_;
    std::uint64_t            login_looked_ms_ = 0;
    /// The look's own pictures. Declared before the look so the look is gone
    /// before they are.
    TemplateStore            login_store_;
    bool                     login_store_loaded_ = false;   ///< by the look
    std::future<LoginLook>   login_look_;
    /// The tab has tried to write a missing configs/login.ini, and not tried
    /// again since: cleared once there is a file.
    bool                     login_file_tried_ = false;

    /// Notice a run starting or stopping, however it happened -- the button,
    /// the keys, the panic key, the full screen -- and start or freeze the
    /// tallies under the button. Called from tick(), which runs even while
    /// the game covers this window and nothing is drawn.
    void simple_track_run(bool running);

    /// After a change on the simple screen: hand it to whatever is running,
    /// start what now has something to do, and write it to disk. That screen
    /// has no Save button; a setting a player has to remember to save is a
    /// setting they lose.
    void simple_changed();

    /// A box showing one key. Click it, then press a key; Delete empties it
    /// when `can_clear`, Esc leaves it as it was. True on the frame it changed.
    bool simple_key_box(const char* id, std::uint32_t& vk, bool can_clear, bool dimmed,
                        const char* tip);

    /// A line of Vietnamese saying what just happened, under the start
    /// button. Kept apart from set_status(), whose messages come from the
    /// engines and are in English.
    void simple_note(const std::string& msg, std::uint32_t ms = 6000);

    ImFont*       text_font_ = nullptr;
    ImFont*       strong_font_ = nullptr;
    bool          simple_view_ = true;
    bool          simple_seeded_ = false;        ///< potion keys made sure of
    bool          simple_reader_tried_ = false;  ///< HP/MP reader started for this window
    bool          simple_find_bars_ = false;     ///< look for the bars on the next frame
    std::uint64_t simple_find_bars_since_ms_ = 0;
    bool          simple_pick_window_ = false;   ///< open the window list
    std::uint32_t simple_listening_ = 0;         ///< ImGui id of the key box waiting for a key
    /// A key just added with +, whose box should wait for its key as soon as
    /// it is drawn. Pressing + and then the key is the whole of adding one.
    std::size_t   simple_listen_key_ = static_cast<std::size_t>(-1);
    std::string   simple_note_;
    std::uint64_t simple_note_until_ms_ = 0;
    /// Why the bot stopped itself, in red on the card until the next BẮT ĐẦU.
    std::string   simple_stopped_vi_;
    bool          simple_was_running_ = false;
    std::uint64_t simple_started_ms_ = 0;
    std::uint64_t simple_stopped_ms_ = 0;
    std::uint64_t simple_base_finished_ = 0;     ///< the hunter's tally when the run began
    std::vector<std::uint64_t> simple_base_presses_;   ///< the macro's, per key
    /// The keys those were counted against, so a key added or taken out
    /// during the run is matched by which key it is (macro_key_match).
    std::vector<MacroKey> simple_base_keys_;

    // ---- alerts ----
    /// When something needs a person -- the character died, the game went,
    /// the bot stopped itself -- one short message to the player's Discord,
    /// and a beep and a Windows notification here. Lives in simple_alerts.cpp.
    ///
    /// The one way any part of GoBot raises an alert, from any thread: it
    /// writes the one 'ALERT <event>: <text>' line to the log and hands the
    /// rest to the alert service, which neither waits nor touches the network
    /// on the caller's thread. `event` is one of alert_events()' keys.
    void alert(const char* event, const std::string& text_vi);
    /// Right after "ALERT death": the macro's black box -- HP and MP as read
    /// and each potion key's decisions over the last 30 s -- and what the
    /// potion slots last showed, with the HP and MP figures, into the log.
    void log_black_box();
    /// A party joined or a player invited: the name goes only when the
    /// Thông báo tab allows other players' names.
    void alert_party(bool joined, const std::string& who);
    void alerts_init();          ///< from initialise(): the settings and the saved webhook
    void alerts_tick();          ///< from tick(): what is noticed by watching, and the beeps
    void alerts_shutdown();      ///< from ~GuiApp, once every worker has stopped
    void simple_alerts_tab();
    /// The tab's dot: alerts on, and Discord not refusing the webhook.
    bool alerts_dot() const;
    struct AlertsUi;
    struct AlertsUiFree { void operator()(AlertsUi* ui) const; };
    std::unique_ptr<AlertsUi, AlertsUiFree> alerts_ui_;

    // ---- quick quest ----
    /// "Làm nhiệm vụ nhanh", as GoC does it for this client: every few minutes
    /// of a run, open the Nhiệm Vụ window, go to NV Phụ and tick the game's own
    /// Tự Động there if it is off, with everything else held off for those few
    /// seconds. One thread for the timed rounds and for the tab's two buttons;
    /// started for a button alone, it does that and ends. Lives in
    /// simple_quest.cpp.
    void start_quest();                   ///< the timed rounds, for a run
    void request_quest(bool look_only);   ///< one round now, or a look that presses nothing
    void stop_quest(bool wait = true);    ///< at once: nothing more is pressed; `wait` as for stop_party
    void quest_loop();
    void say_quest(const std::string& line);
    void simple_quest_tab();
    std::thread              quest_thread_;
    std::atomic<bool>        quest_running_{false};
    std::atomic<bool>        quest_stop_{false};
    std::atomic<bool>        quest_oneshot_{false};   ///< here for one asked-for round or look
    std::atomic<bool>        quest_restart_{false};   ///< a one-off turned into a run
    std::atomic<int>         quest_request_{0};       ///< 1 one round now, 2 look only
    /// Held for turning a worker into a run, and by the worker on its way out.
    std::mutex               quest_life_mutex_;
    struct QuestUi;
    struct QuestUiFree { void operator()(QuestUi* ui) const; };
    std::unique_ptr<QuestUi, QuestUiFree> quest_ui_;

    // ---- GoBot's own keys ----
    /// F10, F11 and F12, and the two that do one job at once -- "Dọn túi
    /// ngay" and "Vào game ngay" -- read every tick and acted on by which
    /// window is in front (gobot/hotkeys.hpp). Lives in simple_hotkeys.cpp.
    /// Returns what tick() is to do -- start/stop, hunting, the panic stop --
    /// and does the two new keys' jobs itself, unless the panic key went down
    /// in the same tick: then nothing else is acted on.
    HotkeyActions hotkeys_tick();
    void load_hotkeys();                        ///< from initialise()
    /// Why `got` may not be a game key, in Vietnamese: one of GoBot's own
    /// switches other than `mine`, the box's own key. Empty when it may.
    std::string own_key_clash_vi(std::uint32_t got, std::uint32_t mine) const;
    /// Every key GoBot presses or watches, and what for: the new keys' boxes
    /// refuse each of them.
    std::vector<NamedKey> game_keys_in_use();
    void simple_hotkey_box(bool trash);         ///< "Phím dọn ngay", or "Phím vào game ngay"
    std::string simple_hotkeys_line() const;    ///< the keys line's part for them
    /// "Vào game ngay": one crossing, by the keeper watching or by a keeper
    /// started for it alone; pressed again during it, that crossing stopped.
    void login_now();
    void cancel_login_now();
    HotkeyConfig      hotkeys_;
    HotkeyEdgeTracker hotkey_edges_;
    /// Whether the game was the window in front at the last tick: losing it
    /// lets go of every key held there (tick).
    bool              game_was_front_ = false;
    /// The last "this key is one GoBot presses itself" said, and when: said
    /// again only when it changes, or ten minutes on.
    std::string       hotkey_taken_said_;
    std::uint64_t     hotkey_taken_ms_ = 0;

    // ---- the keeper's look between crashes ----
    /// What keeper_loop does after a look, besides what CrashWatch says.
    /// Lives in simple_keeper.cpp.
    enum class KeeperWake {
        None,       ///< nothing more: go on as CrashWatch says
        Stalled,    ///< nobody in the world for too long: end the client, as for a crash
        Quit,       ///< a keeper started for "Vào game ngay" alone is done
    };
    /// `move` is what CrashWatch made of the look `seen`. Stalled puts in
    /// `seen` the look the client is ended by.
    KeeperWake keeper_wake(CrashWatch::Move move, CrashWatch& watch,
                           const CrashWatch::Settings& watch_settings,
                           AutoLogin::Settings& login, TemplateStore& store,
                           CrashWatch::Look& seen);
    /// The login key's crossing, on the keeper's thread: a look for the world
    /// first, then run_crossing(Crossing::Key). False when the keeper is to
    /// stop: started for the key alone and done, stopped, or a crossing that
    /// gave up.
    bool keeper_key_crossing(CrashWatch& watch, const CrashWatch::Settings& watch_settings,
                             AutoLogin::Settings& login, TemplateStore& store);
    std::atomic<bool> keeper_login_now_{false};    ///< the login key asked for a crossing
    /// That crossing is under way; set and cleared under keeper_flow_mutex_.
    std::atomic<bool> keeper_key_crossing_{false};
    std::atomic<bool> keeper_key_cancel_{false};   ///< and the key was pressed again
    /// Started for the login key alone: it ends in the world, and is no run.
    std::atomic<bool> keeper_oneshot_{false};
    /// The watchdog, for a game that runs with nobody in the world: the keeper
    /// thread's alone, made afresh for each keeper (start_keeper).
    struct KeeperUi;
    struct KeeperUiFree { void operator()(KeeperUi* ui) const; };
    std::unique_ptr<KeeperUi, KeeperUiFree> keeper_ui_;
    /// After a crossing gets in: the watchdog counts afresh, after its grace.
    void keeper_stall_restart();
    /// Its tick and minutes, under "Tự vào lại game" on the Tự đăng nhập tab.
    void simple_stall_option();
    bool stall_tab_loaded_ = false;
    bool stall_tab_on_ = false;
    int  stall_tab_min_ = 5;

    // ---- potion stock ----
    /// "Còn N bình" under each potion row: the number on each potion key's
    /// hotbar slot, read off the HP reader's frames every few seconds of a
    /// run, and the "Sắp hết bình" and "Hết bình" alerts. Look only. Lives in
    /// simple_potions.cpp.
    void potions_tick();                            ///< from tick()
    void simple_potion_stock(const MacroKey& key);  ///< under that key's tier of a potion row
    void simple_potion_warn_box();                  ///< "Báo khi còn dưới", once, under the MP row
    struct PotionsUi;
    struct PotionsUiFree { void operator()(PotionsUi* ui) const; };
    std::unique_ptr<PotionsUi, PotionsUiFree> potions_ui_;
    /// For the black box written after a death (alert): what each potion
    /// key's slot was last believed to hold, and how long before `now` a look
    /// read it so -- "Potion slots: key 1 16 (read 31 s earlier; last look 2 s
    /// earlier: no hotbar), key 2 40 (read 1 s earlier)" (PotionSlotLog). A
    /// press the black box shows with the count unchanged is one the game did
    /// not drink. Empty when nothing was read this run. From any thread.
    std::string potion_slots_line(std::uint64_t now) const;
    mutable std::mutex          potion_slots_mutex_;
    PotionSlotLog               potion_slots_;      ///< under potion_slots_mutex_, on now_ms()'s clock

    // ---- closing stray popups ----
    /// "Tự đóng thông báo": during a run, the few popups on a fixed list --
    /// the check-in reward's notice, Yêu Quái Bách Khoa, the ESC menu GoBot's
    /// own ESC brought up -- closed by their own controls, once they have
    /// stayed up 3 s and nothing else is up (gobot/popup_closer.hpp). Lives in
    /// simple_popups.cpp.
    void start_popups();            ///< for a run; nothing when it is unticked
    /// `wait` as for stop_party.
    void stop_popups(bool wait = true);
    bool popups_running() const;
    void popup_loop();
    void popups_load();             ///< [Simple] close_popups, once
    void say_popups(const std::string& vi);
    void simple_popups_row();       ///< its row on the Đánh quái tab
    std::string simple_popups_line() const;     ///< its line on the state card
    struct PopupsUi;
    struct PopupsUiFree { void operator()(PopupsUi* ui) const; };
    std::unique_ptr<PopupsUi, PopupsUiFree> popups_ui_;

    // ---- the party HUD ----
    /// The party as the game draws it (gobot/party_hud.hpp). The party worker
    /// invites only when an invite can work -- not in a full party, and
    /// leading when "Chỉ mời khi đang làm chủ tổ đội" is ticked -- and not
    /// while anything else holds the game; the Tổ đội tab says what it saw.
    /// Lives in simple_party.cpp.
    /// One asker, on the party worker's thread, before the mouse is touched:
    /// false, and said why, when the invite is not to go. `look`: the
    /// worker's look at the game this pass, with what was so of it when it
    /// was grabbed (take_party_look), held up against party_events().
    bool party_may_invite(const std::string& who, const PartyLook& look, const TemplateStore& store,
                          bool always_leader);
    /// Every few seconds of the party worker, the HUD read off its look for
    /// the Tổ đội tab.
    void party_hud_tick(const PartyLook& look, const TemplateStore& store);
    /// What GoBot itself has done to the party, and when: the party worker's
    /// own answers and invites. A frame grabbed before the last of them is
    /// stale for every decision on the party -- the worker's, Tự chat's,
    /// "Thử xem".
    PartyEvents& party_events();
    /// The party by the game's own word, for those same decisions: the one
    /// place P0-3 fills from the game's packets. Nothing reads it yet.
    PartyMembership party_from_game() const;
    void simple_party_leader_option();      ///< "Chỉ mời khi đang làm chủ tổ đội"
    void simple_party_hud_line();           ///< "Tổ đội: …" on the state card, and "Thử xem"
    struct PartyHudUi;
    struct PartyHudUiFree { void operator()(PartyHudUi* ui) const; };
    static PartyHudUi* make_party_hud_ui();
    /// Made with the window, before any worker that reads it starts.
    std::unique_ptr<PartyHudUi, PartyHudUiFree> party_hud_ui_{ make_party_hud_ui() };

    // ---- Tự chat's "Chỉ khi chưa có tổ đội" ----
    /// On Tự chat's thread, for the line whose turn it is: when that line is
    /// ticked and the party HUD, read on a fresh look, does not show the
    /// player alone -- no member's panel, no crown, by the rule the tick's
    /// invite goes by (read_party, alone_for_chat) -- it is passed over for
    /// the next -- or, with no other line to go instead, put off a minute --
    /// and true comes back, with nothing held or pressed for it. Lives in
    /// simple_chat_alone.cpp.
    bool chat_alone_put_off(const ChatSettings& s, int index, const TemplateStore& store,
                            ChatSchedule& schedule, std::uint64_t now);
    /// The tick after preview row `i` of the Tự chat tab, `lines` being the
    /// box's lines as they are now.
    void simple_chat_alone_tick(const std::vector<std::string>& lines, std::size_t i);
    /// The box of lines edited: saved, each line keeping its tick by its words.
    void simple_chat_save_lines(const std::string& cfg);
    struct ChatAloneUi;
    struct ChatAloneUiFree { void operator()(ChatAloneUi* ui) const; };
    static ChatAloneUi* make_chat_alone_ui();
    std::unique_ptr<ChatAloneUi, ChatAloneUiFree> chat_alone_ui_{ make_chat_alone_ui() };

    // ---- "Dùng" in Dọn rác ----
    /// Registering fragments in Yêu Quái Bách Khoa with the trash pass
    /// (gobot/bag.hpp, TrashList::Action::Use): the options in the Dọn rác
    /// tab's two pickers and what a pass says of it. Lives in
    /// simple_trash_use.cpp.
    /// The catalog picker's options for fragment `id`; true when one was taken.
    bool simple_use_options(const std::string& id, const char* folder);
    /// The bag picture's option for the slot picked, on Tiêu Dùng only.
    bool simple_use_cut_option(const ScreenRect& at);
    /// " Đã dùng N món." for the trash worker's line about a pass.
    static std::string trash_use_said_vi(const BagCleaner::Result& r, bool look_only);
    /// "Dùng mảnh ghép", [Trash] use_fragments: the one switch registering
    /// goes by, off as shipped. `put` writes a [Trash] key of trash.ini.
    void simple_use_switch(const std::function<void(const char*, const std::string&)>& put);
    /// "Dùng bùa", [Trash] use_charms, and how soon after one is used the next
    /// may be (charm_every_minutes): the charms' own switch, off as shipped.
    /// The worker's charm job reads them (trash_loop); here they start and end
    /// the worker with the rest of what is ticked.
    void simple_charm_switch(const std::function<void(const char*, const std::string&)>& put);
    /// The line under the switch: when the charms on the list were last used,
    /// and when they next may be. Empty when there are none or none was used.
    std::string simple_charm_line();
    /// Whether the pictures a charm run is held to are beside GoBot.
    bool simple_can_use_charm();
    std::string       simple_charm_line_;
    std::uint64_t     simple_charm_line_ms_ = 0;
    /// A charm run now, for the button "Dùng bùa ngay": a pass of its own,
    /// the same worker, the clock not asked.
    void request_charms();
    /// The potion shop (BagCleaner::Mode::ShopBuy), now: "Mua ngay" (the time
    /// is not asked), or with `dry` "Thử mua" -- everything up to the quantity
    /// box, which is answered Hủy Bỏ, nothing typed and nothing bought. The
    /// same worker. Lives in simple_shop.cpp's card.
    void request_shop(bool dry);
    /// The card "Tự mua bình" of the Dọn rác tab: the switch, the time, the
    /// thresholds and limits, the two buttons.
    void simple_shop_card(const std::function<void(const char*, const std::string&)>& put);
    /// Whether the pictures a purchase is held to are beside GoBot.
    bool simple_can_buy();


    // ---- the monster lists ----
    /// "Lọc quái" on the Đánh quái tab: which monsters are never attacked and
    /// which go first (HunterConfig's skip_names / first_names, matched by
    /// gobot/monster_list.hpp), picked from "Quái đang thấy" -- the names the
    /// detector reads -- or typed. Look only: "Nhìn quanh" starts the
    /// detector, which reads the screen and presses nothing. Lives in
    /// simple_lists.cpp.
    void simple_monster_row();          ///< its row, under "Tự đánh quái"
    void simple_monster_lists();        ///< the "Danh sách quái" window, when asked for
    /// From tick(): the names read lately, and the end of a look -- here
    /// rather than in the window, which is not drawn while the game covers it.
    void simple_lists_tick();
    struct ListsUi;
    struct ListsUiFree { void operator()(ListsUi* ui) const; };
    std::unique_ptr<ListsUi, ListsUiFree> lists_ui_;

    // ---- skills that follow the fight ----
    /// "Chỉ khi đang đánh quái" and "Chi tiết…" on the "Tự dùng kỹ năng" row,
    /// the "Kỹ năng" window, and the key sequence said where a player sees it
    /// (MacroConfig::skills_follow_fight). Lives in simple_skills.cpp.
    void simple_skills_line();          ///< after the row's "+"
    void simple_skills_popup();         ///< the "Kỹ năng" window, when asked for
    void simple_sequence_note();        ///< "Chuỗi phím … đang bật", with its "Tắt"
    /// What the state card adds about the skills: held for MP while
    /// attacking, or waiting until the character is seen.
    std::string simple_skills_note(bool no_player);
    /// Whether "Phím đánh" is also an enabled skill key.
    bool attack_key_is_skill() const;
    struct SkillsUi;
    struct SkillsUiFree { void operator()(SkillsUi* ui) const; };
    std::unique_ptr<SkillsUi, SkillsUiFree> skills_ui_;
    SkillsUi& skills_ui();
    /// The sequence's presses when the run began, as simple_base_presses_.
    std::uint64_t simple_base_seq_presses_ = 0;

    // ---- Kiểu đánh, Giữ khoảng cách, the jump key ----
    /// The attack style (gobot/attack_style.hpp), keeping distance, and the
    /// jump key with its "Thử" (gobot/jump_check.hpp) on the Đánh quái tab.
    /// "Thử" presses one key in the game, on the player's click only, off
    /// the drawing thread. Lives in simple_style.cpp.
    void simple_jump_box();             ///< "Phím nhảy [k] [Thử]", beside "Phím đánh"
    void simple_style_line();           ///< "Kiểu đánh [▼] (?) [Tầm đánh…]"
    void simple_keep_distance_tick();   ///< "Giữ khoảng cách", beside "Đứng yên một chỗ"
    void simple_style_hints();          ///< their lines under the row
    void simple_jump_checklist();       ///< the jump key's line in "Trước khi bắt đầu"
    void simple_style_popups();         ///< the "Tầm đánh" window, when asked for
    void simple_jump_try();             ///< "Thử": the one press, on a thread of its own
    /// Whether the keeper is getting into the game -- the one-shot "vào game
    /// ngay", a cold start, a crossing under way: it takes no hold while it
    /// clicks through the login pages, so "Thử" asks this as well.
    bool keeper_entering();
    /// From tick(): a "Thử" that has finished -- what it saw, and the
    /// detector stopped again if it was started for it.
    void simple_style_tick();
    /// F12, DỪNG, a failed revive and closing GoBot (stop_jobs): a "Thử" under
    /// way presses nothing more.
    void simple_style_stop();
    /// Declared after everything a "Thử" uses, so it goes before them: one
    /// under way is waited for while the detector and the keyboard are still
    /// there.
    struct StyleUi;
    struct StyleUiFree { void operator()(StyleUi* ui) const; };
    std::unique_ptr<StyleUi, StyleUiFree> style_ui_;
    StyleUi& style_ui();

    // ---- Chỗ đánh quái ----
    /// The hunting spot (HunterConfig::spot) on the Đánh quái tab: its tick
    /// and "Đặt chỗ này" on the "Tự đánh quái" row, its line under the row,
    /// Npcap's line on the checklist, the card's line under "Vị trí:", why
    /// fighting with it ticked does not start, and its part of the
    /// "Positions" line. The spot is the hunter's own, taken where the
    /// character stands and kept for the run only; the area's numbers are in
    /// the "Tầm đánh" window (simple_style.cpp). Lives in simple_spot.cpp.
    void simple_spot_tick();            ///< "Chỗ đánh quái [Đặt chỗ này]", after "Vị trí từ gói tin"
    void simple_spot_hint();            ///< its line under the row, while it is ticked
    void simple_spot_checklist();       ///< Npcap's line in "Trước khi bắt đầu", while it is ticked
    /// The card's line under "Vị trí:", from the hunter's status.
    void simple_spot_state(const Hunter::Status& hs);
    /// Why fighting with the spot ticked cannot start, or null when it can:
    /// the spot goes by where the game's packets put the character, so Npcap
    /// must be there and packet_positions not Off. In the log's English, and
    /// in `vi` the simple screen's words. Asks Npcap's service unless a
    /// capture runs: for a start, not for every frame.
    const char* spot_refusal(std::string& vi);
    /// What the last start_hunt was refused for by spot_refusal, for the card
    /// under BẮT ĐẦU; "" once a hunt starts, or a start is not refused.
    std::string   spot_refused_vi_;
    /// The checklist's own look at Npcap's service, and when it was taken.
    bool          spot_npcap_ok_ = true;
    std::uint64_t spot_npcap_ms_ = 0;
    /// The "Positions" line's part about the spot: where it is, how far off
    /// the character is, the walks back and those given up on.
    std::string spot_positions_text();

    // ---- status ----
    std::string status_message_;
    std::uint64_t status_until_ms_ = 0;
    void set_status(const std::string& msg, std::uint32_t ms = 4000);
};

} // namespace gobot
