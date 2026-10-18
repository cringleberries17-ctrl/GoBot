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
#include "gobot/game_state.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/macro.hpp"
#include "gobot/map_file.hpp"
#include "gobot/map_mapper.hpp"
#include "gobot/minimap_tracker.hpp"
#include "gobot/net_discovery.hpp"
#include "gobot/opcode_registry.hpp"
#include "gobot/packet_capture.hpp"
#include "gobot/pathfinder.hpp"
#include "gobot/protocol_analyzer.hpp"
#include "gobot/protocol_handler.hpp"
#include "gobot/routine.hpp"
#include "gobot/crash_watch.hpp"
#include "gobot/party.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/player_list.hpp"
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
    void stop_party();
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
    void stop_keeper();
    void keeper_loop();
    void draw_keeper_panel();

    std::thread       keeper_thread_;
    std::atomic<bool> keeper_running_{false};
    std::atomic<bool> keeper_stop_{false};
    std::atomic<int>  keeper_restarts_{0};

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
    void stop_revive();
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
    /// True when it ended a hold the bin may still need, and said so.
    bool stop_trash();
    void trash_loop();
    void say_trash(const std::string& line);

    std::thread              trash_thread_;
    std::atomic<bool>        trash_running_{false};
    std::atomic<bool>        trash_stop_{false};
    std::atomic<bool>        trash_oneshot_{false};   ///< here for one asked-for pass
    std::atomic<int>         trash_request_{0};       ///< 1 clean now, 2 look only
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
    /// the executor press anything meanwhile: its buff and potion threads run
    /// from launch, and nothing but that hold kept them off.
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
    /// running to look, bin_looks_clear every couple of seconds, so the
    /// executor's buffs and potions come back by themselves once the player
    /// has answered the bin's dialog. What the stop stopped waits for BẮT ĐẦU.
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
    /// End the hold, if it is still `reason`'s.
    void end_hold(const std::string& reason);
    std::mutex               hold_mutex_;

    // ---- saying things in chat ----
    /// Says the lines in configs/chat.ini in the game's chat, one at a time on
    /// a timer, with every other part of the bot held off while a line is
    /// typed -- and for as long after as the chat line may be open, until a
    /// look shows it shut. One thread for the timed lines and for the chat
    /// tab's two buttons; started for a button alone, it does that and ends.
    void start_chat();                    ///< the timed lines, for a run
    void request_chat(bool send_one);     ///< look at the chat bar, or say one line now
    /// Stops the worker. A line half typed is rubbed out and its line closed
    /// first, which takes a second or three: with `wait` false this returns
    /// at once and the worker finishes that on its own, joined by tick() or
    /// the next start, so the window does not freeze meanwhile.
    ///
    /// `quit` is the player taking over -- DỪNG, F12, closing GoBot: a chat
    /// line still in doubt then is handed over (chat_handover_) rather than
    /// waited on. Without it -- Tự chat unticked -- the worker stays until it
    /// sees the line shut, holding everything off until then.
    void stop_chat(bool quit = true, bool wait = true);
    void chat_loop();
    void say_chat(const std::string& line);
    /// A line Tự chat has just said, for the party worker: read back off the
    /// chat it is our own words, not somebody asking for an invitation.
    void remember_said(const std::string& typed);
    /// The lines said in the last half hour.
    std::vector<std::string> recently_said();
    /// When the last line was said, a Gửi thử's among them, on now_ms()'s
    /// clock; 0 for none yet.
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
    /// every couple of seconds, on a thread of its own, so the executor's
    /// buffs and potions come back by themselves once the player has shut the
    /// line. What the stop stopped waits for BẮT ĐẦU.
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
    void draw_calibrate_panel();
    void apply_findings();

    FramingSpec framing_;
    bool        framing_verified_ = false;

    /// What the client sends is framed differently -- the opcode sits at offset
    /// 2 coming down and offset 6 going up. A capture given only `framing_`
    /// decodes the server's half and grinds the client's half into resync
    /// noise, a byte at a time.
    FramingSpec client_framing_;
    bool        has_client_framing_ = false;

    /// Hand the capture both directions' framing. Use instead of calling
    /// `capture_.set_framing()` directly, so the client half is never dropped.
    void push_framings();

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
        bool refocus = true;       ///< bring the game back when nobody is using the machine
    };
    SimpleOptions simple_opts_;
    void load_simple_options();
    void save_simple_option(const char* key, bool value);

    /// Bring the game back to the front during a run, when it has lost focus
    /// and nobody has touched the mouse or keyboard for half a minute. The
    /// keys only land while the game is in front, so a notification that
    /// steals focus at 2am otherwise stops the bot until morning. Waiting for
    /// the machine to go idle is what keeps it from fighting a person for
    /// their own desktop. Called from tick().
    void refocus_if_idle();
    std::uint64_t last_refocus_ms_ = 0;

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
    void stop_quest();                    ///< at once: nothing more is pressed
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
    /// and does the two new keys' jobs itself.
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
                           const AutoLogin::Settings& login, TemplateStore& store,
                           CrashWatch::Look& seen);
    /// The login key's crossing, on the keeper's thread. False when the
    /// keeper is to stop: started for the key alone and done, or a crossing
    /// that gave up.
    bool keeper_key_crossing(CrashWatch& watch, const CrashWatch::Settings& watch_settings,
                             const AutoLogin::Settings& login, TemplateStore& store);
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
    void simple_potion_stock(MacroKeyKind kind);    ///< under that potion row
    struct PotionsUi;
    struct PotionsUiFree { void operator()(PotionsUi* ui) const; };
    std::unique_ptr<PotionsUi, PotionsUiFree> potions_ui_;

    // ---- closing stray popups ----
    /// "Tự đóng thông báo": during a run, the few popups on a fixed list --
    /// the check-in reward's notice, Yêu Quái Bách Khoa, the ESC menu GoBot's
    /// own ESC brought up -- closed by their own controls, once they have
    /// stayed up 3 s and nothing else is up (gobot/popup_closer.hpp). Lives in
    /// simple_popups.cpp.
    void start_popups();            ///< for a run; nothing when it is unticked
    void stop_popups();
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
    /// false, and said why, when the invite is not to go. `from_screen`: the
    /// frame was read off the screen (ScreenCapture::from_screen).
    bool party_may_invite(const std::string& who, const Frame& frame, bool from_screen,
                          const TemplateStore& store, bool always_leader);
    /// Every few seconds of the party worker, the HUD read off its frame for
    /// the Tổ đội tab.
    void party_hud_tick(const Frame& frame, bool from_screen, const TemplateStore& store);
    void simple_party_leader_option();      ///< "Chỉ mời khi đang làm chủ tổ đội"
    void simple_party_hud_line();           ///< "Tổ đội: …" on the state card, and "Thử xem"
    struct PartyHudUi;
    struct PartyHudUiFree { void operator()(PartyHudUi* ui) const; };
    static PartyHudUi* make_party_hud_ui();
    /// Made with the window, before any worker that reads it starts.
    std::unique_ptr<PartyHudUi, PartyHudUiFree> party_hud_ui_{ make_party_hud_ui() };

    // ---- Tự chat's "Chỉ khi chưa có tổ đội" ----
    /// On Tự chat's thread, for the line whose turn it is: when that line is
    /// ticked and the party HUD, read on a fresh frame, does not show the
    /// player alone, it is passed over for the next -- or, with no other line
    /// to go instead, put off a minute -- and true comes back, with nothing
    /// held or pressed for it. Lives in simple_chat_alone.cpp.
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

    // ---- status ----
    std::string status_message_;
    std::uint64_t status_until_ms_ = 0;
    void set_status(const std::string& msg, std::uint32_t ms = 4000);
};

} // namespace gobot
