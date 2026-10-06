// gobot_cli — the bot runner.
//
// Wires the pipeline together:
//
//   Npcap -> TcpReassembler -> framing -> OpcodeRegistry -> ProtocolHandler
//                                                              |
//                                                          GameState
//                                                              |
//                                                           BotBrain
//                                                              |
//                                                       CommandExecutor
//                                                              |
//                                                        InputManager -> game
//
// Capture is read-only; the bot acts only through synthetic keyboard input
// into the focused game window.

#include "gobot/auto_login.hpp"
#include "gobot/crash_watch.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/player_list.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/bot_brain.hpp"
#include "gobot/command_executor.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/game_state.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/camera_tracker.hpp"
#include "gobot/field_finder.hpp"
#include "gobot/hunter.hpp"
#include "gobot/minimap_tracker.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/macro.hpp"
#include "gobot/map_file.hpp"
#include "gobot/map_locator.hpp"
#include "gobot/map_table.hpp"
#include "gobot/opcode_registry.hpp"
#include "gobot/clr_bridge.hpp"
#include "gobot/dialog_driver.hpp"
#include "gobot/party.hpp"
#include "gobot/routine.hpp"
#include "gobot/session_manager.hpp"
#include "gobot/template_matcher.hpp"
#include "gobot/vitals_reader.hpp"
#include "gobot/world_anchor.hpp"
#include "gobot/packet_capture.hpp"
#include "gobot/pathfinder.hpp"
#include "gobot/protocol_handler.hpp"
#include "gobot/window_focus.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

using namespace gobot;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_bot_enabled{false};

/// How much of a calibration run is spent walking rather than fighting.
int g_walk_share_pct = 55;

// How far the character ranged during a calibration run, in level pixels.
std::atomic<double> g_walk_lo{1e18};
std::atomic<double> g_walk_hi{-1e18};
std::atomic<double> g_walk_span{0.0};

void on_signal(int) { g_stop.store(true); }

/// Whether this process is elevated. Decides more than it looks: Windows will
/// not let a process inject input while a window of higher integrity has the
/// foreground, so a bot started from a normal shell cannot drive a game
/// started from an elevated one.
bool running_as_admin() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = sizeof(e);
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size))
            elevated = e.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

void print_usage() {
    std::printf(R"(gobot_cli — GhostOnline packet-capture bot

USAGE
  gobot_cli [options]

OPTIONS
  --app <path>        App config      (default: configs/app.ini)
  --bot <path>        Bot config      (default: configs/bot_default.ini)
  --protocol <path>   Protocol map    (default: configs/protocol.ini)
  --iface <name>      Capture interface (overrides app config)
  --host <ip>         Game server IP
  --port <n>          Game server port
  --window <spec>     Which window to aim at. A plain string is a piece of
                      its title. For a window with no title -- the game's
                      launcher is one -- name it by class and program instead:
                        --window "class=#32770,proc=GhostOnlineVTC.exe"
                      See --windows for what is on screen.
  --status            Print state every second instead of running the bot.
                      Use this to confirm capture and decoding work before
                      letting it press any keys.
  --see               Look at the game window once and print every name plate
                      found, then exit. No packets, no keys pressed — this is
                      how to check what the radar can actually see, from a
                      console where nothing is covering the game.
  --read [x,y,w,h]    Read the game's text. With a rectangle, reads that patch;
                      with none, reads the map's name off the minimap title bar
                      and the name on every plate found. This is not OCR
                      guesswork: the game draws text with a bitmap font, so the
                      glyphs are matched against the font's own pictures and
                      the answer is exact.
  --find <text>       Say where that text is on screen, searching the whole
                      frame. This is how the bot finds your own character:
                      it has no health bar to be spotted by — its health is
                      in the HUD — but it has its name under its feet.
  --vitals <n>        Watch HP and MP for n seconds and print both readings:
                      the figures the game prints across its bars, and what
                      measuring the coloured part says. They disagree, and the
                      printed figures are the ones to believe — the caption is
                      drawn over the end of the fill, so the colour runs out
                      early and a nearly-full bar measures short. Ends with how
                      many looks could read the figures; well under all of them
                      means the bar geometry is wrong.
  --whereami          Name the map the character is standing in and load its
                      collision data.
  --windows           List every window the bot can see, with its class, its
                      program and where it is. Untitled ones are in the list:
                      the game's launcher -- the window with the purple start
                      button -- has no title at all, and a window you cannot
                      name is one you have to find by class and program.
  --templates         List the game's UI pictures this build carries.
  --template <name>   Say whether that picture is on screen, and where.
  --tplsave <name>    Write that picture out as a PNG so you can see what it
                      actually is; "*" for all of them. These come from
                      somebody else's assembly under a name they chose, and
                      searching the screen cannot identify one that is not
                      currently on it -- which, for a dialog, is most of the time.
  --survey            Try every picture against the screen at once. Open the
                      interface first: this is how to tell a set that belongs
                      to this build of the game from one that does not, which
                      is otherwise indistinguishable from "the dialog is shut".
  --grab name=x,y,w,h Save a rectangle of the live window as a new picture,
                      for anything the game's own set does not cover.
  --click x,y         Click that point of the window, in client coordinates.
                      For measuring: working out where a screen's controls
                      are means pressing one and seeing what changes.
  --calibrate <n>     Walk the character for n seconds while capturing, then
                      report which packet fields track the movement. This is
                      how the position field is found: no guessing, the screen
                      measures the truth and the correlation decides.
  --anchor <n>        Watch for n seconds and solve for where the camera is,
                      by matching creatures on screen against the ones the
                      server reports.

UNATTENDED RUNNING
  --session <n>       Watch the session for n seconds the way an overnight run
                      would, and report every drop. What it watches is the
                      HUD: a character in the world has a health bar and a mana
                      bar drawn as a matched pair, and nothing else the client
                      shows looks like that -- not the launcher, not the
                      channel list, not a disconnect dialog, not a loading
                      screen. A dialog nobody has ever seen still reads
                      correctly as "not playing".
  --autoparty <n>     For n seconds, invite whoever types the keyword in
                      chat. Reads the chat, waits for the same line to be
                      seen a few times over, then opens the player list and
                      presses Req Party on that name. --verbose prints every
                      chat line as it is read.
  --invite <name>     Invite that player: open the list of players on the
                      map, find their row, pick it, press Req Party. Add
                      --dry to see where it would click without clicking.
  --invites <n>       Watch for party invitations for n seconds and answer
                      them. The client gives about ten seconds before the
                      modal disappears, so an unattended run accepts none of
                      them otherwise. The inviter's name is read off the
                      dialog and checked against configs/party.ini before
                      anything is clicked.
  --doctor            Check everything GoBot needs on this machine and say
                      what is wrong: elevation, pictures, glyphs, configs,
                      the game window, screen capture, the launcher path.
                      Run this first when something does not work.
  --crashcheck        Say whether the client has fallen over, and how: exited,
                      stopped answering, or showing a message box. When it is a
                      box, the box's text is printed and the error code is read
                      out of it -- read, not matched against a picture, so a
                      code nobody has seen before still comes out right.
  --keepalive <n>     Keep the client alive for n minutes. Watches for the
                      above, and when it happens ends the client -- the
                      launcher behind it too -- and crosses back in. Gives up
                      after a few restarts: a client that falls over that often
                      is not failing at random, and a program that keeps
                      reconnecting to a server refusing it is indistinguishable
                      from an attack on that server.
  --setlogin          Write the game account down for the auto-login. The
                      password is typed with the echo off, never appears on a
                      command line, and is stored encrypted with your Windows
                      account's own key.
  --login             Cross from a cold machine to a character in the world:
                      launcher, login page, game list, start, connect, channel,
                      character. Says what it still needs if any page has yet
                      to be identified. It does not answer captchas and never
                      will -- if one appears it stops and says so.
  --upto <page>       With --login: cross as far as that page and stop with it
                      on screen. For measuring. The channel list and the
                      character select are each up for about a second in the
                      middle of a crossing, which is not long enough to
                      photograph one and work out where its entries are.
  --routines          List the click sequences in configs/routines.ini: buying,
                      mining, reviving, closing whatever is in the way. They
                      live in a config file rather than in code so they can be
                      corrected with the game open in front of you, which is
                      what will be wanted when the publisher patches the client.
  --routine <name>    Run one.
  --close             Close whatever dialogs are in the way, for real. The one
                      thing here that needs no calibration -- the middle of a
                      close button is the close button -- so it is the right
                      first thing to try against a live client.
  --dry               With --routine: find each step's picture and say where it
                      would click, without clicking anything. Use this first.
                      The offsets in that file were worked out from the
                      pictures' names rather than by watching the game, so a
                      first pass over a routine is expected to be wrong
                      somewhere -- and finding out by watching it misclick its
                      way through a shop is the expensive way.
  --verbose           Debug logging.
  --help              This text.

HOTKEYS (while running)
  F10                 Toggle the bot on/off
  Ctrl+C              Quit

BEFORE THIS WORKS
  configs/protocol.ini must contain a [Framing] block and at least the
  packet layouts the bot reads: self position, self HP, and monster
  positions. Derive them with gobot_sniff — see README.md.

  Run as Administrator; Npcap requires it.
)");
}

struct Options {
    std::string app_path      = "configs/app.ini";
    std::string bot_path;
    std::string protocol_path;
    std::string iface;
    std::string host;
    std::string window;
    std::uint16_t port = 0;
    bool status_only = false;
    bool see_only = false;
    bool read_text = false;
    std::string read_rect;
    bool readself = false;
    std::string readself_text;
    std::string find_text;
    int  calibrate_seconds = 0;
    bool keytest = false;
    bool inputcheck = false;
    int  hunt_seconds = 0;
    std::string press_key;
    std::string click_spec;
    std::string analyse_path;
    std::string dump_opcode_arg;
    std::string check_arg;
    int anchor_seconds = 0;
    bool fields = false;
    bool whereami = false;
    bool list_windows = false;
    bool templates = false;
    bool survey = false;
    bool template_save = false;
    std::string template_source;
    int  vitals_seconds = 0;
    int  session_seconds = 0;
    bool login = false;
    std::string login_upto;
    int  keepalive_minutes = 0;
    bool crash_check = false;
    bool doctor = false;
    int  invite_seconds = 0;
    std::string invite_who;
    int  auto_party_seconds = 0;
    bool set_login = false;
    bool routines = false;
    bool dry_run = false;
    bool close_popups = false;
    bool party = false;
    bool player_list = false;
    int  pick_row = 0;
    std::string routine_name;
    std::string template_name;
    std::string grab_spec;
    bool bridge = false;
    std::string bridge_method = "Probe";
    std::string bridge_arg;
    int walk_pct = 55;
    bool verbose = false;
};

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "--app")            o.app_path = next("--app");
        else if (a == "--bot")       o.bot_path = next("--bot");
        else if (a == "--protocol")  o.protocol_path = next("--protocol");
        else if (a == "--iface")     o.iface = next("--iface");
        else if (a == "--host")      o.host = next("--host");
        else if (a == "--port")      o.port = static_cast<std::uint16_t>(std::stoi(next("--port")));
        else if (a == "--window")    o.window = next("--window");
        else if (a == "--status")    o.status_only = true;
        else if (a == "--see")       o.see_only = true;
        else if (a == "--read") {
            o.read_text = true;
            // The rectangle is optional, so take the next argument only when
            // it is one rather than another switch.
            if (i + 1 < argc && argv[i + 1][0] != '-') o.read_rect = argv[++i];
        }
        else if (a == "--find")     o.find_text = next("--find");
        else if (a == "--readself") {
            o.readself = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.readself_text = argv[++i];
        }
        else if (a == "--calibrate") o.calibrate_seconds = std::stoi(next("--calibrate"));
        else if (a == "--keytest")   o.keytest = true;
        else if (a == "--inputcheck") o.inputcheck = true;
        else if (a == "--hunt")      o.hunt_seconds = std::stoi(next("--hunt"));
        else if (a == "--press")     o.press_key = next("--press");
        else if (a == "--analyse")   o.analyse_path = next("--analyse");
        else if (a == "--dump")      o.dump_opcode_arg = next("--dump");
        else if (a == "--check")     o.check_arg = next("--check");
        else if (a == "--anchor")    o.anchor_seconds = std::stoi(next("--anchor"));
        else if (a == "--fields")    o.fields = true;
        else if (a == "--whereami") o.whereami = true;
        else if (a == "--windows")  o.list_windows = true;
        else if (a == "--upto")     o.login_upto = next("--upto");
        else if (a == "--click")    o.click_spec = next("--click");
        else if (a == "--keepalive") o.keepalive_minutes = std::stoi(next("--keepalive"));
        else if (a == "--crashcheck") o.crash_check = true;
        else if (a == "--doctor")   o.doctor = true;
        else if (a == "--invites")  o.invite_seconds = std::stoi(next("--invites"));
        else if (a == "--invite")   o.invite_who = next("--invite");
        else if (a == "--autoparty") o.auto_party_seconds = std::stoi(next("--autoparty"));
        else if (a == "--templates") o.templates = true;
        else if (a == "--grab")      o.grab_spec = next("--grab");
        else if (a == "--template")  { o.templates = true; o.template_name = next("--template"); }
        else if (a == "--survey")    { o.templates = true; o.survey = true; }
        else if (a == "--tplsave")   { o.templates = true; o.template_save = true;
                                       o.template_name = next("--tplsave"); }
        else if (a == "--tplsrc")    o.template_source = next("--tplsrc");
        else if (a == "--vitals")    o.vitals_seconds = std::stoi(next("--vitals"));
        else if (a == "--session")   o.session_seconds = std::stoi(next("--session"));
        else if (a == "--routines")  o.routines = true;
        else if (a == "--routine")   { o.routines = true; o.routine_name = next("--routine"); }
        else if (a == "--dry")       o.dry_run = true;
        else if (a == "--close")     o.close_popups = true;
        else if (a == "--party")     o.party = true;
        else if (a == "--playerlist") o.player_list = true;
        else if (a == "--pickrow")   { o.player_list = true; o.pick_row = std::stoi(next("--pickrow")); }
        else if (a == "--bridge") {
            o.bridge = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.bridge_method = next("--bridge");
            if (i + 1 < argc && argv[i + 1][0] != '-') o.bridge_arg = next("--bridge");
        }
        else if (a == "--login")     o.login = true;
        else if (a == "--setlogin")  o.set_login = true;
        else if (a == "--walk")      o.walk_pct = std::stoi(next("--walk"));
        else if (a == "--verbose")   o.verbose = true;
        else if (a == "--help" || a == "-h") { print_usage(); std::exit(0); }
        else {
            std::fprintf(stderr, "error: unknown argument '%s'\n\n", a.c_str());
            return false;
        }
    }
    return true;
}

std::string resolve(const std::string& path) {
    // Bare or configs/-relative paths resolve against the exe, so the tool
    // works from any working directory.
    if (path.size() > 1 && (path[1] == ':' || path[0] == '\\' || path[0] == '/')) return path;
    return exe_relative(path);
}

FramingSpec load_framing(const std::string& path, bool& found,
                         const char* section = "Framing") {
    FramingSpec spec;
    IniFile ini;
    found = false;
    if (!ini.load(path) || !ini.has_section(section)) return spec;

    found = true;
    const std::string Framing = section;     // the reads below name it
    spec.header_size   = static_cast<std::uint32_t>(ini.get_int(Framing, "header_size", 4));
    spec.length_offset = static_cast<std::uint32_t>(ini.get_int(Framing, "length_offset", 0));
    spec.length_size   = static_cast<std::uint32_t>(ini.get_int(Framing, "length_size", 2));
    spec.length_le     = ini.get(Framing, "length_endian", "little") != "big";
    spec.length_includes_header = ini.get_bool(Framing, "length_includes_header", true);
    spec.length_adjust = ini.get_int(Framing, "length_adjust", 0);
    spec.opcode_offset = static_cast<std::uint32_t>(ini.get_int(Framing, "opcode_offset", 2));
    spec.opcode_size   = static_cast<std::uint32_t>(ini.get_int(Framing, "opcode_size", 2));
    spec.opcode_le     = ini.get(Framing, "opcode_endian", "little") != "big";
    spec.min_frame     = static_cast<std::uint32_t>(ini.get_int(Framing, "min_frame", static_cast<int>(spec.header_size)));
    spec.max_frame     = static_cast<std::uint32_t>(ini.get_int(Framing, "max_frame", 65535));
    spec.checksum_offset = static_cast<std::uint32_t>(ini.get_int(Framing, "checksum_offset", static_cast<int>(FramingSpec::kNoChecksum)));
    spec.trailer_opcode = static_cast<std::uint16_t>(ini.get_int(Framing, "trailer_opcode", 0));
    spec.trailer_bytes  = static_cast<std::uint32_t>(ini.get_int(Framing, "trailer_bytes", 0));
    return spec;
}

/// Global hotkey thread. Uses GetAsyncKeyState rather than RegisterHotKey so
/// the key still reaches the game — a registered hotkey is swallowed.
void hotkey_loop(std::uint32_t toggle_vk, BotBrain& brain, CommandExecutor& exec) {
    bool was_down = false;

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));

        const bool down = (GetAsyncKeyState(static_cast<int>(toggle_vk)) & 0x8000) != 0;
        if (down && !was_down) {
            const bool enable = !g_bot_enabled.load();
            g_bot_enabled.store(enable);

            if (enable) {
                brain.reanchor_origin();
                brain.start();
                exec.set_auto_attack(true);
                GB_INFO("=== BOT ENABLED ===");
            } else {
                brain.stop();
                exec.set_auto_attack(false);
                exec.clear_queue();
                GB_INFO("=== BOT DISABLED ===");
            }
        }
        was_down = down;
    }
}

/// Attach to the window these options point at.
///
/// A title given on the command line stands on its own. The class and process
/// from the app config identify the *game*, so pairing them with a title the
/// user chose means the two conditions can only ever both hold for the game:
/// asking for the publisher's launcher by name matched nothing and reported
/// "no window matching 'VTCZone'" while VTCZone sat there on the screen.
///
/// Naming a window is how you say "not the game, that one", so the only
/// sensible reading is to drop the conditions that say "the game".
/// `--window` may also name a class and a program, not just a title -- see
/// `InputManager::parse_spec`. The game's launcher has no title, so it is the
/// only way to aim at it.
bool attach_window(InputManager& input, const Options& o, const AppConfig& app) {
    if (!o.window.empty())
        return input.find_game_window(InputManager::parse_spec(o.window));
    return input.find_game_window(app.window_title, app.window_class, app.process_name);
}

/// One look at the game window, printed.
///
/// A console has no window of its own in the way, which is the point: the game
/// will not render itself into a bitmap on request, so frames are lifted off
/// the screen and anything covering the game is read instead. When the radar
/// in the GUI sees nothing, this says whether the problem is the detector or
/// the GUI window sitting on top of the game.
int run_see(const Options& o, const AppConfig& app) {
    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'. Is the game running?\n", title.c_str());
        return 1;
    }

    int w = 0, h = 0;
    input.window_size(w, h);
    std::printf("Window '%s'  client %dx%d  focused: %s\n\n",
                title.c_str(), w, h, input.window_focused() ? "yes" : "no");

    ScreenCapture capture;
    Frame frame;
    if (!capture.grab(input.target_window(), frame) || !frame.valid()) {
        std::fprintf(stderr, "Could not capture the window.\n");
        return 1;
    }
    std::printf("Captured %dx%d%s\n\n", frame.width, frame.height,
                capture.from_screen() ? "  (off the screen -- keep the game unobscured)" : "");

    GlyphFont font;
    const std::string font_dir = GlyphFont::default_dir();
    const bool have_font = !font_dir.empty() && font.load(font_dir);

    // Vitals, read with whatever the macro config says the bars are.
    {
        MacroConfig mc;
        mc.load(resolve("configs/macro.ini"));

        // Both answers, side by side, because they disagree in a way worth
        // seeing: the game prints its figures over the end of the bar, so
        // measuring the colour reads a nearly-full bar as two-thirds full.
        // Showing only the better one hides why the other was ever wrong.
        const auto show = [&](const char* what, const BarSpec& b) {
            std::printf("%s bar at %d,%d %dx%d  colour %u,%u,%u +/-%d\n",
                        what, b.x, b.y, b.width, b.height, b.r, b.g, b.b, b.tolerance);

            const float pct = VitalsReader::measure(frame, b);
            std::printf("    bar measures   ");
            if (pct < 0.0f) std::printf("unreadable\n");
            else            std::printf("%.0f%%\n", pct);

            std::printf("    printed says   ");
            int cur = 0, max = 0;
            if (!have_font)
                std::printf("no glyph set installed\n");
            else if (VitalsReader::read_numbers(frame, b, font, cur, max))
                std::printf("%d / %d  (%.0f%%)\n", cur, max,
                            100.0 * cur / std::max(1, max));
            else {
                const ScreenRect box = VitalsReader::caption_box(b);
                std::printf("could not read the figures at %d,%d %dx%d\n",
                            box.x, box.y, box.w, box.h);
            }
        };
        show("HP", mc.hp_bar);
        show("MP", mc.mp_bar);

        // And what they would be if found from scratch. The game's HUD can be
        // resized from its own settings, which moves the bars and leaves the
        // saved geometry reading the wrong fraction of the wrong bar.
        BarSpec fhp = mc.hp_bar, fmp = mc.mp_bar;
        const int n = VitalsReader::find_bars(frame, fhp, fmp);
        std::printf("\nfound %d bar(s) by looking: ", n);
        if (n == 0) {
            std::printf("none\n");
        } else {
            std::printf("\n");
            show("HP", fhp);
            show("MP", fmp);
        }
        std::printf("\n");
    }

    std::printf(have_font ? "Reading plates with the game's own font (%zu glyphs).\n\n"
                          : "No glyph set installed -- plates are being guessed at "
                            "from pixel statistics.%.0s\n\n",
                font.size());

    const DetectorTuning tuning = DetectorTuning::defaults();
    MobDetector::Cost cost;
    const auto t_detect = std::chrono::steady_clock::now();
    const std::vector<Nameplate> plates =
        MobDetector::detect(frame, tuning, have_font ? &font : nullptr, &cost);
    const double ms_with = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_detect).count();
    std::printf("%.0f ms: %d runs of red looked at, %d of them read, %zu kept\n\n",
                ms_with, cost.bars, cost.read, plates.size());

    if (plates.empty()) {
        std::printf("Nothing found.\n\n"
                    "Either there is nothing on screen with a name plate, or the "
                    "game is covered by another window.\n");
        return 0;
    }

    std::printf("%-8s %-6s %-6s %-9s %-7s %s\n",
                "what", "x", "y", "bar", "health", "name");
    for (const Nameplate& n : plates) {
        std::printf("%-8s %-6d %-6d %dx%-6d %5.0f%%   \"%s\"\n",
                    n.is_player ? "player" : "monster",
                    n.x(), n.y(), n.bar.w, n.bar.h, n.hp * 100.0f,
                    n.name.c_str());
    }

    Nameplate me;
    std::printf("\n");
    bool have_me = MobDetector::pick_player(plates, frame.width, me);
    if (have_me) {
        std::printf("Your character is at %d,%d.\n", me.x(), me.y());
    } else {
        // The bar test cannot find the local character -- it wears no bars --
        // so look for its name instead.
        HunterConfig hc;
        hc.load(resolve("configs/hunt.ini"));
        ScreenRect at{};
        float sure = 0.0f;
        if (hc.character_name.empty()) {
            std::printf("No character_name set in configs/hunt.ini, so there is "
                        "nothing to look for.\n"
                        "Your character wears no health bar -- your health is in "
                        "the HUD -- so its name is the only way to find it.\n");
        } else if (!have_font) {
            std::printf("No glyph set installed, so \"%s\" cannot be looked for.\n",
                        hc.character_name.c_str());
        } else if (font.find_text(frame, ScreenRect{ 0, 0, frame.width, frame.height },
                                  hc.character_name, at, &sure)) {
            have_me = true;
            std::printf("Your character \"%s\" is at %d,%d  (found by name, %.0f%% "
                        "sure)\n", hc.character_name.c_str(), at.cx(), at.y,
                        sure * 100.0f);
        } else {
            std::printf("\"%s\" is not on screen. Check the spelling against what "
                        "the game writes under your feet.\n",
                        hc.character_name.c_str());
        }
    }

    // Which map this is.
    MinimapTracker tracker;
    tracker.set_layout(MinimapTracker::default_layout(frame.width, frame.height));
    for (int i = 0; i < 6; ++i) tracker.update(frame);   // settle the fingerprint

    MapBook book;
    book.load(resolve("configs/maps.ini"));
    const std::uint64_t fp = tracker.fingerprint();
    const std::string name = book.name_for(fp);
    std::printf("\nMap %016llx%s%s\n",
                static_cast<unsigned long long>(fp),
                name.empty() ? "" : "  -- ", name.c_str());
    if (name.empty())
        std::printf("  Not named yet. Name it on the Radar tab and it will be "
                    "recognised from now on.\n");

    // Follow the view for a few seconds and report where the character ends up
    // in level coordinates. Walk about while this runs and the numbers should
    // follow; stand still and they should not move.
    {
        CameraTracker cam;
        cam.set_play_area(ScreenRect{ 100, 130, 620, 380 });
        ScreenCapture cap2;
        Frame fr;
        std::printf("\nFollowing the view for 4 seconds...\n");
        for (int i = 0; i < 28; ++i) {
            if (cap2.grab(input.target_window(), fr)) cam.update(fr);
            std::this_thread::sleep_for(std::chrono::milliseconds(140));
        }
        const auto fx = cam.fix();
        std::printf("  view moved %.0f,%.0f   %llu matched, %llu lost   match %.0f%%\n",
                    fx.x, fx.y,
                    static_cast<unsigned long long>(fx.updates),
                    static_cast<unsigned long long>(fx.lost),
                    fx.confidence * 100.0f);

        Nameplate now;
        if (cap2.grab(input.target_window(), fr)) {
            const std::vector<Nameplate> again = MobDetector::detect(fr, tuning);
            if (MobDetector::pick_player(again, fr.width, now)) {
                double wx = 0.0, wy = 0.0;
                cam.to_world(now.x(), now.y(), wx, wy);
                std::printf("  you are at %.0f,%.0f in the level\n", wx, wy);
            }
        }
    }

    if (have_me) {
        std::printf("\nRelative to you:\n");
        for (const Nameplate& n : plates) {
            if (n.is_player) continue;
            const int dx = n.x() - me.x();
            const int dy = n.y() - me.y();
            std::printf("  %4d px %-5s  %4d px %-4s\n",
                        std::abs(dx), dx < 0 ? "left" : "right",
                        std::abs(dy), dy < 0 ? "up" : "down");
        }
    }
    return 0;
}

/// Report what a capture contains and which fields track what we measured.
/// Shared between a live run and a re-analysis of a saved one.
void report_findings(const FieldFinder& finder) {
    std::printf("what the capture contained:\n");
    std::printf("  %-8s %-5s %8s %8s %10s %s\n",
                "opcode", "dir", "packets", "bytes", "creatures", "named by");
    for (const FieldFinder::Summary& s : finder.summarise()) {
        std::printf("  0x%04X   %-5s %8zu %8zu %10zu %s\n",
                    s.opcode, s.direction == Direction::ToClient ? "S->C" : "C->S",
                    s.count, s.payload_bytes, s.entities, s.id_field.c_str());
    }

    // Pairs first. One field correlating is a coincidence waiting to happen
    // across this many hypotheses; an x and a y from the same creature, side
    // by side in the packet, is a position.
    const auto pairs = finder.position_pairs(20, 0.75);
    std::printf("\n=== position candidates (an x and a y together) ===\n");
    if (pairs.empty()) {
        std::printf("none.\n");
    } else {
        int shown = 0;
        for (const FieldFinder::Pair& p : pairs) {
            if (++shown > 10) break;
            std::printf("opcode 0x%04X  creature %s = 0x%08X\n",
                        p.x.opcode, p.x.id_reading.describe().c_str(), p.x.id_value);
            std::printf("   x at %-11s corr %6.3f  %8.2f units/px  %zu samples\n",
                        p.x.reading.describe().c_str(), p.x.correlation,
                        p.x.scale, p.x.samples);
            std::printf("   y at %-11s corr %6.3f  %8.2f units/px  %zu samples\n\n",
                        p.y.reading.describe().c_str(), p.y.correlation,
                        p.y.scale, p.y.samples);
        }
    }

    // The busiest message from the client, read as little-endian words with
    // the measured position beside it. Statistics say which byte; looking at
    // the bytes says whether the answer is sensible.
    for (const FieldFinder::Summary& s : finder.summarise()) {
        if (s.direction != Direction::ToServer || s.count < 10) continue;
        const auto rows = finder.examples(s.opcode, false, 12);
        if (rows.empty()) break;

        std::printf("\n=== opcode 0x%04X from the client, %zu bytes ===\n",
                    s.opcode, s.payload_bytes);
        const std::size_t words = rows.front().payload.size() / 2;
        std::printf("%9s %9s |", "screen x", "screen y");
        for (std::size_t w = 0; w < words; ++w) std::printf("%7zu", w * 2);
        std::printf("   (byte offset)\n");

        for (const auto& r : rows) {
            if (r.position) std::printf("%9.0f %9.0f |", r.x, r.y);
            else            std::printf("%9s %9s |", "-", "-");
            for (std::size_t w = 0; w < words && (w * 2 + 1) < r.payload.size(); ++w) {
                const unsigned v = static_cast<unsigned>(r.payload[w * 2]) |
                                   (static_cast<unsigned>(r.payload[w * 2 + 1]) << 8);
                std::printf("%7u", v);
            }
            std::printf("\n");
        }
        break;
    }

    const auto found = finder.analyse(20, 0.70);
    std::printf("\n=== every field that tracked something ===\n");
    if (found.empty()) {
        std::printf("none above the threshold.\n");
        return;
    }
    std::printf("%-8s %-5s %-11s %-4s %7s %7s %10s %7s  %s\n",
                "opcode", "dir", "field", "is", "corr", "vs‑clk", "units/px",
                "samples", "creature");
    int shown = 0;
    for (const FieldFinder::Candidate& c : found) {
        if (++shown > 30) break;
        char who[40] = "all";
        if (c.grouped)
            std::snprintf(who, sizeof(who), "%s = 0x%08X",
                          c.id_reading.describe().c_str(), c.id_value);
        std::printf("0x%04X   %-5s %-11s %-4s %7.3f %7.3f %10.2f %7zu  %s\n",
                    c.opcode, c.direction == Direction::ToClient ? "S->C" : "C->S",
                    c.reading.describe().c_str(),
                    FieldFinder::to_string(c.quantity),
                    c.correlation, c.clock_correlation, c.scale, c.samples, who);
    }
    std::printf("\n'vs-clk' is how well the same field tracks the clock. Anything\n"
                "that follows the clock as closely as the measurement is a counter\n"
                "and has already been dropped; what is left should be well below.\n");
}

/// Print raw payloads of one opcode next to what was measured alongside them.
///
/// The correlation table says which byte. This says whether the answer is
/// sensible -- whether the number next to a position that moved by a thousand
/// pixels also moved by about a thousand, whether the bytes around it look
/// like a creature record repeated, whether the field is where a protocol
/// designer would have put it. Statistics cannot see any of that.
void dump_opcode(const FieldFinder& finder, std::uint16_t opcode, bool from_server) {
    const auto rows = finder.examples(opcode, from_server, 24);
    if (rows.empty()) {
        std::printf("\nNo samples of 0x%04X %s.\n", opcode,
                    from_server ? "from the server" : "from the client");
        return;
    }

    std::size_t width = 0;
    for (const auto& r : rows) width = std::max(width, r.payload.size());

    std::printf("\n=== 0x%04X %s, %zu bytes, %zu samples ===\n", opcode,
                from_server ? "S->C" : "C->S", width, rows.size());
    std::printf("%8s %8s  ", "meas x", "meas y");
    for (std::size_t i = 0; i < width; ++i) std::printf("%02zX ", i);
    std::putchar('\n');

    for (const auto& r : rows) {
        if (r.position) std::printf("%8.0f %8.0f  ", r.x, r.y);
        else            std::printf("%8s %8s  ", "-", "-");
        for (std::size_t i = 0; i < width; ++i) {
            if (i < r.payload.size()) std::printf("%02X ", r.payload[i]);
            else                      std::printf("   ");
        }
        std::putchar('\n');
    }

    // The same bytes read as little-endian 16-bit words, which is how this
    // protocol writes everything else in its header.
    std::printf("\nas u16le:\n%8s  ", "meas x");
    for (std::size_t i = 0; i + 1 < width; i += 2) std::printf("%6zu ", i);
    std::putchar('\n');
    for (const auto& r : rows) {
        if (r.position) std::printf("%8.0f  ", r.x); else std::printf("%8s  ", "-");
        for (std::size_t i = 0; i + 1 < r.payload.size(); i += 2)
            std::printf("%6u ",
                        static_cast<unsigned>(r.payload[i] | (r.payload[i + 1] << 8)));
        std::putchar('\n');
    }
}

/// Cut a picture out of the running game and keep it as a template.
///
/// This turned out to be the only source that works. Two other programs ship
/// sets of this game's UI extracted from their own clients -- a thousand
/// pictures between them, named and organised -- and not one of them matches
/// what is on this screen: they came from a different build, in a different
/// language, with a different theme. Even the item icons differ.
///
/// Pictures taken from the client that is actually running cannot have that
/// problem. There is no guessing about skins or versions, because it is the
/// same pixels the matcher will later be looking for.
int run_grab(const Options& o, const AppConfig& app, const std::string& spec) {
    char name[64] = {};
    int x = 0, y = 0, w = 0, h = 0;
    if (std::sscanf(spec.c_str(), "%63[^=]=%d,%d,%d,%d", name, &x, &y, &w, &h) != 5 ||
        w <= 0 || h <= 0) {
        std::fprintf(stderr, "Expected --grab name=x,y,w,h\n");
        return 2;
    }

    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'.\n", title.c_str());
        return 1;
    }
    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    ScreenCapture cap;
    Frame frame;
    if (!cap.grab(input.target_window(), frame) || !frame.valid()) {
        std::fprintf(stderr, "Could not capture the window.\n");
        return 1;
    }
    if (x < 0 || y < 0 || x + w > frame.width || y + h > frame.height) {
        std::fprintf(stderr, "%d,%d %dx%d is outside the %dx%d client.\n",
                     x, y, w, h, frame.width, frame.height);
        return 2;
    }

    Frame cut;
    cut.width = w;
    cut.height = h;
    cut.pixels.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            std::uint8_t r, g, b;
            frame.get(x + col, y + row, r, g, b);
            const std::size_t i = (static_cast<std::size_t>(row) * w + col) * 4;
            cut.pixels[i] = b; cut.pixels[i + 1] = g; cut.pixels[i + 2] = r;
            cut.pixels[i + 3] = 255;
        }
    }

    const std::string dir = TemplateStore::default_dir() + "\\grabbed";
    CreateDirectoryA(TemplateStore::default_dir().c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    const std::string path = dir + "\\" + name + ".png";
    if (!save_png(cut, path)) {
        std::fprintf(stderr, "Could not write %s\n", path.c_str());
        return 1;
    }

    std::printf("%s  (%dx%d from %d,%d)\n\n", path.c_str(), w, h, x, y);
    std::printf("Refer to it as  grabbed/%s  or give it a short name in\n"
                "configs/templates.ini:\n\n    %s = grabbed/%s\n\n"
                "Then check it with:  gobot_cli --template %s\n",
                name, name, name, name);
    return 0;
}

/// List the UI pictures available, or look for one on the screen now.
///
/// Both halves matter. The pictures come from someone else's install and are
/// known only by position, so the list is how a name gets attached to one; and
/// a template that has never been tried against a live screen is a guess, so
/// the search is how it gets checked before a feature is built on it.
/// Try every loaded picture against what is on screen right now.
///
/// This exists to answer a question that cost an afternoon: are these pictures
/// actually this client's? A set extracted from a different build of the game
/// matches nothing, and matches nothing *silently* -- every look-up simply
/// reports the dialog absent, which is indistinguishable from the dialog being
/// absent. One sweep over a screen with the interface open settles it: if the
/// set belongs to this client, the pictures that are on screen are found at
/// close to a perfect score, because they are the very bytes the game drew.
int run_survey(const TemplateStore& store, const Frame& frame) {
    struct Hit { std::string name; int x, y, w, h; float score; };
    std::vector<Hit> hits;

    const auto names = store.names();
    const auto started = std::chrono::steady_clock::now();
    std::size_t looked = 0, skipped = 0;

    for (const std::string& name : names) {
        const Frame* tpl = store.get(name);
        if (!tpl || !tpl->valid()) { ++skipped; continue; }
        // Too small to carry evidence: a few pixels correlate with anything.
        if (tpl->width < 8 || tpl->height < 8) { ++skipped; continue; }
        if (tpl->width > frame.width || tpl->height > frame.height) { ++skipped; continue; }

        ++looked;
        const auto m = TemplateMatcher::find(
            frame, ScreenRect{ 0, 0, frame.width, frame.height }, *tpl, 0.90f);
        if (m.found)
            hits.push_back(Hit{ name, m.at.x, m.at.y, m.at.w, m.at.h, m.score });

        if (looked % 100 == 0)
            std::fprintf(stderr, "  ... %zu of %zu tried, %zu found\n",
                         looked, names.size(), hits.size());
    }
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.score > b.score; });

    std::printf("tried %zu pictures against a %dx%d screen in %.0f s "
                "(%zu too small or too large to try)\n\n",
                looked, frame.width, frame.height, secs, skipped);

    if (hits.empty()) {
        std::printf("none of them is on this screen.\n\n"
                    "With the interface open, that is the answer to the question:\n"
                    "this set was extracted from a different build of the game, so\n"
                    "none of its artwork is what this client draws. Grab what is\n"
                    "needed from the live client instead:\n"
                    "  gobot_cli --grab name=x,y,w,h\n");
        return 1;
    }

    std::printf("%zu found:\n\n", hits.size());
    for (const Hit& h : hits)
        std::printf("  %7.3f  %4d,%-4d %3dx%-3d  %s\n",
                    h.score, h.x, h.y, h.w, h.h, h.name.c_str());
    return 0;
}

/// Every window the bot can see.
///
/// This exists because of a day lost to its absence. The game's launcher --
/// the small window with the purple start button -- has no title. Nothing
/// could find it, the login flow waited out a full minute for a window that
/// was on the screen in front of it, and from the outside that is
/// indistinguishable from "the button was pressed and nothing happened". A
/// window you cannot name is one you find by class and program, and this
/// prints both.
int run_list_windows() {
    const auto windows = InputManager::enumerate_windows();
    std::printf("%zu windows\n\n", windows.size());
    std::printf("  %-34s %-22s %-24s %s\n", "title", "class", "program", "at");
    std::printf("  %-34s %-22s %-24s %s\n",
                "----------------------------------",
                "----------------------",
                "------------------------", "---------------------");

    for (const auto& w : windows) {
        RECT r{};
        char where[64] = "?";
        if (GetWindowRect(static_cast<HWND>(w.hwnd), &r))
            std::snprintf(where, sizeof(where), "%d,%d %dx%d",
                          static_cast<int>(r.left), static_cast<int>(r.top),
                          static_cast<int>(r.right - r.left),
                          static_cast<int>(r.bottom - r.top));

        std::string title = w.title.empty() ? "(untitled)" : w.title;
        if (title.size() > 34) title = title.substr(0, 31) + "...";

        std::printf("  %-34s %-22s %-24s %s\n", title.c_str(),
                    w.window_class.c_str(), w.process.c_str(), where);
    }

    std::printf("\nTo aim at one of these, give --window a piece of its title,\n"
                "or -- for an untitled one -- put its class and program in the\n"
                "config that needs it. configs/login.ini does exactly that for\n"
                "the launcher: client_class and client_process.\n");
    return 0;
}

int run_templates(const Options& o, const AppConfig& app, const std::string& which) {
    TemplateStore store;

    // A named source, when asked for. The launcher's pages are not in the
    // game's own picture set -- they belong to the publisher's launcher, and
    // the only extracted copy of them is inside the other helper's assembly.
    if (!o.template_source.empty()) {
        const bool is_dll = o.template_source.size() > 4 &&
            _stricmp(o.template_source.c_str() + o.template_source.size() - 4, ".dll") == 0;
        const bool ok = is_dll
            ? store.load_from_assembly(o.template_source, resolve("configs/templates.ini"))
            : store.load(o.template_source);
        if (!ok) {
            std::fprintf(stderr, "No pictures in '%s'.\n", o.template_source.c_str());
            return 1;
        }
        if (!is_dll) store.apply_names(resolve("configs/templates.ini"));
        std::printf("from %s\n\n", o.template_source.c_str());
    } else if (!store.load_best(resolve("configs/templates.ini"))) {
        std::fprintf(stderr,
            "No UI pictures found. These are the game's own dialogs, buttons and\n"
            "item icons, looked for in this order:\n"
            "  templates\\ beside this executable\n"
            "  C:\\Program Files\\ZTeam\\GoC\\data\\images\n"
            "  C:\\Program Files\\TH_VGO_Bot_Helper\\TungHaVGOBot.dll\n"
            "Without them the features that recognise dialogs stay off.\n");
        return 1;
    }
    if (o.template_source.empty()) std::printf("from %s\n\n", store.source().c_str());

    if (which.empty() && !o.survey) {
        // The short names this project gave them, not the source's own paths
        // -- there are a thousand of those and they are not the point.
        std::printf("%zu pictures loaded. The ones this project has named:\n\n",
                    store.size());
        int named = 0;
        for (const std::string& name : store.names()) {
            if (name.find('/') != std::string::npos) continue;
            if (name.size() == 4 && name[0] == 't') continue;
            const Frame* f = store.get(name);
            std::printf("  %-22s %dx%d\n", name.c_str(), f->width, f->height);
            ++named;
        }
        std::printf("\n%d named of %zu. The rest keep the source's own names; "
                    "add one to configs/templates.ini to give it a short one.\n",
                    named, store.size());
        return 0;
    }

    const Frame* tpl = o.survey ? nullptr : store.get(which);
    if (!o.survey && !tpl) {
        std::fprintf(stderr, "Nothing called '%s'.\n", which.c_str());
        return 1;
    }

    // Write it out instead of hunting for it.
    //
    // These pictures come out of somebody else's assembly and are known only
    // by a name they chose. "party_panel_confirm" could be the dialog that
    // asks you to join a party, or the one that confirms you left it, and the
    // difference decides whether a bot clicking it joins or quits. The only
    // way to know is to look, and searching the screen for it cannot tell you
    // when the thing is not on screen -- which, for a dialog, is most of the
    // time.
    if (o.template_save) {
        const std::string dir = TemplateStore::default_dir() + "\\seen";
        CreateDirectoryA(TemplateStore::default_dir().c_str(), nullptr);
        CreateDirectoryA(dir.c_str(), nullptr);

        std::vector<std::string> wanted;
        if (which == "*") wanted = store.names();
        else              wanted.push_back(which);

        int written = 0;
        for (const std::string& name : wanted) {
            const Frame* f = store.get(name);
            if (f == nullptr || !f->valid()) continue;

            // Their names contain slashes; a file name cannot.
            std::string safe = name;
            for (char& c : safe)
                if (c == '/' || c == '\\' || c == ':') c = '_';

            const std::string path = dir + "\\" + safe + ".png";
            if (!save_png(*f, path)) continue;
            std::printf("  %-26s %4dx%-4d  %s\n", name.c_str(), f->width, f->height,
                        path.c_str());
            ++written;
        }
        std::printf("\n%d written.\n", written);
        return written > 0 ? 0 : 1;
    }

    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'.\n", title.c_str());
        return 1;
    }
    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }

    ScreenCapture cap;
    Frame frame;
    if (!cap.grab(input.target_window(), frame) || !frame.valid()) {
        std::fprintf(stderr, "Could not capture the window.\n");
        return 1;
    }

    if (o.survey) return run_survey(store, frame);

    const auto started = std::chrono::steady_clock::now();
    const auto hits = TemplateMatcher::find_all(
        frame, ScreenRect{ 0, 0, frame.width, frame.height }, *tpl, 0.6f, 8);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    std::printf("'%s' is %dx%d; searched a %dx%d screen in %.0f ms\n\n",
                which.c_str(), tpl->width, tpl->height, frame.width, frame.height, ms);
    if (hits.empty()) {
        std::printf("not on screen.\n");
        return 1;
    }
    for (const auto& h : hits)
        std::printf("  at %d,%d  %dx%d  score %.3f\n",
                    h.at.x, h.at.y, h.at.w, h.at.h, h.score);
    return 0;
}

/// Watch HP and MP for a while, the way the bot does.
///
/// A single look says whether a reading is possible. Only a run of them says
/// whether it is dependable, and that is the question a potion rule actually
/// asks: a reader that works nine times in ten has a tenth tick with no
/// opinion, and what matters is whether it then holds its last answer or
/// reports nonsense. The tally at the end is the honest version of "it works".
int run_vitals(const Options& o, const AppConfig& app, int seconds) {
    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'. Is the game running?\n", title.c_str());
        return 1;
    }

    MacroConfig mc;
    mc.load(resolve("configs/macro.ini"));

    VitalsReader reader;
    reader.set_target_window(input.target_window());
    reader.set_hp_bar(mc.hp_bar);
    reader.set_mp_bar(mc.mp_bar);
    reader.start();

    if (!reader.has_font())
        std::printf("No glyph set installed: the figures cannot be read, only "
                    "the bars measured.\n\n");

    std::printf("Watching for %d seconds. Spend some mana and take a hit, so "
                "the numbers have to move.\n\n", seconds);
    std::printf("   time  HP                        MP\n");

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int ticks = 0;
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto v = reader.values();
        std::printf("  %4ds  ", ++ticks / 2);
        if (v.have_hp()) std::printf("%7d / %-7d %3.0f%%  ", v.hp, v.hp_max,
                                     100.0 * v.hp / v.hp_max);
        else             std::printf("%-25s", "unread");
        if (v.have_mp()) std::printf("%7d / %-7d %3.0f%%", v.mp, v.mp_max,
                                     100.0 * v.mp / v.mp_max);
        else             std::printf("%s", "unread");
        std::printf("   (bars say %.0f%% / %.0f%%)\n",
                    reader.hp_percent(), reader.mp_percent());
    }

    const auto st = reader.stats();
    reader.stop();

    std::printf("\n%llu looks, %llu of them read the printed figures",
                static_cast<unsigned long long>(st.reads),
                static_cast<unsigned long long>(st.number_reads));
    if (st.reads)
        std::printf(" (%.0f%%)", 100.0 * st.number_reads / st.reads);
    std::printf(".\n%llu captures failed, bars relocated %llu time(s).\n",
                static_cast<unsigned long long>(st.failures),
                static_cast<unsigned long long>(st.relocations));
    return 0;
}

/// Call into the other helper's own compiled code.
///
/// Everything else here reads that assembly from the outside. This runs a .NET
/// runtime inside this process, loads the helper, and calls into it -- which is
/// the only way to use what it knows rather than what it looks like.
///
/// `--bridge Hello` says whether the assembly loads at all. `--bridge Probe`
/// lists what is in there that could be called, which is the question that
/// decides how much of this is worth doing: a feature written as part of a WPF
/// window cannot be called from outside, however much one would like to.
int run_bridge(const Options& o) {
    ClrBridge bridge;

    std::printf(".NET runtime : %s\n",
                ClrBridge::runtime_dir().empty() ? "not installed"
                                                 : ClrBridge::runtime_dir().c_str());
    std::printf("hosting lib  : %s\n",
                ClrBridge::hostfxr_path().empty() ? "not found"
                                                  : ClrBridge::hostfxr_path().c_str());
    std::printf("bridge       : %s\n\n", ClrBridge::bridge_assembly().c_str());

    std::string trouble;
    if (!bridge.start(&trouble)) {
        std::fprintf(stderr, "Could not start it: %s\n", trouble.c_str());
        return 1;
    }

    // Anything after the method name is handed to it, which is how Click is
    // told where to click.
    std::string answer;
    if (!bridge.call(o.bridge_method, o.bridge_arg, answer, &trouble)) {
        std::fprintf(stderr, "Calling %s failed: %s\n", o.bridge_method.c_str(),
                     trouble.c_str());
        return 1;
    }

    std::printf("--- GoBot.Bridge.%s ---\n%s\n", o.bridge_method.c_str(), answer.c_str());
    return 0;
}

/// Watch for party invitations and answer them.
///
/// The half of auto-party that nothing covered. The deciding code for inviting
/// others has been there a while; being invited was not handled at all, and
/// the client gives about ten seconds to answer before the modal disappears --
/// so an unattended run accepts none of them.
///
/// Nothing is clicked unless the name on the dialog passes the rules in
/// configs/party.ini. The same modal shape carries other requests, and a bot
/// that presses the confirm button on whatever appears will eventually confirm
/// something else.
int run_invites(const Options& o, const AppConfig& app, int seconds) {
    const std::string config = resolve("configs/party.ini");
    Party::Settings rules;
    rules.load(config);

    IniFile ini;
    ini.load(config);
    PartyInvite::Settings geometry;
    geometry.load(ini);

    TemplateStore store;
    if (!o.template_source.empty()) {
        const bool is_dll = o.template_source.size() > 4 &&
            _stricmp(o.template_source.c_str() + o.template_source.size() - 4,
                     ".dll") == 0;
        if (is_dll) store.load_from_assembly(o.template_source,
                                             resolve("configs/templates.ini"));
        else if (store.load(o.template_source))
            store.apply_names(resolve("configs/templates.ini"));
    } else if (!store.load_best(resolve("configs/templates.ini"))) {
        std::fprintf(stderr, "No UI pictures found.\n");
        return 1;
    }

    if (store.get(geometry.picture) == nullptr) {
        std::fprintf(stderr,
            "No picture called '%s'.\n\n"
            "That is the invitation dialog's footer -- the two buttons and the\n"
            "border beside them -- and without it there is nothing to recognise.\n"
            "It is cut out of a captured invitation rather than shipped, because\n"
            "it is the game's own artwork.\n", geometry.picture.c_str());
        return 1;
    }

    GlyphFont font;
    const std::string font_dir = GlyphFont::default_dir();
    const bool have_font = !font_dir.empty() && font.load(font_dir);
    if (!have_font)
        std::printf("No bitmap font, so the inviter's name cannot be read.\n"
                    "Invitations will be reported but never accepted.\n\n");

    InputManager input;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No game window.\n");
        return 1;
    }

    std::printf("Watching for invitations for %d seconds.\n\n"
                "  accept           %s\n"
                "  only from        %s\n"
                "  blacklist        %s (%zu names)\n\n",
                seconds, rules.auto_accept ? "yes" : "no",
                rules.accept_only.empty()
                    ? "anyone" : std::to_string(rules.accept_only.size()).c_str(),
                rules.use_blacklist ? "on" : "off", rules.blacklist.size());

    ScreenCapture capture;
    PartyInvite watcher;
    watcher.set_settings(geometry);

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::string answered;
    int seen = 0, accepted = 0;

    while (std::chrono::steady_clock::now() < until) {
        Frame frame;
        if (!capture.grab(input.target_window(), frame) || !frame.valid()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        const auto look = watcher.look(frame, store, have_font ? &font : nullptr);
        if (!look.up) {
            answered.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            continue;
        }

        // The same dialog stays up for ten seconds; answer it once.
        if (!look.who.empty() && look.who == answered) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            continue;
        }

        ++seen;
        std::printf("  invitation at %d,%d (%.3f)\n", look.at.x, look.at.y,
                    static_cast<double>(look.score));
        std::printf("    line read   \"%s\"\n", look.line.c_str());
        std::printf("    from        %s\n",
                    look.who.empty() ? "(could not read a name)" : look.who.c_str());

        if (!rules.auto_accept) {
            std::printf("    -> left alone: accepting is off\n\n");
            answered = look.who;
            continue;
        }
        // The name is only needed when a rule turns on it.
        //
        // Refusing to answer an unreadable name sounds careful and is not: a
        // live invitation read as "Exampley in" was declined on those grounds
        // while the settings said accept from anyone, so nothing happened and
        // the one rule actually in force was ignored. Ask for a name when
        // there is a list to check it against, and not otherwise.
        const bool name_matters =
            !rules.accept_only.empty() ||
            (rules.use_blacklist && !rules.blacklist.empty());

        if (look.who.empty() && name_matters) {
            std::printf("    -> left alone: there is a list to check against and "
                        "the name could not be read\n\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            continue;
        }
        // Bring the game up before clicking anything.
        //
        // Injected input goes to whatever has the foreground, not to the
        // window the coordinates were worked out against. Without this the
        // bot reports accepting an invitation, the click lands on whatever
        // happens to be sitting at that spot on the screen, and the party is
        // still "alone" afterwards -- which is exactly what the first live
        // test did, twice.
        if (!input.window_focused()) {
            input.focus_window();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        if (name_matters && !rules.may_invite(look.who)) {
            std::printf("    -> declined: the rules say no\n\n");
            input.click_at(look.decline_x, look.decline_y);
            answered = look.who;
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(geometry.settle_ms));
        const bool sent = input.click_at(look.accept_x, look.accept_y);
        std::printf("    -> clicked %d,%d%s%s\n", look.accept_x, look.accept_y,
                    sent ? "" : "  (SendInput refused it)",
                    input.window_focused() ? "" : "  (the game was NOT in front)");

        // Check the dialog actually went away.
        //
        // "Accepted" meaning "a click was sent" is the mistake this project
        // has already made once, on the popup closer: it reported closing six
        // dialogs while clicking an empty banner six times. A click that does
        // nothing looks identical to one that works unless somebody looks
        // afterwards, so this looks afterwards.
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        Frame after;
        bool gone = false;
        if (capture.grab(input.target_window(), after) && after.valid())
            gone = !watcher.look(after, store, nullptr).up;

        if (gone) {
            std::printf("    -> accepted; the dialog closed\n\n");
            ++accepted;
        } else {
            std::printf("    -> the dialog is STILL THERE, so that click did not "
                        "take\n\n");
            if (after.valid()) {
                const std::string shot = resolve("invite_not_taken.png");
                if (save_png(after, shot))
                    std::printf("       what the screen looked like: %s\n\n",
                                shot.c_str());
            }
        }
        answered = look.who;
    }

    std::printf("%d invitation(s) seen, %d accepted.\n", seen, accepted);
    return 0;
}

/// Invite a player by name, the way a person would.
///
/// Open the list of players on the map, find the row with that name on it,
/// pick it, press "Req Party". This is the acting half of auto-party, and it
/// is new: the deciding half -- noticing somebody typed the keyword in chat --
/// has been there a while and was connected to nothing, so a player could ask
/// to join all day and nothing would happen.
int run_invite_player(const Options& o, const AppConfig& app,
                      const std::string& who) {
    const std::string config = resolve("configs/party.ini");
    IniFile ini;
    ini.load(config);

    PlayerList::Settings geometry;
    geometry.load(ini);

    Party::Settings rules;
    rules.load(config);

    // The local picture set, not the helper's assembly.
    //
    // Two of the pictures this needs were cut out of live captures -- the
    // empty-row grid that says the list is open, and the button that opens it
    // -- so they sit beside this executable rather than inside somebody else's
    // DLL, and the whole set has to come from one place.
    TemplateStore store;
    if (!o.template_source.empty()) {
        const bool is_dll = o.template_source.size() > 4 &&
            _stricmp(o.template_source.c_str() + o.template_source.size() - 4,
                     ".dll") == 0;
        if (is_dll) store.load_from_assembly(o.template_source,
                                             resolve("configs/templates.ini"));
        else if (store.load(o.template_source))
            store.apply_names(resolve("configs/templates.ini"));
    } else if (!store.load_best(resolve("configs/templates.ini"))) {
        std::fprintf(stderr, "No UI pictures found.\n");
        return 1;
    }

    GlyphFont font;
    if (!font.load(GlyphFont::default_dir())) {
        std::fprintf(stderr,
            "No bitmap font, so the names in the list cannot be read -- and a\n"
            "row that cannot be read cannot be the right row.\n");
        return 1;
    }

    InputManager input;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No game window.\n");
        return 1;
    }

    if (!rules.may_invite(who)) {
        std::printf("'%s' is not someone the rules allow inviting.\n", who.c_str());
        return 1;
    }

    ScreenCapture capture;
    PlayerList list;
    list.set_settings(geometry);

    std::string why;
    if (!list.ensure_open(input, capture, store, &why)) {
        std::fprintf(stderr, "Could not open the player list: %s\n", why.c_str());
        return 1;
    }

    Frame frame;
    if (!capture.grab(input.target_window(), frame) || !frame.valid()) {
        std::fprintf(stderr, "Could not capture the list.\n");
        return 1;
    }

    const auto rows = list.read(frame, font);
    std::printf("%zu row(s) on the map:\n", rows.size());
    for (const auto& r : rows)
        std::printf("  %2d  y=%-4d %s\n", r.index + 1, r.y, r.name.c_str());

    const int at = list.find(rows, who);
    if (at < 0) {
        std::printf("\n'%s' is not in the list.\n", who.c_str());
        return 1;
    }

    const PlayerList::Row* row = nullptr;
    for (const auto& r : rows) if (r.index == at) row = &r;
    if (row == nullptr) return 1;

    std::printf("\n'%s' is row %d, read as '%s'. Inviting.\n",
                who.c_str(), row->index + 1, row->name.c_str());

    if (o.dry_run) {
        std::printf("Looking only -- would click %d,%d then %d,%d.\n",
                    geometry.click_x, row->y,
                    geometry.click_x + geometry.invite_dx,
                    row->y + geometry.invite_dy);
        return 0;
    }

    if (!list.invite(input, capture, store, *row, &why)) {
        std::fprintf(stderr, "The invite did not go: %s\n", why.c_str());
        return 1;
    }
    std::printf("Invited.\n");
    return 0;
}

/// Invite whoever asks for it in chat.
///
/// The two halves joined up at last. The deciding half -- read the chat, see
/// the keyword, work out who said it, vote on it, hold a cooldown -- has been
/// built and tested for a while and was wired to nothing. The acting half is
/// `PlayerList`. This is the loop between them.
int run_auto_party(const Options& o, const AppConfig& app, int seconds) {
    const std::string config = resolve("configs/party.ini");
    IniFile ini;
    ini.load(config);

    Party::Settings rules;
    rules.load(config);

    PlayerList::Settings geometry;
    geometry.load(ini);

    PartyChat::Settings chat_rules;
    chat_rules.keyword     = ini.get("Chat", "keyword", rules.magic_word);
    chat_rules.votes       = std::max(1, ini.get_int("Chat", "votes", 3));
    chat_rules.vote_gap_ms = std::max(50, ini.get_int("Chat", "vote_gap_ms", 150));
    chat_rules.cooldown_s  = std::max(1, ini.get_int("Chat", "cooldown_s", 30));
    chat_rules.min_name_match = ini.get_float("Chat", "min_name_match", 0.7f);
    chat_rules.whole_word_only = ini.get_bool("Chat", "whole_word_only", true);

    // Where the chat lines are, measured on a 1024x768 client: two readable
    // lines at y 710 and 723, so a pitch of 13, running x 6 to 396.
    const int chat_x = ini.get_int("Chat", "chat_x", 6);
    const int chat_y = ini.get_int("Chat", "chat_y", 707);
    const int chat_w = ini.get_int("Chat", "chat_width", 390);
    const int line_h = std::max(6, ini.get_int("Chat", "line_height", 13));
    const int line_n = std::max(1, ini.get_int("Chat", "lines", 3));

    TemplateStore store;
    if (!store.load_best(resolve("configs/templates.ini"))) {
        std::fprintf(stderr, "No UI pictures found.\n");
        return 1;
    }

    GlyphFont font;
    if (!font.load(GlyphFont::default_dir())) {
        std::fprintf(stderr, "No bitmap font, so the chat cannot be read.\n");
        return 1;
    }

    InputManager input;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No game window.\n");
        return 1;
    }

    std::printf("Inviting on '%s' for %d seconds.\n\n"
                "  seen this often  %d times, %dms apart\n"
                "  cooldown         %ds per player\n"
                "  chat read at     %d,%d  %dx%d, %d line(s)\n\n",
                chat_rules.keyword.c_str(), seconds,
                chat_rules.votes, chat_rules.vote_gap_ms, chat_rules.cooldown_s,
                chat_x, chat_y, chat_w, line_h * line_n, line_n);

    ScreenCapture capture;
    PartyChat chat;
    chat.set_settings(chat_rules);
    PlayerList list;
    list.set_settings(geometry);

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int asked = 0, invited = 0;
    std::string last_seen;

    while (std::chrono::steady_clock::now() < until) {
        Frame frame;
        if (!capture.grab(input.target_window(), frame) || !frame.valid()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(chat_rules.vote_gap_ms));
            continue;
        }

        std::vector<std::string> lines;
        for (int i = 0; i < line_n; ++i) {
            const ScreenRect box{ chat_x, chat_y + i * line_h, chat_w, line_h + 3 };
            if (box.y + box.h > frame.height) break;
            const std::string text = font.read_line(frame, box);
            if (!text.empty()) lines.push_back(text);
        }

        if (!lines.empty() && lines.back() != last_seen) {
            last_seen = lines.back();
            if (o.verbose)
                for (const std::string& l : lines)
                    std::printf("  chat: %s\n", l.c_str());
        }

        const std::uint64_t now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        for (const std::string& who : chat.saw(lines, now)) {
            ++asked;
            std::printf("  '%s' asked to join\n", who.c_str());

            if (!rules.auto_invite) {
                std::printf("    -> left alone: inviting is off\n");
                continue;
            }
            if (!rules.may_invite(who)) {
                std::printf("    -> left alone: the rules say no\n");
                continue;
            }

            std::string why;
            if (!list.ensure_open(input, capture, store, &why)) {
                std::printf("    -> could not open the list: %s\n", why.c_str());
                continue;
            }

            Frame open_now;
            if (!capture.grab(input.target_window(), open_now) || !open_now.valid())
                continue;

            const auto rows = list.read(open_now, font);
            const int at = list.find(rows, who);
            if (at < 0) {
                std::printf("    -> not among the %zu on this map\n", rows.size());
                continue;
            }

            const PlayerList::Row* row = nullptr;
            for (const auto& r : rows) if (r.index == at) row = &r;
            if (row == nullptr) continue;

            if (!list.invite(input, capture, store, *row, &why)) {
                std::printf("    -> the invite did not go: %s\n", why.c_str());
                continue;
            }
            std::printf("    -> invited (row %d, read as '%s')\n",
                        row->index + 1, row->name.c_str());
            ++invited;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(chat_rules.vote_gap_ms));
    }

    std::printf("\n%d asked, %d invited.\n", asked, invited);
    return 0;
}

/// What the party looks like, read by the other helper's own detector.
int run_party(const Options& o, const AppConfig& app) {
    Party::Settings settings;
    const std::string config = resolve("configs/party.ini");
    if (settings.load(config)) {
        std::printf("settings from %s\n", config.c_str());
        std::printf("  accept invitations   %s\n", settings.auto_accept ? "yes" : "no");
        std::printf("  invite others        %s\n", settings.auto_invite ? "yes" : "no");
        std::printf("  only while leading   %s\n", settings.always_leader ? "yes" : "no");
        std::printf("  invite on chat word  %s\n",
                    settings.by_magic_word ? settings.magic_word.c_str() : "(off)");
        std::printf("  blacklist            %s (%zu names)\n",
                    settings.use_blacklist ? "on" : "off", settings.blacklist.size());
        std::printf("  share experience     %s\n",
                    settings.exp_share == Party::ExpShare::More ? "weighted" : "equally");
        std::printf("  share loot           %s\n\n",
                    settings.loot_share == Party::LootShare::Contribution
                        ? "by contribution" : "free for all");
    } else {
        std::printf("No %s yet; using defaults.\n\n", config.c_str());
    }

    InputManager input;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No game window.\n");
        return 1;
    }

    ClrBridge bridge;
    Party party;
    party.set_settings(settings);

    const Party::State state = party.look(bridge, input.target_window());
    if (!state.read) {
        std::fprintf(stderr,
            "Could not read the party: %s\n\n"
            "This one feature is read through the other helper's own detector,\n"
            "which needs .NET and that helper installed, plus GoBotBridge.dll\n"
            "built beside this executable (bridge\\build.ps1).\n",
            state.trouble.c_str());
        return 1;
    }

    std::printf("right now: %s\n", Party::status_name(state.status));
    std::printf("  in a party           %s\n", state.in_party ? "yes" : "no");
    std::printf("  leading it           %s\n", state.is_leader ? "yes" : "no");
    std::printf("\ntheir rules allow:\n");
    std::printf("  accepting            %s\n", state.can_accept ? "yes" : "no");
    std::printf("  inviting             %s\n", state.can_invite ? "yes" : "no");
    std::printf("  handing over the key %s\n", state.can_transfer_key ? "yes" : "no");

    if (!state.points.empty()) {
        std::printf("\nwhere to click, worked out by their code for a %d wide client:\n",
                    state.client_width);
        for (const auto& [name, at] : state.points)
            std::printf("  %-20s %d,%d\n", name.c_str(), at.x, at.y);
    }
    return 0;
}

/// Open the player list and read it.
///
/// This is the half of inviting that is still unknown. Deciding who to invite
/// is done; doing it means opening the list of players on the map, finding the
/// one who asked, clicking their line, and pressing invite. The other helper's
/// configuration names every piece of that -- where the opening button is, the
/// list's rectangle, how tall a line is, where to click one, how far below the
/// selected line the invite button sits -- and ships every one as zero, because
/// those come from its own settings rather than from its code.
///
/// So they have to be measured, and this measures them: open the list, save
/// what appeared, work out its rectangle from what changed on screen, and read
/// every row with the game's own font. The spacing between the rows that say
/// something is the line height.
int run_player_list(const Options& o, const AppConfig& app) {
    TemplateStore store;
    const std::string dll = o.template_source.empty()
        ? TemplateStore::helper_assembly() : o.template_source;
    if (dll.empty() || !store.load_from_assembly(dll, {})) {
        std::fprintf(stderr, "No pictures to find the button with.\n");
        return 1;
    }

    GlyphFont font;
    const bool have_font = font.load(GlyphFont::default_dir());

    InputManager input;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No game window.\n");
        return 1;
    }
    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    ScreenCapture capture;
    Frame before;
    if (!capture.grab(input.target_window(), before) || !before.valid()) {
        std::fprintf(stderr, "Could not capture the window.\n");
        return 1;
    }

    const Frame* button = store.get("player_in_map_icon");
    if (!button) {
        std::fprintf(stderr, "No 'player_in_map_icon' in that picture set.\n");
        return 1;
    }

    const auto found = TemplateMatcher::find(
        before, ScreenRect{ 0, 0, before.width, before.height }, *button, 0.85f);
    if (!found.found) {
        std::fprintf(stderr,
            "The players-on-map button is not on screen. It is what opens the\n"
            "list, so nothing below can be measured without it.\n");
        return 1;
    }
    std::printf("opener at %d,%d %dx%d (%.3f)\n", found.at.x, found.at.y,
                found.at.w, found.at.h, static_cast<double>(found.score));

    input.click_at(found.at.x + found.at.w / 2, found.at.y + found.at.h / 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(900));

    Frame after;
    if (!capture.grab(input.target_window(), after) || !after.valid()) {
        std::fprintf(stderr, "Could not capture after clicking.\n");
        return 1;
    }
    const std::string shot = resolve("playerlist.png");
    save_png(after, shot);
    std::printf("what appeared is saved to %s\n\n", shot.c_str());

    // Where the screen changed. The list is whatever was not there before, so
    // its rectangle falls out of the difference rather than being guessed at.
    int min_x = after.width, min_y = after.height, max_x = -1, max_y = -1;
    for (int y = 0; y < after.height; ++y) {
        for (int x = 0; x < after.width; ++x) {
            std::uint8_t r1, g1, b1, r2, g2, b2;
            before.get(x, y, r1, g1, b1);
            after.get(x, y, r2, g2, b2);
            if (std::abs(r1 - r2) + std::abs(g1 - g2) + std::abs(b1 - b2) < 40) continue;
            min_x = std::min(min_x, x); max_x = std::max(max_x, x);
            min_y = std::min(min_y, y); max_y = std::max(max_y, y);
        }
    }

    if (max_x < 0) {
        std::printf("Nothing on screen changed, so the click opened nothing.\n");
        return 1;
    }
    std::printf("what changed: %d,%d  %dx%d\n\n",
                min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);

    if (!have_font) {
        std::printf("No glyph set installed, so the lines cannot be read.\n");
        return 0;
    }

    std::printf("rows that say something (the gaps between them are the line height):\n");
    int said = 0;
    for (int y = min_y; y + 14 <= max_y + 1; ++y) {
        const ScreenRect row{ min_x, y, max_x - min_x + 1, 14 };
        const std::string text = font.read_line(after, row);
        if (text.size() < 2) continue;
        std::printf("  y=%-4d %s\n", y, text.c_str());
        if (++said >= 40) break;
    }
    if (said == 0)
        std::printf("  (nothing readable -- look at the saved picture)\n");

    // Selecting a row, to find where the invite button appears.
    //
    // It is the one number left, and it cannot be had from an unselected list:
    // the button is not drawn until a player is picked. So pick one, capture
    // again, and what differs between the two is the button. Both pictures
    // have the list open, so the moving scenery behind it is much the same in
    // each and what stands out is what selecting did.
    if (o.pick_row > 0) {
        IniFile ini;
        ini.load(resolve("configs/party.ini"));
        const int click_x = ini.get_int("PlayerList", "click_x", 862);
        const int first_y = ini.get_int("PlayerList", "first_row_y", 172);
        const int row_h   = ini.get_int("PlayerList", "row_height", 20);
        const int row_y   = first_y + (o.pick_row - 1) * row_h + row_h / 2;

        std::printf("\nselecting row %d at %d,%d\n", o.pick_row, click_x, row_y);
        input.click_at(click_x, row_y);
        std::this_thread::sleep_for(std::chrono::milliseconds(900));

        Frame picked;
        if (!capture.grab(input.target_window(), picked) || !picked.valid()) {
            std::fprintf(stderr, "Could not capture after selecting.\n");
            return 1;
        }
        const std::string shot2 = resolve("playerlist_picked.png");
        save_png(picked, shot2);
        std::printf("saved to %s\n", shot2.c_str());

        int bx0 = picked.width, by0 = picked.height, bx1 = -1, by1 = -1;
        for (int y = 0; y < picked.height; ++y)
            for (int x = 0; x < picked.width; ++x) {
                std::uint8_t r1, g1, b1, r2, g2, b2;
                after.get(x, y, r1, g1, b1);
                picked.get(x, y, r2, g2, b2);
                if (std::abs(r1 - r2) + std::abs(g1 - g2) + std::abs(b1 - b2) < 60) continue;
                bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
                by0 = std::min(by0, y); by1 = std::max(by1, y);
            }

        if (bx1 < 0) {
            std::printf("nothing changed -- the click selected nothing\n");
        } else {
            std::printf("selecting changed: %d,%d  %dx%d\n",
                        bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
            std::printf("  which is %+d,%+d from the row clicked\n",
                        bx0 - click_x, by0 - row_y);
        }
    }
    return 0;
}

/// Close whatever dialogs are in the way, for real.
///
/// The smallest thing in this project that works end to end: look at the game,
/// find a close button, click it, check it went away, repeat. It needs no click
/// offsets -- the middle of a close button is the close button -- so it is the
/// one feature that can be trusted before anything has been calibrated, and
/// therefore the right first thing to run against a live client.
int run_close(const Options& o, const AppConfig& app) {
    TemplateStore store;
    const std::string dll = o.template_source.empty()
        ? TemplateStore::helper_assembly() : o.template_source;
    if (dll.empty() || !store.load_from_assembly(dll, {})) {
        std::fprintf(stderr, "No pictures to recognise dialogs with.\n");
        return 1;
    }

    InputManager input;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No game window.\n");
        return 1;
    }
    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    ScreenCapture capture;
    DialogDriver driver(input, store, [&](Frame& f) {
        return capture.grab(input.target_window(), f);
    });

    std::printf("Looking for anything in the way...\n");
    const int closed = driver.close_popups(6);
    if (closed == 0) {
        std::printf("Nothing was in the way.\n");
        return 0;
    }
    std::printf("Closed %d.\n", closed);
    return 0;
}

/// List the routines, or run one.
///
/// With --dry it finds each step's picture and says where it would click,
/// without clicking. That is the mode that matters while the tables are being
/// written: the click offsets and the step order in configs/routines.ini were
/// worked out from the pictures' names rather than by watching the game, so the
/// first pass over any routine is expected to be wrong somewhere, and finding
/// out by watching it misclick through a shop is the expensive way.
int run_routines(const Options& o, const AppConfig& app) {
    const std::string path = resolve("configs/routines.ini");
    const auto routines = Routine::load(path);
    if (routines.empty()) {
        std::fprintf(stderr, "No routines in %s\n", path.c_str());
        return 1;
    }

    // The launcher's pictures and the game's both, since routines draw on
    // whichever has what they need.
    TemplateStore store;
    const std::string dll = o.template_source.empty()
        ? TemplateStore::helper_assembly() : o.template_source;
    if (!dll.empty()) store.load_from_assembly(dll, resolve("configs/templates.ini"));

    if (o.routine_name.empty()) {
        std::printf("%zu routines in %s\n\n", routines.size(), path.c_str());
        for (const Routine& r : routines) {
            std::printf("  %-22s %zu step%s\n", r.name.c_str(), r.steps.size(),
                        r.steps.size() == 1 ? "" : "s");
            for (const DialogDriver::Step& s : r.steps)
                std::printf("      %-24s -> %s%s\n", s.find.c_str(),
                            s.gone ? "(it goes away)" : s.confirm.c_str(),
                            s.optional ? "   [optional]" : "");
        }
        const auto missing = Routine::missing_pictures(routines, store);
        if (!missing.empty()) {
            std::printf("\nPictures named but not loaded:\n");
            for (const std::string& m : missing) std::printf("  %s\n", m.c_str());
        }
        return 0;
    }

    const Routine* routine = Routine::find(routines, o.routine_name);
    if (!routine) {
        std::fprintf(stderr, "No routine called '%s'.\n", o.routine_name.c_str());
        return 1;
    }

    InputManager input;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No game window.\n");
        return 1;
    }
    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }

    ScreenCapture capture;
    auto grab = [&](Frame& f) { return capture.grab(input.target_window(), f); };

    if (o.dry_run) {
        std::printf("'%s', %zu steps -- looking only, clicking nothing.\n\n",
                    routine->name.c_str(), routine->steps.size());

        Frame frame;
        if (!grab(frame) || !frame.valid()) {
            std::fprintf(stderr, "Could not capture the window.\n");
            return 1;
        }

        int found = 0;
        for (std::size_t i = 0; i < routine->steps.size(); ++i) {
            const DialogDriver::Step& step = routine->steps[i];
            std::printf("  %zu. %-24s ", i + 1, step.find.c_str());

            const Frame* picture = store.get(step.find);
            if (!picture) { std::printf("no such picture\n"); continue; }

            const auto hit = TemplateMatcher::find(
                frame, ScreenRect{ 0, 0, frame.width, frame.height }, *picture, step.score);
            if (!hit.found) {
                std::printf("not on screen\n");
                continue;
            }
            ++found;
            const int cx = hit.at.x + (step.click_dx >= 0 ? step.click_dx : hit.at.w / 2);
            const int cy = hit.at.y + (step.click_dy >= 0 ? step.click_dy : hit.at.h / 2);
            std::printf("at %d,%d (%.2f) -- would click %d,%d\n",
                        hit.at.x, hit.at.y, static_cast<double>(hit.score), cx, cy);
        }

        std::printf("\n%d of %zu steps are on screen right now. That is expected to "
                    "be one:\na routine is a sequence, so only its first step can be "
                    "up before it runs.\n", found, routine->steps.size());
        return 0;
    }

    DialogDriver driver(input, store, grab);
    driver.on_step_report([](const std::string& line) {
        std::printf("  %s\n", line.c_str());
        std::fflush(stdout);
    });

    std::string trouble;
    const std::size_t done = driver.run_all(routine->steps, &trouble);
    if (done == routine->steps.size()) {
        std::printf("\n'%s' finished.\n", routine->name.c_str());
        return 0;
    }
    std::fprintf(stderr, "\nStopped after %zu of %zu steps: %s\n",
                 done, routine->steps.size(), trouble.c_str());
    return 1;
}

/// Write the account down, once, so the flow can log in without being asked.
///
/// The password is read with the console's echo turned off and is never put on
/// a command line -- a command line is visible to every process on the machine
/// and ends up in the shell's history besides. What lands in the file is
/// encrypted with this Windows account's own key.
int run_set_login(const Options&) {
    const std::string path = resolve("configs/login.ini");

    AutoLogin::Settings settings;
    if (!settings.load(path)) {
        settings.pages = AutoLogin::default_pages();
        settings.launcher_window = "VTCZone";
        // client_window stays empty on purpose: the game's launcher has no
        // title and is found by its class and program instead.
        settings.game_window = "GhostOnlineVTC";
    }

    std::printf("Account for the auto-login, written to %s\n\n", path.c_str());

    std::printf("Username: ");
    std::fflush(stdout);
    std::string username;
    if (!std::getline(std::cin, username) || username.empty()) {
        std::fprintf(stderr, "\nNothing entered; nothing written.\n");
        return 1;
    }

    // Echo off for the password.
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool is_console = GetConsoleMode(in, &mode) != 0;
    if (is_console) SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));

    std::printf("Password: ");
    std::fflush(stdout);
    std::string password;
    const bool got = static_cast<bool>(std::getline(std::cin, password));

    if (is_console) SetConsoleMode(in, mode);
    std::printf("\n");

    if (!got || password.empty()) {
        std::fprintf(stderr, "Nothing entered; nothing written.\n");
        return 1;
    }

    const std::string protected_text = AutoLogin::protect(password);
    // Scrub the copy in this process before it can reach a crash dump.
    SecureZeroMemory(password.data(), password.size());
    if (protected_text.empty()) {
        std::fprintf(stderr, "Windows would not encrypt it; nothing written.\n");
        return 1;
    }

    settings.username = username;
    settings.password_protected = protected_text;

    // Two keys changed in place when the file already exists, rather than the
    // whole thing rewritten.
    //
    // login.ini is written in by hand: every page of the crossing carries the
    // measurements that identify it and the reasoning behind each number.
    // Saving the settings object over the file keeps the values and throws
    // away all of that, which would be a steep price for typing in a password.
    const bool existed = std::ifstream(path).good();
    const bool wrote =
        existed
            ? (IniFile::set_in_place(path, "Login", "username", username) &&
               IniFile::set_in_place(path, "Login", "password", protected_text))
            : settings.save(path);
    if (!wrote) {
        std::fprintf(stderr, "Could not write %s\n", path.c_str());
        return 1;
    }

    std::printf("Written. The password is encrypted with your Windows account's\n"
                "own key, so the file is of no use to another user or on another\n"
                "machine -- including after you copy this folder somewhere else.\n");
    return 0;
}

/// Everything the crossing needs before it can run: the table, and the
/// pictures that identify its pages.
///
/// Shared, because `--keepalive` runs the same crossing as `--login` and the
/// two drifting apart would mean a bot that logs in differently when it is
/// recovering from a crash than when a person asked it to.
bool login_setup(const Options& o, const AppConfig& app, AutoLogin::Settings& settings,
                 TemplateStore& store, std::string& path, std::string& source) {
    path = resolve("configs/login.ini");

    if (!settings.load(path)) {
        settings.pages = AutoLogin::default_pages();
        settings.launcher_window = "VTCZone";
        // No client_window: the game's launcher is untitled. See the header.
        settings.game_window = o.window.empty() ? app.window_title : o.window;
        settings.save(path);
        std::printf("Wrote a blank %s to fill in.\n\n", path.c_str());
    }

    // The launcher's pages are not in the game's own picture set.
    //
    // They were only ever in the other helper's assembly, so a machine that has
    // the game and not that helper had nothing to match them against and the
    // crossing failed on its first page. Copies now sit beside this executable
    // and are named in configs/templates.ini, and the local set is used when
    // the helper is not installed.
    source = o.template_source.empty() ? TemplateStore::helper_assembly()
                                       : o.template_source;
    bool loaded = false;
    if (!source.empty()) {
        const bool is_dll = source.size() > 4 &&
            _stricmp(source.c_str() + source.size() - 4, ".dll") == 0;
        if (is_dll)
            loaded = store.load_from_assembly(source, resolve("configs/templates.ini"));
        else if (store.load(source)) {
            store.apply_names(resolve("configs/templates.ini"));
            loaded = true;
        }
    }
    if (!loaded && store.load_best(resolve("configs/templates.ini")))
        source = store.source();

    const auto missing = settings.unidentified();
    if (missing.empty()) return true;

    std::printf("The crossing has %zu pages. %zu of them have no picture named\n"
                "for them yet, so the flow cannot run:\n\n",
                settings.pages.size(), missing.size());
    for (const AutoLogin::Page* page : missing)
        std::printf("  %-16s %s\n", page->key.c_str(),
                    AutoLogin::step_name(page->step));
    std::printf("\nTo identify one: get the launcher onto that page, then run\n\n"
                "  gobot_cli --window \"%s\" --tplsrc \"%s\" --survey\n\n"
                "and write what it names into [Page.<key>] picture= in\n%s\n",
                settings.launcher_window.c_str(),
                source.empty() ? "<a picture set>" : source.c_str(), path.c_str());
    return false;
}

/// Cross from a cold machine to a character standing in the world.
int run_login(const Options& o, const AppConfig& app) {
    std::string path, source;
    AutoLogin::Settings settings;
    TemplateStore store;
    if (!login_setup(o, app, settings, store, path, source)) return 1;

    // Looking, not doing.
    //
    // Running the real thing restarts the client, so it is not something to
    // try while somebody is playing -- and it needs an account besides. This
    // checks the table instead: for each page, is its picture on screen right
    // now, and where would it click? Most will be absent, because a sequence
    // only ever has one of its pages up at a time. What matters is that the
    // one that IS up is found, and found where it actually is.
    if (o.dry_run) {
        std::printf("Looking only -- nothing will be clicked.\n\n");

        // Three windows, not two.
        //
        // The crossing passes through the publisher's launcher, then the
        // game's own launcher, then the game -- and the middle one is a
        // separate window belonging to a separate program. Leaving it out of
        // this report meant the page that lives on it always read "not on
        // screen", which is the opposite of the truth and was believed for
        // longer than it should have been.
        struct Look {
            const char*  label;
            std::string  shown;
            InputManager input;
            Frame        frame;
            bool         open = false;
        };
        Look seen[3];
        seen[0].label = "publisher's launcher";
        seen[1].label = "game launcher";
        seen[2].label = "game";
        seen[0].shown = settings.launcher_window.empty() ? "(untitled)"
                                                         : settings.launcher_window;
        seen[1].shown = settings.client_window.empty()
            ? settings.client_process : settings.client_window;
        seen[2].shown = settings.game_window.empty() ? settings.game_process
                                                     : settings.game_window;

        seen[0].open = seen[0].input.find_game_window(settings.launcher_window, {}, {});
        seen[1].open = seen[1].input.find_game_window(
            settings.client_window, settings.client_class, settings.client_process);
        seen[2].open = seen[2].input.find_game_window(
            settings.game_window, settings.game_class, settings.game_process);

        ScreenCapture capture;
        for (Look& look : seen) {
            std::printf("%-22s %-24s %s\n", look.label, look.shown.c_str(),
                        look.open ? "open" : "not running");
            if (look.open) capture.grab(look.input.target_window(), look.frame);
        }
        std::printf("\n");

        int on_screen = 0;
        for (const AutoLogin::Page& page : settings.pages) {
            std::printf("  %-16s %-22s ", page.key.c_str(), page.picture.c_str());

            const Frame* picture = store.get(page.picture);
            if (!picture) { std::printf("no such picture\n"); continue; }

            bool found = false;
            for (const auto& look : seen) {
                if (!look.frame.valid()) continue;
                const auto hit = TemplateMatcher::find(
                    look.frame, ScreenRect{ 0, 0, look.frame.width, look.frame.height },
                    *picture, page.score);
                if (!hit.found) continue;

                const int cx = hit.at.x + (page.click_dx >= 0 ? page.click_dx : hit.at.w / 2);
                const int cy = hit.at.y + (page.click_dy >= 0 ? page.click_dy : hit.at.h / 2);
                std::printf("at %d,%d in the %s (%.3f) -- would click %d,%d\n",
                            hit.at.x, hit.at.y, look.label,
                            static_cast<double>(hit.score), cx, cy);
                found = true;
                ++on_screen;
                break;
            }
            if (!found) std::printf("not on screen\n");
        }

        std::printf("\n%d of %zu pages are up right now.\n\n"
                    "A small number is the healthy one. The crossing is a "
                    "sequence, but its\nthree windows do not close behind it -- "
                    "the publisher's launcher stays open\nunderneath -- and the "
                    "two connect steps are the same button, so they are\nboth "
                    "found whenever either is up. What matters is that the page "
                    "the flow\nis standing on is among them, and found where it "
                    "actually is.\n",
                    on_screen, settings.pages.size());
        return 0;
    }

    AutoLogin flow;
    flow.set_settings(settings);
    flow.set_templates(&store);
    if (!o.login_upto.empty()) {
        const bool known = std::any_of(
            settings.pages.begin(), settings.pages.end(),
            [&](const AutoLogin::Page& p) { return p.key == o.login_upto; });
        if (!known) {
            std::fprintf(stderr, "No page called '%s'. The pages are:\n",
                         o.login_upto.c_str());
            for (const AutoLogin::Page& p : settings.pages)
                std::fprintf(stderr, "  %s\n", p.key.c_str());
            return 2;
        }
        flow.halt_before(o.login_upto);
        std::printf("Crossing as far as '%s' and stopping there.\n\n",
                    o.login_upto.c_str());
    }
    flow.on_step([](AutoLogin::Step s, const std::string& detail) {
        std::printf("  [%-28s] %s\n", AutoLogin::step_name(s), detail.c_str());
        std::fflush(stdout);
    });

    const bool ok = flow.run();
    if (!ok) std::fprintf(stderr, "\nStopped: %s\n", flow.trouble().c_str());
    return ok ? 0 : 1;
}

/// Check everything this program needs, and say what is wrong.
///
/// Written because "it does not work on another PC" is not something that can
/// be fixed from this one. Every assumption GoBot makes about the machine it
/// is on is checked here in one go, so a person on that machine can run one
/// command and send back an answer instead of a dozen rounds of guessing.
int run_doctor(const Options& o, const AppConfig& app) {
    int problems = 0;
    auto good = [](const char* what, const std::string& detail) {
        std::printf("  [ ok ] %-22s %s\n", what, detail.c_str());
    };
    auto bad = [&](const char* what, const std::string& detail) {
        std::printf("  [FAIL] %-22s %s\n", what, detail.c_str());
        ++problems;
    };
    auto note = [](const char* what, const std::string& detail) {
        std::printf("  [    ] %-22s %s\n", what, detail.c_str());
    };

    std::printf("GoBot doctor\n\n");

    // ---- the machine ----
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        note("running from", path);

        OSVERSIONINFOEXA os{};
        os.dwOSVersionInfoSize = sizeof(os);
        char build[64]{};
        DWORD size = sizeof(build);
        RegGetValueA(HKEY_LOCAL_MACHINE,
                     "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     "CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &size);
        note("windows build", build[0] ? build : "unknown");

        // Elevation. Without it Windows silently drops every injected
        // keystroke, which looks exactly like the bot being broken.
        bool elevated = false;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION e{};
            DWORD got = 0;
            if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got))
                elevated = e.TokenIsElevated != 0;
            CloseHandle(token);
        }
        if (elevated) good("administrator", "yes");
        else bad("administrator", "NO -- keys and clicks will go nowhere");
    }

    // ---- the pictures ----
    TemplateStore store;
    if (!store.load_best(resolve("configs/templates.ini"))) {
        bad("UI pictures", "none found anywhere");
    } else {
        good("UI pictures", std::to_string(store.size()) + " from " + store.source());

        // The ones features actually look up by name. A set that loaded but
        // has none of these is a set from somewhere unexpected.
        const char* needed[] = {
            "hpmp_indicator2", "start_game_page", "start_game_button",
            "game_ready_indicator", "channel_first_index",
            "select_char_indicator", "login_popup1",
            "party_invite_buttons", "player_list_grid", "player_in_map_icon",
        };
        std::string absent;
        for (const char* n : needed)
            if (store.get(n) == nullptr) absent += std::string(absent.empty() ? "" : ", ") + n;
        if (absent.empty()) good("named pictures", "all present");
        else bad("named pictures", "missing: " + absent);
    }

    // ---- the glyphs ----
    {
        const std::string dir = GlyphFont::default_dir();
        if (dir.empty()) {
            bad("bitmap font", "no bitmap_font folder found -- HP/MP, chat and "
                               "the player list cannot be read");
        } else {
            GlyphFont font;
            if (!font.load(dir)) bad("bitmap font", "found at " + dir + " but nothing loaded");
            else good("bitmap font", std::to_string(font.size()) + " glyphs from " + dir);
        }
    }

    // ---- the configs ----
    {
        const char* files[] = { "app.ini", "macro.ini", "login.ini", "party.ini",
                                "templates.ini", "routines.ini" };
        std::string absent;
        for (const char* f : files) {
            const std::string p = resolve(std::string("configs/") + f);
            if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES)
                absent += std::string(absent.empty() ? "" : ", ") + f;
        }
        if (absent.empty()) good("configs", "all present");
        else bad("configs", "missing: " + absent);
    }

    // ---- the game ----
    {
        InputManager input;
        if (attach_window(input, o, app)) {
            int w = 0, h = 0;
            input.window_size(w, h);
            good("game window", "found, client " + std::to_string(w) + "x" +
                                std::to_string(h));
            if (w != 1024 || h != 768)
                note("", "every measurement in the configs was taken on 1024x768; "
                         "another size needs them re-measured");

            ScreenCapture cap;
            Frame frame;
            if (cap.grab(input.target_window(), frame) && frame.valid())
                good("screen capture", std::to_string(frame.width) + "x" +
                                       std::to_string(frame.height) +
                                       (cap.from_screen() ? " (off the screen -- keep "
                                                            "the game unobscured)" : ""));
            else bad("screen capture", "could not capture the game window");
        } else {
            bad("game window", "not found");

            // Say what it was looking for, and what is actually there.
            //
            // "Not found" on its own is useless from another machine: the
            // window may be named differently on another build, or its process
            // name may not be readable at all. Printing both sides turns a
            // round of guessing into one answer.
            std::printf("         looking for   title~'%s' class='%s' process='%s'\n",
                        app.window_title.c_str(), app.window_class.c_str(),
                        app.process_name.c_str());
            std::printf("         on screen now:\n");
            for (const auto& w : InputManager::enumerate_windows()) {
                RECT r{};
                int ww = 0, wh = 0;
                if (GetWindowRect(static_cast<HWND>(w.hwnd), &r)) {
                    ww = static_cast<int>(r.right - r.left);
                    wh = static_cast<int>(r.bottom - r.top);
                }
                std::printf("           %-28s %-22s %-20s %dx%d\n",
                            w.title.empty() ? "(untitled)" : w.title.c_str(),
                            w.window_class.c_str(),
                            w.process.empty() ? "(unreadable)" : w.process.c_str(),
                            ww, wh);
            }
            std::printf("         If the game is in that list, copy its class and\n"
                        "         process into [Window] in configs/app.ini.\n");
        }
    }

    // ---- the launcher, for auto-login ----
    {
        AutoLogin::Settings login;
        if (!login.load(resolve("configs/login.ini"))) {
            note("auto-login", "configs/login.ini will not load");
        } else if (login.launcher_path.empty()) {
            note("auto-login", "no launcher written down");
        } else if (GetFileAttributesA(login.launcher_path.c_str()) ==
                   INVALID_FILE_ATTRIBUTES) {
            bad("auto-login launcher", login.launcher_path +
                " does not exist on this machine -- set `launcher` in "
                "configs/login.ini to where VTCZone is installed here");
        } else {
            good("auto-login launcher", login.launcher_path);
        }
        if (login.username.empty())
            note("auto-login account", "none written down -- run --setlogin");
    }

    // ---- the optional things ----
    {
        const HMODULE wpcap = LoadLibraryA("wpcap.dll");
        if (wpcap) { note("npcap", "installed (only the protocol tools use it)");
                     FreeLibrary(wpcap); }
        else note("npcap", "not installed -- fine, nothing that plays the game needs it");

        note("dotnet", ClrBridge::runtime_dir().empty()
                           ? "not installed -- fine, only the party-status "
                             "reading uses it"
                           : ClrBridge::runtime_dir());
    }

    std::printf("\n");
    if (problems == 0) {
        std::printf("Nothing wrong that this can see.\n");
        return 0;
    }
    std::printf("%d problem(s) above. Send this whole output back.\n", problems);
    return 1;
}

/// One look at whether the client has fallen over, printed.
///
/// For checking the detection itself, which is otherwise only exercised at the
/// moment it matters. Run it with the game up and it should say the client is
/// fine; close the game and it should say so; and when the client does put up
/// an error box, this prints the box's text and the code read out of it.
int run_crash_check(const Options& o) {
    IniFile ini;
    ini.load(resolve(o.app_path));
    CrashWatch::Settings settings;
    settings.load(ini);

    CrashWatch watch(settings);
    const CrashWatch::Look seen = watch.look();

    std::printf("%s\n", CrashWatch::trouble_name(seen.what));
    if (!seen.detail.empty()) std::printf("\n%s\n", seen.detail.c_str());
    if (!seen.code.empty())   std::printf("\nerror code: %s\n", seen.code.c_str());

    if (!seen.bad())
        std::printf("\nNothing to recover from. `--keepalive <minutes>` is the one\n"
                    "that acts: it watches for exactly this, and when it happens it\n"
                    "ends the client and crosses back in.\n");
    else
        std::printf("\n`--keepalive <minutes>` would end the client here and log "
                    "back in.\n");
    return 0;
}

/// Keep the client alive: notice a crash, end it, and cross back in.
///
/// This is the half of an unattended run that nothing else covers. The session
/// watcher notices that the HUD is gone, which is the right question when the
/// session is merely interrupted and the wrong one when the client has fallen
/// over -- a crash, a loading screen and a death all look the same to it.
///
/// What ends a run overnight is almost never the bot. It is the client:
/// it exits on its own (watched live -- left at the character select this one
/// closes after about two minutes), or it stops answering, or it puts up a
/// message box with a code in it and waits for somebody who is not there.
int run_keepalive(const Options& o, const AppConfig& app, int minutes) {
    std::string path, source;
    AutoLogin::Settings settings;
    TemplateStore store;
    if (!login_setup(o, app, settings, store, path, source)) return 1;

    IniFile ini;
    ini.load(resolve(o.app_path));
    CrashWatch::Settings watch_settings;
    watch_settings.load(ini);
    if (!settings.game_process.empty())
        watch_settings.game_process = settings.game_process;
    if (!settings.client_process.empty())
        watch_settings.launcher_process = settings.client_process;

    CrashWatch watch(watch_settings);

    std::printf("Keeping the client alive for %d minute(s).\n\n"
                "  watching     %s, and %s behind it\n"
                "  restarts     at most %d, then a person is needed\n"
                "  channel      %d        character %d\n\n",
                minutes, watch_settings.game_process.c_str(),
                watch_settings.launcher_process.c_str(),
                watch_settings.max_restarts,
                settings.channel_index + 1, settings.character_index + 1);

    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(minutes);
    int restarts = 0;
    bool said_fine = false;

    while (std::chrono::steady_clock::now() < until) {
        const CrashWatch::Look seen = watch.look();

        if (!seen.bad()) {
            if (!said_fine) {
                std::printf("  [%-22s] %s\n", "watching",
                            "the client is running and answering");
                std::fflush(stdout);
                said_fine = true;
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        said_fine = false;

        std::printf("\n  [%-22s] %s\n", CrashWatch::trouble_name(seen.what),
                    seen.detail.c_str());
        if (!seen.code.empty())
            std::printf("  [%-22s] %s\n", "error code", seen.code.c_str());
        std::fflush(stdout);

        if (restarts >= watch_settings.max_restarts) {
            std::fprintf(stderr,
                "\nThat is %d restart(s) and it has gone again. Stopping.\n\n"
                "A client that falls over this often is not failing at random, and\n"
                "restarting it again will not fix whatever it is -- nor should a\n"
                "program keep reconnecting to a server that is refusing it.\n",
                restarts);
            return 1;
        }

        watch.shut_down(seen);
        std::printf("  [%-22s] %ds\n", "cooling down", watch_settings.cooldown_s);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(watch_settings.cooldown_s));

        ++restarts;
        std::printf("  [%-22s] %d of %d\n\n", "logging back in", restarts,
                    watch_settings.max_restarts);
        std::fflush(stdout);

        AutoLogin flow;
        flow.set_settings(settings);
        flow.set_templates(&store);
        flow.on_step([](AutoLogin::Step s, const std::string& detail) {
            std::printf("  [%-22s] %s\n", AutoLogin::step_name(s), detail.c_str());
            std::fflush(stdout);
        });
        if (!flow.run()) {
            std::fprintf(stderr, "\nCould not get back in: %s\n",
                         flow.trouble().c_str());
            return 1;
        }
        std::printf("\n");
    }

    std::printf("\n%d minute(s) up. %d restart(s).\n", minutes, restarts);
    return 0;
}

/// Watch the session the way an unattended run would.
///
/// Prints the state as it changes and every drop it notices. Run it and then
/// go and do the things that break a bot overnight -- walk through a door, die,
/// open a full-screen dialog, log out, close the client -- and watch what it
/// makes of each. The ones that should be reported as a lost session are the
/// last two; everything before them should pass without a word, and if they do
/// not, the grace period is too short.
int run_session(const Options& o, const AppConfig& app, int seconds) {
    IniFile ini;
    ini.load(resolve(o.app_path));

    SessionManager::Settings settings;
    settings.load(ini);

    // Watching for a fixed time from the command line, not a scheduled run:
    // whatever the config says about stopping and shutting down belongs to the
    // real thing, and must not be acted on because somebody asked for a look.
    settings.stop_after_minutes = 0;
    settings.shutdown_when_done = false;

    SessionManager session;
    session.set_settings(settings);
    session.set_window_hints(o.window.empty() ? app.window_title : o.window,
                             o.window.empty() ? app.window_class : std::string{},
                             o.window.empty() ? app.process_name : std::string{});

    session.on_event([](SessionManager::Event e, const std::string& detail) {
        std::printf("  [%-17s] %s\n", SessionManager::event_name(e), detail.c_str());
        std::fflush(stdout);
    });

    std::printf("Watching for %d seconds, looking every %d ms.\n"
                "A session counts as lost after %d seconds with no HUD.\n\n",
                seconds, settings.poll_ms, settings.lost_grace_s);

    session.start();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const auto st = session.stats();
    const auto final_state = session.state();
    session.stop();

    std::printf("\n%llu looks, %llu captures failed.\n"
                "%llu seconds of them in the world, %llu sessions lost.\n"
                "Ended %s.\n",
                static_cast<unsigned long long>(st.looks),
                static_cast<unsigned long long>(st.captures_failed),
                static_cast<unsigned long long>(st.seconds_playing),
                static_cast<unsigned long long>(st.sessions_lost),
                SessionManager::state_name(final_state));
    return 0;
}

/// Say which map the character is standing in, and load its geometry.
///
/// The two halves of this were both already here and had never been
/// introduced. The game ships exact collision data for every map it has, and
/// the bot can read a map's name off the minimap's title bar. What was missing
/// was the index tying one to the other -- which the game also ships, in a
/// table nobody had looked in.
int run_whereami(const Options& o, const AppConfig& app) {
    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'. Is the game running?\n", title.c_str());
        return 1;
    }

    MapLocator locator;
    if (!locator.init()) {
        std::fprintf(stderr,
            "Need two things from the game's own install:\n"
            "  <game>\\zVietnam\\Chart\\tb_Map.tbl   the index of places\n"
            "  <game>\\data\\Map\\*.map              the collision data\n"
            "and a glyph set to read the map's name with.\n");
        return 1;
    }

    // The creature list, which is what settles a name that means two places.
    GameState state;
    std::unique_ptr<OpcodeRegistry> registry;
    std::unique_ptr<ProtocolHandler> handler;
    PacketCapture capture;

    bool framing_found = false;
    const FramingSpec framing = load_framing(resolve(app.protocol_path), framing_found);
    registry = std::make_unique<OpcodeRegistry>();
    if (framing_found && registry->load(resolve(app.protocol_path))) {
        handler = std::make_unique<ProtocolHandler>(*registry, state);
        capture.set_framing(framing);
        // The client frames its half differently; without this it is ground
        // into resync noise a byte at a time.
        bool cf_found = false;
        const FramingSpec cf =
            load_framing(resolve(app.protocol_path), cf_found, "FramingClient");
        if (cf_found) capture.set_client_framing(cf);
        capture.set_packet_callback([&](const std::vector<GamePacket>& packets) {
            for (const GamePacket& p : packets) handler->on_packet(p);
        });
        CaptureConfig cc;
        cc.interface_name = app.interface_name;
        cc.server_ip_start = app.server_ip_start;
        cc.server_ip_end = app.server_ip_end;
        cc.port_start = app.port_start;
        cc.port_end = app.port_end;
        if (capture.start(cc)) {
            std::printf("Listening for a moment, so the creature list can settle "
                        "an ambiguous name...\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        }
    }

    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }
    ScreenCapture cap;
    Frame frame;
    if (!cap.grab(input.target_window(), frame) || !frame.valid()) {
        std::fprintf(stderr, "Could not capture the window.\n");
        capture.stop();
        return 1;
    }

    const std::vector<Monster> world = state.monsters();
    const bool found = locator.identify(frame, world);
    capture.stop();

    std::printf("\ntitle bar reads \"%s\" (%.0f%% sure)\n",
                locator.last_read().c_str(), locator.last_confidence() * 100.0f);
    std::printf("%zu creatures on the wire\n\n", world.size());

    if (!locator.candidates().empty() && locator.candidates().size() > 1) {
        std::printf("that name belongs to %zu places:\n", locator.candidates().size());
        for (const MapTable::Entry& e : locator.candidates())
            std::printf("  %-10s %s\n", e.stem().c_str(), e.name.c_str());
        std::printf("\n");
    }

    if (!found || !locator.here()) {
        std::fprintf(stderr, "Could not tell which map this is.\n");
        return 1;
    }

    const MapTable::Entry& here = *locator.here();
    std::printf("you are in \"%s\"  ->  %s.map\n", here.name.c_str(), here.stem().c_str());

    if (!locator.has_geometry()) {
        std::fprintf(stderr, "but its collision file did not load.\n");
        return 1;
    }
    const MapFile& g = locator.geometry();
    std::printf("%d x %d cells, %d x %d px, %zu surface pixels\n",
                g.width_cells(), g.height_cells(), g.width_px(), g.height_px(),
                g.surface_count());

    // The creature list and the collision file describe the same place in the
    // same units, so saying so out loud is a check on both.
    if (!world.empty()) {
        float lo_x = 1e9f, hi_x = -1e9f, lo_y = 1e9f, hi_y = -1e9f;
        for (const Monster& m : world) {
            lo_x = std::min(lo_x, m.pos.x); hi_x = std::max(hi_x, m.pos.x);
            lo_y = std::min(lo_y, m.pos.y); hi_y = std::max(hi_y, m.pos.y);
        }
        std::printf("creatures occupy x %.0f-%.0f, y %.0f-%.0f -- %s\n",
                    lo_x, hi_x, lo_y, hi_y,
                    (hi_x <= g.width_px() && hi_y <= g.height_px())
                        ? "inside the map, as they should be"
                        : "OUTSIDE the map, so something is wrong");
    }
    return 0;
}

/// Watch the two halves of the world line up.
///
/// Packets say where every creature in the level is and nothing about where you
/// are. The screen shows creatures and your character together but in pixels.
/// This prints both, and the offset between them -- which is the camera, and
/// therefore the answer to where you are standing in the server's own numbers.
int run_anchor(const Options& o, const AppConfig& app, int seconds) {
    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'. Is the game running?\n", title.c_str());
        return 1;
    }

    bool framing_found = false;
    const FramingSpec framing = load_framing(resolve(app.protocol_path), framing_found);
    if (!framing_found) {
        std::fprintf(stderr, "No [Framing] in the protocol config.\n");
        return 1;
    }

    OpcodeRegistry registry;
    if (!registry.load(resolve(app.protocol_path))) {
        std::fprintf(stderr, "No packet layouts in the protocol config.\n");
        return 1;
    }
    GameState state;
    ProtocolHandler handler(registry, state);

    PacketCapture capture;
    capture.set_framing(framing);
    // The client frames its half differently; without this it is ground into
    // resync noise a byte at a time.
    bool client_framing_found = false;
    const FramingSpec client_framing =
        load_framing(resolve(app.protocol_path), client_framing_found, "FramingClient");
    if (client_framing_found) capture.set_client_framing(client_framing);
    capture.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        for (const GamePacket& p : packets) handler.on_packet(p);
    });

    CaptureConfig cc;
    cc.interface_name = app.interface_name;
    cc.server_ip_start = app.server_ip_start;
    cc.server_ip_end = app.server_ip_end;
    cc.port_start = app.port_start;
    cc.port_end = app.port_end;
    if (!capture.start(cc)) {
        std::fprintf(stderr, "Could not start capture. Is Npcap installed?\n");
        return 1;
    }

    GlyphFont font;
    const std::string font_dir = GlyphFont::default_dir();
    const bool have_font = !font_dir.empty() && font.load(font_dir);

    HunterConfig hunt;
    hunt.load(resolve("configs/hunt.ini"));

    WorldAnchor anchor;
    CameraTracker camera;
    camera.set_play_area(ScreenRect{ 100, 130, 620, 380 });

    // The view is tracked in a loop of its own, far faster than anything else
    // here runs.
    //
    // Tracking works by matching each frame against the one before it, so how
    // far the view can move between frames is set by how often frames are
    // taken. Sharing a loop with the detector and the reader meant a third of
    // a second between captures, by which time a walking character has carried
    // the view further than any sane search window -- and a frame that fails
    // to match is movement that is never counted, which quietly corrupts every
    // position measured against it afterwards. Capturing for the tracker alone
    // costs a screen grab and a few hundred comparisons.
    std::atomic<bool> watching{ true };
    std::thread eyes([&] {
        ScreenCapture own;
        Frame f;
        while (watching.load()) {
            if (own.grab(input.target_window(), f)) camera.update(f);
            std::this_thread::sleep_for(std::chrono::milliseconds(55));
        }
    });

    ScreenCapture cap;
    Frame frame;
    const DetectorTuning tuning = DetectorTuning::defaults();

    std::printf("\n%-5s %-6s %-16s %-16s %-10s %s\n",
                "wire", "screen", "camera is at", "evidence", "minimap",
                "your character");
    int ticks = 0;
    int breaks = 0;
    std::uint64_t lost_before = 0;
    const std::uint64_t until = now_ms() + static_cast<std::uint64_t>(seconds) * 1000;
    while (now_ms() < until && !g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!cap.grab(input.target_window(), frame) || !frame.valid()) continue;

        camera.update(frame);
        const auto view = camera.fix();

        const std::vector<Monster> world = state.monsters();
        const std::vector<Nameplate> seen =
            MobDetector::detect(frame, tuning, have_font ? &font : nullptr);

        // A break in the view's tracking throws the landmarks away.
        //
        // Every landmark is a screen position with the camera added back, so
        // they all mean something only while the camera's own count is
        // continuous. A frame the tracker could not match is movement it did
        // not see, and the count is wrong from then on by however far the view
        // went during it -- silently, and for good. Landmarks gathered before
        // such a gap and after it are measured from different origins and
        // cannot be matched against the same creature list, which is what had
        // been quietly ruining every run that walked briskly enough to lose
        // the lock.
        if (view.lost != lost_before) {
            anchor.reset();
            ++breaks;
        }
        lost_before = view.lost;

        if (view.tracking) anchor.observe(world, seen, view.x, view.y);
        const WorldAnchor::Fix fix = anchor.current();

        char at_text[32] = "-";
        if (fix.agreed > 0)
            std::snprintf(at_text, sizeof at_text, "%s%.0f,%.0f x%.2f",
                          fix.valid ? "" : "?", fix.offset_x, fix.offset_y, fix.scale);
        char evidence[32];
        std::snprintf(evidence, sizeof evidence, "%d/%d vs %.1f b%d",
                      fix.agreed, fix.considered, fix.chance, breaks);

        // And where that puts the character, found on screen by its own name.
        char me[32] = "-";
        if (fix.valid && have_font && !hunt.character_name.empty()) {
            ScreenRect spot{};
            if (font.find_text(frame, ScreenRect{ 0, 0, frame.width, frame.height },
                               hunt.character_name, spot, nullptr)) {
                Vec2 w;
                if (anchor.to_world(spot.cx(), spot.y, view.x, view.y, w))
                    std::snprintf(me, sizeof me, "%.0f,%.0f", w.x, w.y);
            }
        }

        // And where the game itself says the character is. The one statement
        // of our own position that this game makes anywhere: the marker it
        // draws on the minimap for the player to look at.
        char marker[24] = "-";
        const auto layout = MinimapTracker::default_layout(frame.width, frame.height);
        const auto spot = MinimapTracker::find_marker(frame, layout.body);
        if (spot.valid)
            std::snprintf(marker, sizeof marker, "%.1f,%.1f", spot.x, spot.y);

        std::printf("%-5zu %-6zu %-16s %-16s %-10s %s\n",
                    world.size(), seen.size(), at_text, evidence, marker, me);

        // Once in a while, both lists side by side. If the spacing between
        // creatures on screen does not match the spacing between creatures on
        // the wire, no offset will ever line them up and the fault is the
        // scale, not the search.
        if (o.verbose && ++ticks % 8 == 0) {
            std::printf("    landmarks:");
            for (const auto& r : anchor.rivals(6))
                std::printf("  [%.0f,%.0f seen%d wander%.0f]", r.x, r.y, r.weight, r.wander);
            std::printf("\n    on screen:");
            for (const Nameplate& n : seen)
                if (!n.is_player) std::printf("  %d,%d", n.x(), n.y());
            std::printf("\n    on the wire (within a screen of the view):");
            for (const Monster& m : world) {
                const double sx = m.pos.x - view.x;
                if (sx < -600.0 || sx > 1600.0) continue;
                std::printf("  %.0f,%.0f", m.pos.x, m.pos.y);
            }
            std::printf("\n");
        }
        std::fflush(stdout);
    }

    capture.stop();
    return 0;
}

/// Re-run the analysis over a capture taken earlier.
int run_analyse(const std::string& path, const std::string& dump,
                const std::string& check, bool fields) {
    FieldFinder finder;
    if (!finder.load(path)) {
        std::fprintf(stderr, "Could not read '%s'.\n", path.c_str());
        return 1;
    }
    const auto fs = finder.stats();
    std::printf("%llu samples from %s\n\n",
                static_cast<unsigned long long>(fs.packets), path.c_str());

    if (fields) {
        // The map's size, taken from the one packet already understood.
        std::printf("Everywhere in the protocol that looks like a position.\n"
                    "A float in the right range with another beside it, varying,\n"
                    "in most frames of that opcode.\n\n");
        std::printf("%-8s %-5s %-9s %8s %14s %14s %7s\n",
                    "opcode", "dir", "at", "samples", "x range", "y range", "in range");
        for (const auto& f : finder.position_fields(8000.0, 2000.0)) {
            char at[16], xr[20], yr[20];
            std::snprintf(at, sizeof at, "+%zu,+%zu", f.x_offset, f.y_offset);
            std::snprintf(xr, sizeof xr, "%.0f-%.0f", f.x_min, f.x_max);
            std::snprintf(yr, sizeof yr, "%.0f-%.0f", f.y_min, f.y_max);
            std::printf("0x%04X   %-5s %-9s %8zu %14s %14s %6.0f%%\n",
                        f.opcode, f.direction == Direction::ToClient ? "S->C" : "C->S",
                        at, f.samples, xr, yr, f.in_range_pct);
        }
        return 0;
    }

    if (!check.empty()) {
        // opcode:id_off:x_off:y_off[:width]
        unsigned op = 0, idoff = 0, xoff = 0, yoff = 0, width = 2;
        const int got = std::sscanf(check.c_str(), "%i:%u:%u:%u:%u",
                                    &op, &idoff, &xoff, &yoff, &width);
        if (got < 4) {
            std::fprintf(stderr, "Expected --check opcode:id_off:x_off:y_off[:width]\n");
            return 2;
        }
        const bool from_server = check.find("/c") == std::string::npos;
        const bool is_float = check.find('f') != std::string::npos;
        std::printf("0x%04X %s, grouped by u32le@%u, x=%c%u@%u, y=%c%u@%u\n\n",
                    op, from_server ? "S->C" : "C->S", idoff,
                    is_float ? 'f' : 'u', width * 8, xoff,
                    is_float ? 'f' : 'u', width * 8, yoff);
        std::printf("%8s %8s %8s %11s %13s %8s %11s %13s\n",
                    "group", "samples", "x corr", "x units/px", "x range",
                    "y corr", "y units/px", "y range");
        for (const auto& c : finder.check_layout(static_cast<std::uint16_t>(op),
                                                 from_server, idoff, xoff, yoff,
                                                 static_cast<int>(width), is_float)) {
            char xr[32], yr[32];
            std::snprintf(xr, sizeof xr, "%.0f-%.0f", c.x_min, c.x_max);
            std::snprintf(yr, sizeof yr, "%.0f-%.0f", c.y_min, c.y_max);
            std::printf("%8u %8zu %8.3f %11.3f %13s %8.3f %11.3f %13s\n",
                        c.id, c.samples, c.x_corr, c.x_scale, xr,
                        c.y_corr, c.y_scale, yr);
        }
        return 0;
    }

    if (!dump.empty()) {
        // "0x0038", or "0x0038/c" for the client's half -- the same number
        // means different things in each direction. The separator is not
        // decoration: 'c' is also a hex digit, and 0x00C6c parses as 0x0C6C.
        const bool from_server = dump.find("/c") == std::string::npos;
        const auto opcode = static_cast<std::uint16_t>(
            std::strtoul(dump.c_str(), nullptr, 0));
        dump_opcode(finder, opcode, from_server);
        return 0;
    }

    report_findings(finder);
    return 0;
}

/// Run the vision bot from a console: watch, hunt, and report.
///
/// The same pieces the Radar tab drives, without a window of our own to get in
/// the way of the screen capture. Useful for checking that the whole chain
/// works end to end — eyes, decision, keys — and for running the thing on a
/// machine where the GUI would only be sitting on top of the game.
int run_hunt(const Options& o, const AppConfig& app, int seconds) {
    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'. Is the game running?\n", title.c_str());
        return 1;
    }

    if (!InputManager::injection_works()) {
        std::fprintf(stderr,
            "Keys sent from here will not reach the game: it is running with\n"
            "higher privileges than this process. Run as Administrator.\n");
        return 1;
    }
    if (!input.focus_window()) {
        std::fprintf(stderr, "Could not bring the game to the front.\n");
        return 1;
    }

    ExecutionCoordinator coord;

    // Declared before the detector so that it is destroyed after it. The
    // detector reads the font from its own thread, and that thread is not
    // stopped until the detector is torn down.
    GlyphFont font;
    const std::string font_dir = GlyphFont::default_dir();
    const bool have_font = !font_dir.empty() && font.load(font_dir);

    MobDetector detector;
    CameraTracker camera;
    MinimapTracker minimap;

    int w = 0, h = 0;
    input.window_size(w, h);
    minimap.set_layout(MinimapTracker::default_layout(w, h));
    camera.set_play_area(ScreenRect{ 100, 130, 620, 380 });

    detector.set_target_window(input.target_window());
    detector.set_tuning(DetectorTuning::defaults());
    detector.set_minimap(&minimap);
    detector.set_camera(&camera);

    HunterConfig cfg;
    cfg.load(resolve("configs/hunt.ini"));

    if (have_font) {
        detector.set_font(&font);
        std::printf("Reading name plates with %zu glyphs of the game's font.\n",
                    font.size());
    }
    if (!cfg.character_name.empty()) {
        detector.set_character_name(cfg.character_name);
        if (!have_font)
            std::printf("No glyph set installed, so \"%s\" cannot be looked for -- "
                        "the bot will not be able to see your character.\n",
                        cfg.character_name.c_str());
    } else {
        std::printf("No character_name in configs/hunt.ini. The bot can see the "
                    "monsters but not itself; set it and it can walk to them.\n");
    }
    detector.start();

    Hunter hunter(input, coord, detector);
    cfg.focus_on_start = false;         // already done, and it is ours to keep
    hunter.set_config(cfg);
    hunter.start();

    std::printf("Hunting for %d seconds with %s. Ctrl+C to stop early.\n\n",
                seconds, InputManager::key_name(cfg.attack_key).c_str());

    const std::uint64_t until = now_ms() + static_cast<std::uint64_t>(seconds) * 1000;
    while (now_ms() < until && !g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Keep the game in front. The GUI deliberately does not do this -- if
        // you alt-tab away it should wait, not fight you for the desktop --
        // but a headless run was started for no other purpose, and whatever
        // stole the foreground (this console, as often as not) is in the way.
        if (!input.window_focused()) input.focus_window();

        const auto ds = detector.stats();
        const auto hs = hunter.status();
        // Saturating: the clock can pass the deadline between the loop's test
        // and this line, and an unsigned subtraction that goes negative prints
        // as half a billion years remaining.
        const std::uint64_t t_now = now_ms();
        std::printf("\r  %llus | %zu monsters | %-28s | %llu attacks, %llu steps, %llu dropped   ",
                    static_cast<unsigned long long>(t_now >= until ? 0 : (until - t_now) / 1000),
                    ds.last_mobs, Hunter::to_string(hs.state),
                    static_cast<unsigned long long>(hs.attacks),
                    static_cast<unsigned long long>(hs.steps),
                    static_cast<unsigned long long>(hs.targets_dropped));
        std::fflush(stdout);
    }

    hunter.stop();
    detector.stop();
    input.release_all();

    const auto hs = hunter.status();
    const auto ds = detector.stats();
    std::printf("\n\n%llu frames looked at, character seen: %s\n",
                static_cast<unsigned long long>(ds.frames),
                ds.player_seen ? "yes" : "no");
    std::printf("%llu attacks thrown, %llu walking steps, %llu targets given up on\n",
                static_cast<unsigned long long>(hs.attacks),
                static_cast<unsigned long long>(hs.steps),
                static_cast<unsigned long long>(hs.targets_dropped));
    std::printf("the view moved %.0f px while it worked\n", camera.travelled());
    return 0;
}

/// Print a patch of the frame as characters, so what the reader is looking at
/// can be seen rather than guessed at.
///
/// Two pictures of the same pixels: what counts as ink, and how bright they
/// are. Together they say whether a region holds light text on a dark ground,
/// dark text on a light one, or no text at all — which is the difference
/// between a matcher that needs fixing and a rectangle that is in the wrong
/// place.
void print_region(const Frame& f, const ScreenRect& r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.right()), y1 = std::min(f.height, r.bottom());
    if (x1 <= x0 || y1 <= y0) { std::printf("(region is off the frame)\n"); return; }

    std::printf("brightness ('#' bright enough to be a stroke, digits below that):\n");
    for (int y = y0; y < y1; ++y) {
        std::printf("  ");
        for (int x = x0; x < x1; ++x) {
            std::uint8_t pr, pg, pb;
            f.get(x, y, pr, pg, pb);
            const int v = GlyphFont::level(pr, pg, pb);
            std::putchar(v >= GlyphFont::kMinInkLevel
                         ? '#'
                         : static_cast<char>('0' + std::min(9, v * 10 / 256)));
        }
        std::putchar('\n');
    }

    // The same patch as a picture, blown up so a six-pixel-tall glyph can
    // actually be looked at: the crop on top, what the reader thinks is ink
    // below it. Characters on a terminal cannot show this honestly; at this
    // size one wrong row is the difference between 'e' and 'c'.
    const int w = x1 - x0, h = y1 - y0;
    const int zoom = std::max(1, std::min(12, 480 / std::max(1, w)));
    const int gutter = 4;

    Frame out;
    out.width  = w * zoom;
    out.height = h * zoom * 2 + gutter;
    out.pixels.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);

    const auto put = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const std::size_t i = (static_cast<std::size_t>(y) * out.width + x) * 4;
        out.pixels[i] = b; out.pixels[i + 1] = g; out.pixels[i + 2] = r;
        out.pixels[i + 3] = 255;
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t pr, pg, pb;
            f.get(x0 + x, y0 + y, pr, pg, pb);
            const bool ink = GlyphFont::level(pr, pg, pb) >= GlyphFont::kMinInkLevel;
            for (int dy = 0; dy < zoom; ++dy) {
                for (int dx = 0; dx < zoom; ++dx) {
                    put(x * zoom + dx, y * zoom + dy, pr, pg, pb);
                    const std::uint8_t v = ink ? 255 : 0;
                    put(x * zoom + dx, h * zoom + gutter + y * zoom + dy, v, v, v);
                }
            }
        }
    }

    const std::string path = exe_relative("region.png");
    if (save_png(out, path))
        std::printf("region written to %s  (crop above, ink below, %dx)\n",
                    path.c_str(), zoom);
}

/// Read the game's own text off the screen.
///
/// With no rectangle given, reads the places worth reading: the minimap's
/// title bar, which names the map, and every name plate the detector found.
/// Read back text drawn with the glyphs themselves.
///
/// No window, no game, no login: if a string built out of the font's own
/// pictures does not come back, the reader is wrong and nothing about the
/// screen explains it.
int run_readself(const std::string& text) {
    GlyphFont font;
    const std::string dir = GlyphFont::default_dir();
    if (dir.empty() || !font.load(dir)) {
        std::fprintf(stderr, "No glyph set found.\n");
        return 1;
    }

    const std::vector<std::string> samples =
        text.empty() ? std::vector<std::string>{ "ThanhAmCoc", "KhiCon", "abcmno",
                                                 "0123456789", "Thanh Am Coc",
                                                 "ThanhÂmCốc" }
                     : std::vector<std::string>{ text };

    int bad = 0;
    for (const std::string& want : samples) {
        Frame f;
        if (!font.render_line(want, f)) {
            std::printf("  (no glyph for some letter of \"%s\")\n", want.c_str());
            continue;
        }
        const auto started = std::chrono::steady_clock::now();
        std::string got;
        constexpr int kRuns = 20;
        for (int i = 0; i < kRuns; ++i)
            got = font.read_line(f, ScreenRect{ 0, 0, f.width, f.height });
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started).count() / kRuns;

        const bool ok = (got == want);
        if (!ok) ++bad;
        std::printf("%s  \"%s\"\n      -> \"%s\"  (%.0f%%, %dx%d in %.1f ms)\n",
                    ok ? "ok  " : "WRONG", want.c_str(), got.c_str(),
                    font.last_confidence() * 100.0f, f.width, f.height, ms);
    }
    return bad == 0 ? 0 : 1;
}

int run_read(const Options& o, const AppConfig& app, const std::string& rect,
             const std::string& find_me) {
    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'.\n", title.c_str());
        return 1;
    }

    GlyphFont font;
    const std::string dir = GlyphFont::default_dir();
    if (dir.empty() || !font.load(dir)) {
        std::fprintf(stderr,
            "No glyph set found. This reads the game's text by matching the\n"
            "pictures its font is made of, and needs a directory of them --\n"
            "normally the one the commercial client extracted, at\n"
            "  C:\\Program Files\\ZTeam\\GoC\\data\\bitmap_font\n");
        return 1;
    }
    std::printf("%zu glyphs loaded from %s\n", font.size(), dir.c_str());

    // The game will not render itself into a bitmap on request, so frames come
    // off the screen -- which means anything covering the game is what gets
    // read. Raise it first, or the reader is handed a picture of whatever
    // window happens to be on top.
    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }

    ScreenCapture cap;
    Frame frame;
    if (!cap.grab(input.target_window(), frame) || !frame.valid()) {
        std::fprintf(stderr, "Could not capture the window.\n");
        return 1;
    }
    std::printf("captured %dx%d%s, game %s\n", frame.width, frame.height,
                cap.from_screen() ? " off the screen" : "",
                input.window_focused() ? "in front" : "NOT IN FRONT -- the picture "
                                                      "is of whatever is covering it");

    // The frame as the reader received it, for when the two disagree.
    const std::string shot = exe_relative("frame.png");
    if (save_png(frame, shot)) std::printf("frame written to %s\n", shot.c_str());
    std::printf("\n");

    if (!find_me.empty()) {
        const auto started = std::chrono::steady_clock::now();
        ScreenRect at{};
        float sure = 0.0f;
        const bool got = font.find_text(
            frame, ScreenRect{ 0, 0, frame.width, frame.height }, find_me, at, &sure);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started).count();
        if (got)
            std::printf("\"%s\" found at %d,%d %dx%d  (%.0f%% sure, %.0f ms to "
                        "search the whole frame)\n",
                        find_me.c_str(), at.x, at.y, at.w, at.h, sure * 100.0f, ms);
        else
            std::printf("\"%s\" is not on screen  (%.0f ms)\n", find_me.c_str(), ms);
        return got ? 0 : 1;
    }

    if (!rect.empty()) {
        int x = 0, y = 0, w = 0, h = 0;
        if (std::sscanf(rect.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) != 4) {
            std::fprintf(stderr, "Expected --read x,y,w,h\n");
            return 2;
        }
        const ScreenRect r{ x, y, w, h };
        print_region(frame, r);
        const auto t0 = std::chrono::steady_clock::now();
        const std::string text = font.read_line(frame, r);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::printf("\n%d,%d %dx%d -> \"%s\"   (%.0f%% confident, %.1f ms)\n",
                    x, y, w, h, text.c_str(), font.last_confidence() * 100.0f, ms);

        std::printf("\n%-6s %-10s %-10s %s\n", "char", "at", "size", "fit");
        for (const GlyphFont::Placed& p : font.last_placed()) {
            char at[16], size[16];
            std::snprintf(at, sizeof at, "%d,%d", p.box.x, p.box.y);
            std::snprintf(size, sizeof size, "%dx%d", p.box.w, p.box.h);
            std::printf("%-6s %-10s %-10s %.3f\n", p.ch.c_str(), at, size, p.quality);
        }

        // And what else could have started at the left edge. When a letter
        // comes back wrong this is usually the whole explanation.
        std::printf("\ncandidates starting at x=%d:\n", r.x);
        for (const GlyphFont::Placed& c : font.candidates(frame, r, 10))
            std::printf("  %-6s y=%-5d %dx%-5d %.3f\n",
                        c.ch.c_str(), c.box.y, c.box.w, c.box.h, c.quality);
        return 0;
    }

    // The map's name, off the minimap's title bar.
    const auto layout = MinimapTracker::default_layout(frame.width, frame.height);
    if (layout.title.valid()) {
        const std::string name = font.read_line(frame, layout.title);
        std::printf("map name at %d,%d %dx%d -> \"%s\"   (%.0f%%)\n",
                    layout.title.x, layout.title.y, layout.title.w, layout.title.h,
                    name.c_str(), font.last_confidence() * 100.0f);
    }

    // And every name plate on screen, read rather than guessed at.
    std::printf("\n%-8s %-20s %-6s %s\n", "what", "plate", "hp", "name");
    for (const Nameplate& n :
         MobDetector::detect(frame, DetectorTuning::defaults(), &font)) {
        char where[32], hp[8];
        std::snprintf(where, sizeof where, "%d,%d %dx%d",
                      n.plate.x, n.plate.y, n.plate.w, n.plate.h);
        std::snprintf(hp, sizeof hp, "%.0f%%", n.hp * 100.0f);
        std::printf("%-8s %-20s %-6s \"%s\"  (%.0f%%)\n",
                    n.is_player ? "player" : "monster", where, hp,
                    n.name.c_str(), n.name_confidence * 100.0f);
    }
    return 0;
}

/// Why synthetic keystrokes are or are not getting through.
///
/// Windows refuses input injected by a process running at a lower integrity
/// level than the window that currently has the foreground. A game started
/// from an elevated shell therefore ignores a bot started from a normal one,
/// silently, with SendInput still reporting success — which looks exactly like
/// an anticheat blocking macros and is not.
///
/// Telling the two apart is a matter of where the probe is aimed: if injection
/// works with an ordinary window in front and stops working the moment the
/// game is in front, that is the integrity rule and not the anticheat.
int run_inputcheck(const Options& o, const AppConfig& app) {
    std::printf("Are we elevated?            %s\n\n",
                running_as_admin() ? "yes" : "no");

    std::printf("Injecting with whatever is in front now... ");
    std::fflush(stdout);
    const bool before = InputManager::injection_works();
    std::printf("%s\n", before ? "works" : "blocked");

    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::printf("\nGame window not found, so that is as far as this goes.\n");
        return before ? 0 : 1;
    }
    if (!input.focus_window()) {
        std::printf("\nCould not bring the game to the front.\n");
        return 1;
    }

    std::printf("Injecting with the game in front...        ");
    std::fflush(stdout);
    const bool after = InputManager::injection_works();
    std::printf("%s\n\n", after ? "works" : "blocked");

    if (after) {
        std::printf("Input is getting through. Nothing to fix here.\n");
        return 0;
    }
    if (before && !after) {
        std::printf("This is Windows' integrity rule, not the game's anticheat.\n"
                    "The game is running at a higher integrity level than GoBot,\n"
                    "and Windows will not let a lower one inject input while a\n"
                    "higher one has the foreground.\n\n"
                    "Run GoBot as Administrator and it will work.\n");
        return 1;
    }
    std::printf("Injection is blocked even with an ordinary window in front, so\n"
                "something is filtering it system-wide rather than protecting the\n"
                "game specifically.\n");
    return 1;
}

/// Hold one key for a while. For checking by eye, or against a screenshot,
/// whether synthetic input reaches the game at all.
int run_press(const Options& o, const AppConfig& app, const std::string& key, int ms) {
    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'.\n", title.c_str());
        return 1;
    }
    const std::uint32_t vk = InputManager::parse_key(key);
    if (vk == 0) { std::fprintf(stderr, "Cannot parse key '%s'.\n", key.c_str()); return 2; }

    if (!input.focus_window()) {
        std::fprintf(stderr, "Could not bring the game to the front.\n");
        return 1;
    }
    std::printf("holding %s for %d ms\n", InputManager::key_name(vk).c_str(), ms);
    input.key_press(vk, ms);
    input.release_all();
    return 0;
}

/// Click one point of the game window, in client coordinates.
///
/// For measuring. Working out where a screen's controls are means pressing one
/// and seeing what changes, and doing that by hand is both slow and imprecise
/// -- the thing you want to know is what a click at exactly 893,492 does, not
/// what a click somewhere near there does.
int run_click(const Options& o, const AppConfig& app, const std::string& spec) {
    int x = 0, y = 0;
    if (std::sscanf(spec.c_str(), "%d,%d", &x, &y) != 2) {
        std::fprintf(stderr, "Expected --click x,y\n");
        return 2;
    }

    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'.\n", title.c_str());
        return 1;
    }
    if (!input.focus_window()) {
        std::fprintf(stderr, "Could not bring the window to the front.\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    int w = 0, h = 0;
    input.window_size(w, h);
    if (x < 0 || y < 0 || (w > 0 && x >= w) || (h > 0 && y >= h)) {
        std::fprintf(stderr, "%d,%d is outside the %dx%d client.\n", x, y, w, h);
        return 2;
    }

    input.click_at(x, y);
    std::printf("clicked %d,%d\n", x, y);
    return 0;
}

/// Find out which key actually walks the character.
///
/// Worth asking rather than assuming. This game reads the keyboard through
/// DirectInput and keeps its bindings in its own file as scan codes, so the
/// arrow keys are a guess and not a given — and a bot pressing the wrong key
/// looks exactly like a bot whose input does not work at all.
int run_keytest(const Options& o, const AppConfig& app) {
    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'.\n", title.c_str());
        return 1;
    }
    if (!input.focus_window()) {
        std::fprintf(stderr, "Could not bring the game to the front.\n");
        return 1;
    }

    CameraTracker camera;
    camera.set_play_area(ScreenRect{ 100, 130, 620, 380 });
    ScreenCapture cap;
    Frame frame;
    const DetectorTuning tuning = DetectorTuning::defaults();

    // Where the character is, in level coordinates. Screen position alone will
    // not do: when the view scrolls with the character its place on screen
    // barely changes.
    const auto where = [&](double& wx, double& wy) -> bool {
        if (!cap.grab(input.target_window(), frame)) return false;
        camera.update(frame);
        Nameplate me;
        const std::vector<Nameplate> plates = MobDetector::detect(frame, tuning);
        if (!MobDetector::pick_player(plates, frame.width, me)) return false;
        camera.to_world(me.x(), me.y(), wx, wy);
        return true;
    };

    const struct { const char* name; std::uint32_t vk; } kKeys[] = {
        { "Left arrow",  0x25 }, { "Right arrow", 0x27 },
        { "A",           'A'  }, { "D",           'D'  },
        { "Numpad 4",    0x64 }, { "Numpad 6",    0x66 },
        { "Space",       0x20 }, { "Up arrow",    0x26 },
        { "Z",           'Z'  }, { "LCtrl",       0x11 },
    };

    // Before blaming the game: does an injected keystroke reach anything at
    // all? This process injects one and immediately asks Windows whether that
    // key is down. If its own key does not come back, the input never got as
    // far as the system -- which is a different problem from the game ignoring
    // it, and one no choice of key will fix.
    {
        GetAsyncKeyState(VK_F7);                // clear the sticky bit
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_F7;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_F7, MAPVK_VK_TO_VSC));
        SendInput(1, &in, sizeof(INPUT));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const bool seen = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));

        std::printf("injected keystrokes reach this machine: %s\n\n",
                    seen ? "yes" : "NO -- something is filtering them");
    }

    std::printf("Holding each key and watching the character the whole time.\n"
                "Sampled during the press, not after it -- a jump comes back down.\n"
                "Leave the game in front.\n\n%-14s %8s %8s\n", "key", "max dx", "max dy");

    for (const auto& k : kKeys) {
        // Let the view settle, and establish where we are starting from.
        double x0 = 0.0, y0 = 0.0;
        bool have = false;
        for (int i = 0; i < 5 && !have; ++i) {
            have = where(x0, y0);
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
        }
        if (!have) { std::printf("%-14s %8s\n", k.name, "no fix"); continue; }

        double max_dx = 0.0, max_dy = 0.0;
        input.key_down(k.vk);
        for (int i = 0; i < 16; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
            double x = 0.0, y = 0.0;
            if (!where(x, y)) continue;
            max_dx = std::max(max_dx, std::abs(x - x0));
            max_dy = std::max(max_dy, std::abs(y - y0));
        }
        input.key_up(k.vk);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        std::printf("%-14s %8.0f %8.0f %s\n", k.name, max_dx, max_dy,
                    (max_dx > 40.0 || max_dy > 40.0) ? " <-- does something" : "");
    }

    input.release_all();
    std::printf("\nIf nothing moved at all, the game is not accepting synthetic\n"
                "keystrokes -- which is a different problem from the wrong key.\n");
    return 0;
}

/// Work out which bytes of which packet carry the character's position.
///
/// Everything needed is already here: the screen says where the character is,
/// and the network says what the server thinks. Walking the character back and
/// forth makes both move together, and the field that moves in step with the
/// measurement is the one we are looking for.
///
/// Nothing is guessed at. Every byte offset in every packet is tried as a
/// number, four ways each, and the correlation decides.
int run_calibrate(const Options& o, const AppConfig& app, int seconds) {
    InputManager input;
    const std::string title = o.window.empty() ? app.window_title : o.window;
    if (!attach_window(input, o, app)) {
        std::fprintf(stderr, "No window matching '%s'. Is the game running?\n", title.c_str());
        return 1;
    }
    if (!input.focus_window()) {
        std::fprintf(stderr, "Could not bring the game to the front. Click it and retry.\n");
        return 1;
    }

    bool framing_found = false;
    const FramingSpec framing = load_framing(resolve(app.protocol_path), framing_found);
    if (!framing_found) {
        std::fprintf(stderr, "No [Framing] in the protocol config -- nothing to decode with.\n");
        return 1;
    }

    // The client frames its half of the conversation differently, and its half
    // is the one that carries this character's own position: the server tells
    // you about everyone else, not about you.
    bool client_found = false;
    const FramingSpec client_framing =
        load_framing(resolve(app.protocol_path), client_found, "FramingClient");

    FieldFinder finder;
    MobDetector detector;
    CameraTracker camera;
    camera.set_play_area(ScreenRect{ 100, 130, 620, 380 });

    MacroConfig macro;
    macro.load(resolve("configs/macro.ini"));

    PacketCapture capture;
    capture.set_framing(framing);
    // The client frames its half differently, and its half is the one that
    // carries this character's own position.
    if (client_found) capture.set_client_framing(client_framing);
    capture.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        for (const GamePacket& p : packets) finder.on_packet(p);
    });

    CaptureConfig cc;
    cc.interface_name = app.interface_name;
    cc.server_ip_start = app.server_ip_start;
    cc.server_ip_end = app.server_ip_end;
    cc.port_start = app.port_start;
    cc.port_end = app.port_end;
    if (!o.host.empty()) { cc.server_ip_start = o.host; cc.server_ip_end = o.host; }
    if (o.port != 0) { cc.port_start = o.port; cc.port_end = o.port; }

    if (!capture.start(cc)) {
        std::fprintf(stderr, "Could not start capture. Is Npcap installed?\n");
        return 1;
    }

    // Walk the character if we are allowed to, and ask the user to if we are
    // not. Windows refuses injected input from a process less privileged than
    // the window in front, so an unelevated run against an elevated game can
    // watch but not drive.
    const bool can_drive = InputManager::injection_works();
    if (can_drive) {
        std::printf("Walking the character back and forth for %d seconds.\n"
                    "Leave the game in front and do not touch the keyboard.\n\n", seconds);
    } else {
        std::printf("Cannot send keys to the game -- it is running with higher\n"
                    "privileges than this. Run as Administrator to have GoBot walk\n"
                    "for you.\n\n"
                    "For now: WALK YOUR CHARACTER AROUND for the next %d seconds.\n"
                    "Left and right, as much ground as you can cover.\n\n", seconds);
    }

    // The eyes: one capture thread feeding the detector and the view tracker.
    std::atomic<bool> watching{true};
    std::thread eyes([&] {
        ScreenCapture cap;
        Frame frame;
        while (watching.load()) {
            if (cap.grab(input.target_window(), frame)) {
                camera.update(frame);

                // Where the character is, taken from where the view is.
                //
                // Not from finding the character on screen. That was the
                // mistake that made every earlier run useless: the local
                // character wears no health bar -- its health is in the HUD --
                // so the detector finds it almost never, and a measurement
                // that only arrives almost never pairs with almost no packets.
                // Two hundred samples came out of a run that saw tens of
                // thousands of frames, and nothing correlated.
                //
                // The camera follows the character. Walking right scrolls the
                // view right by exactly as much, so the view's own position is
                // the character's position give or take a constant -- and a
                // constant is invisible to a correlation. It is available on
                // every single frame, needs nothing to be visible, and is
                // measured by whole-pixel scroll matching that does not drift.
                const auto fx = camera.fix();
                const float hp = VitalsReader::measure(frame, macro.hp_bar);
                const float mp = VitalsReader::measure(frame, macro.mp_bar);

                if (fx.tracking) {
                    double lo = g_walk_lo.load(), hi = g_walk_hi.load();
                    if (lo > 1e17) { g_walk_lo.store(fx.x); g_walk_hi.store(fx.x); }
                    else {
                        if (fx.x < lo) g_walk_lo.store(fx.x);
                        if (fx.x > hi) g_walk_hi.store(fx.x);
                        g_walk_span.store(g_walk_hi.load() - g_walk_lo.load());
                    }
                    finder.set_truth(fx.x, fx.y, hp, mp, true);
                } else {
                    finder.set_truth(0, 0, hp, mp, false);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
    });

    // The legs.
    //
    // Long sweeps rather than short shuffles. A second of walking is nothing on
    // a map this size -- and if the character happens to be standing on a ledge
    // it is not even enough to leave it, which reads as "the keys did not
    // work" when the keys worked fine.
    // Two phases, because one kind of behaviour cannot reveal everything.
    //
    // Walking tells you which field is a position and nothing about which is
    // health, because health does not change while you walk. Standing in front
    // of something that hits you tells you which field is health and nothing
    // about position. A run that does both, in that order, gets both -- and
    // the analysis already refuses to report a quantity that never varied, so
    // the half that learns nothing stays quiet instead of inventing something.
    const std::uint64_t started = now_ms();
    const std::uint64_t until = started + static_cast<std::uint64_t>(seconds) * 1000;
    const std::uint64_t walk_until = started +
        static_cast<std::uint64_t>(seconds) * 1000 * g_walk_share_pct / 100;

    HunterConfig hunt;
    hunt.load(resolve("configs/hunt.ini"));

    int leg = 0;
    bool announced_fight = false;
    while (now_ms() < until && !g_stop.load()) {
        if (!can_drive) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else if (now_ms() < walk_until) {
            // Walk out, stand, walk back, stand. The standing matters as much
            // as the walking: a field that keeps changing while the character
            // is still is a clock or a sequence number. Walking back to where
            // it started makes the same point from the other side -- a real
            // coordinate comes home to the value it left.
            //
            // Long legs. The span the character covers is the whole dynamic
            // range of the measurement, and a correlation over two hundred
            // pixels is a far weaker claim than one over a thousand.
            if (!input.window_focused()) input.focus_window();
            switch (leg % 4) {
                case 0: input.key_press(input.right_key(), 6000); break;
                case 2: input.key_press(input.left_key(), 6000); break;
                default:
                    // Jump, then swing, without moving on.
                    //
                    // Jumping is the only thing that gives a y field anything
                    // to track, since flat ground scrolls the view sideways
                    // only. Attacking here rather than in a phase of its own
                    // is what makes a combat packet's position mean something:
                    // keeping all the fighting until the end produced attacks
                    // from one spot, to be correlated against a measurement
                    // that never moved, which can say nothing about anything.
                    for (int j = 0; j < 3; ++j) {
                        input.key_press(hunt.jump_key, 90);
                        std::this_thread::sleep_for(std::chrono::milliseconds(650));
                    }
                    for (int swing = 0; swing < 7 && now_ms() < until; ++swing) {
                        input.key_press(hunt.attack_key, 60);
                        std::this_thread::sleep_for(std::chrono::milliseconds(360));
                    }
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ++leg;
        } else {
            if (!announced_fight) {
                announced_fight = true;
                std::printf("\n\nNow attacking in place, to find health and the "
                            "monster packets.\n");
            }
            // Stand and fight. Health moves both ways -- down from being hit,
            // up from the heal keys the macro is already pressing -- and
            // monsters appear and disappear, which is what names the spawn and
            // remove packets.
            if (!input.window_focused()) input.focus_window();
            input.key_press(hunt.attack_key, 60);
            std::this_thread::sleep_for(std::chrono::milliseconds(450));
        }

        const auto fs = finder.stats();
        const std::uint64_t t_now = now_ms();
        std::printf("\r  %llus left | %llu packets, %llu paired | covered %.0f px   ",
                    static_cast<unsigned long long>(t_now >= until ? 0 : (until - t_now) / 1000),
                    static_cast<unsigned long long>(fs.packets),
                    static_cast<unsigned long long>(fs.paired), g_walk_span.load());
        std::fflush(stdout);
    }
    input.release_all();

    watching.store(false);
    eyes.join();
    capture.stop();
    input.release_all();

    const auto fs = finder.stats();
    std::printf("\n\n%llu packets seen, %llu kept with a measurement alongside them\n",
                static_cast<unsigned long long>(fs.packets),
                static_cast<unsigned long long>(fs.paired));

    const auto cf = camera.fix();
    std::printf("the view moved %.0f px; the character covered %.0f px "
                "(%llu frames matched, %llu lost)\n\n",
                camera.travelled(), g_walk_span.load(),
                static_cast<unsigned long long>(cf.updates),
                static_cast<unsigned long long>(cf.lost));

    // What matters is how far the character went, not how far the view did:
    // inside the camera's dead zone, and at the edges of a map, the character
    // moves across a view that stays put.
    if (g_walk_span.load() < 60.0) {
        std::printf("The character barely moved, so there is nothing to correlate\n"
                    "against. Run it again and walk around while it watches.\n");
        return 1;
    }

    // Keep the capture. Walking the character around is the expensive part of
    // this, and the analysis is worth redoing without doing that again.
    const std::string saved = resolve("calibration.bin");
    finder.save(saved);
    std::printf("capture saved to %s -- re-analyse with --analyse\n\n", saved.c_str());

    report_findings(finder);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    Options opt;
    if (!parse_args(argc, argv, opt)) {
        print_usage();
        return 2;
    }

    Logger::instance().set_level(opt.verbose ? LogLevel::Debug : LogLevel::Info);

    // ---- config ----
    AppConfig app;
    app.load(resolve(opt.app_path));

    if (!opt.bot_path.empty())      app.bot_config_path = opt.bot_path;
    if (!opt.protocol_path.empty()) app.protocol_path   = opt.protocol_path;
    if (!opt.iface.empty())         app.interface_name  = opt.iface;
    if (!opt.window.empty())        app.window_title    = opt.window;
    if (!opt.host.empty())        { app.server_ip_start = opt.host; app.server_ip_end = opt.host; }
    if (opt.port != 0)            { app.port_start = opt.port; app.port_end = opt.port; }

    Logger::instance().set_file(resolve(app.log_path));

    // Say what is doing the picture matching.
    //
    // OpenCV is linked into this executable rather than sitting beside it as a
    // DLL, which means an empty-looking build folder is indistinguishable from
    // OpenCV not being used at all. The only honest fix is for the program to
    // say so itself, every run, where it cannot be missed.
    GB_INFO("Template matching: %s", TemplateMatcher::backend().c_str());

    // Looking at the screen needs none of the protocol machinery below.
    if (opt.list_windows) return run_list_windows();
    if (opt.doctor) return run_doctor(opt, app);
    if (opt.crash_check) return run_crash_check(opt);
    if (opt.see_only) return run_see(opt, app);
    if (opt.readself) return run_readself(opt.readself_text);
    if (opt.read_text || !opt.find_text.empty())
        return run_read(opt, app, opt.read_rect, opt.find_text);
    if (!opt.analyse_path.empty())
        return run_analyse(resolve(opt.analyse_path), opt.dump_opcode_arg,
                           opt.check_arg, opt.fields);
    if (!opt.click_spec.empty()) return run_click(opt, app, opt.click_spec);
    if (!opt.press_key.empty()) return run_press(opt, app, opt.press_key, 1200);
    if (opt.inputcheck) return run_inputcheck(opt, app);
    if (!opt.grab_spec.empty()) return run_grab(opt, app, opt.grab_spec);
    if (opt.vitals_seconds > 0) return run_vitals(opt, app, opt.vitals_seconds);
    if (opt.session_seconds > 0) return run_session(opt, app, opt.session_seconds);
    if (opt.bridge) return run_bridge(opt);
    if (opt.player_list) return run_player_list(opt, app);
    if (opt.auto_party_seconds > 0) return run_auto_party(opt, app, opt.auto_party_seconds);
    if (!opt.invite_who.empty()) return run_invite_player(opt, app, opt.invite_who);
    if (opt.invite_seconds > 0) return run_invites(opt, app, opt.invite_seconds);
    if (opt.party) return run_party(opt, app);
    if (opt.close_popups) return run_close(opt, app);
    if (opt.routines) return run_routines(opt, app);
    if (opt.set_login) return run_set_login(opt);
    if (opt.login) return run_login(opt, app);
    if (opt.keepalive_minutes > 0) return run_keepalive(opt, app, opt.keepalive_minutes);
    if (opt.templates) return run_templates(opt, app, opt.template_name);
    if (opt.whereami) return run_whereami(opt, app);
    if (opt.anchor_seconds > 0) return run_anchor(opt, app, opt.anchor_seconds);
    if (opt.hunt_seconds > 0) return run_hunt(opt, app, opt.hunt_seconds);
    if (opt.keytest) return run_keytest(opt, app);
    if (opt.calibrate_seconds > 0) {
        g_walk_share_pct = std::max(0, std::min(100, opt.walk_pct));
        return run_calibrate(opt, app, opt.calibrate_seconds);
    }

    BotConfig bot;
    bot.load(resolve(app.bot_config_path));

    // ---- protocol ----
    const std::string protocol_path = resolve(app.protocol_path);

    bool have_framing = false;
    const FramingSpec framing = load_framing(protocol_path, have_framing);

    // What the client sends is framed differently; decoding it with the
    // server's spec throws the whole direction away as noise.
    bool client_found = false;
    const FramingSpec client_framing =
        load_framing(protocol_path, client_found, "FramingClient");

    if (!have_framing) {
        std::fprintf(stderr,
            "\nNo [Framing] section in %s.\n\n"
            "GhostOnline's wire format has to be derived from your own traffic\n"
            "before the bot can read anything. Start here:\n\n"
            "  gobot_sniff --list\n"
            "  gobot_sniff --record capture.bin --host <game server ip>\n"
            "  gobot_sniff --detect capture.bin\n\n"
            "See README.md for the full walkthrough.\n",
            protocol_path.c_str());
        return 1;
    }

    // Framing confirmation is deliberately not a hard gate here. Whether the
    // framing is right is measurable at runtime — the capture reports resync
    // bytes against frames decoded, and a wrong spec resyncs constantly — and
    // gating on a config flag only stranded people who had derived the format
    // correctly. The real gate below is having packet layouts, which is the
    // part that genuinely needs human confirmation.
    {
        IniFile ini;
        ini.load(protocol_path);
        if (!ini.get_bool("Framing", "verified", false)) {
            std::fprintf(stderr,
                "\nNote: [Framing] in %s is not marked confirmed.\n"
                "Watch the resync count once running -- more than a couple of bytes\n"
                "per frame means the framing is wrong, and the bot will be acting on\n"
                "nonsense. Check with: gobot_sniff --decode\n\n",
                protocol_path.c_str());
        }
    }

    OpcodeRegistry registry;
    registry.load(protocol_path);

    if (registry.size() == 0) {
        std::fprintf(stderr,
            "\nFraming is configured but %s has no [Packet.*] layouts.\n"
            "The bot would see frames it cannot interpret.\n\n"
            "  gobot_sniff --watch          see which opcodes fire\n"
            "  gobot_sniff --diff 0x<op>    work out one packet's fields\n",
            protocol_path.c_str());
        return 1;
    }

    // ---- wiring ----
    // Declaration order is destruction order reversed. PathFinder must outlive
    // the executor and brain that hold pointers to it, so it is declared
    // first; executor must outlive brain, whose destructor enqueues a stop.
    GameState state;
    ProtocolHandler protocol(registry, state);
    InputManager input;
    ExecutionCoordinator coord;
    PathFinder pathfinder;
    CommandExecutor executor(bot, state, input, coord);
    BotBrain brain(bot, state, executor);

    executor.set_pathfinder(&pathfinder);
    brain.set_pathfinder(&pathfinder);

    protocol.set_map_data_callback([&](const MapData& map) {
        pathfinder.set_double_jump(bot.double_jump);
        pathfinder.init(map);
    });

    protocol.set_death_callback([&] {
        if (!bot.continue_auto_after_death) {
            GB_WARN("Death — disabling bot (continue_auto_after_death is off)");
            g_bot_enabled.store(false);
            brain.stop();
        }
    });

    protocol.set_captcha_callback([&] {
        // A captcha dialog eats keystrokes and an unattended bot will fail it.
        // Freezing is the honest response: the alternative is typing combat
        // keys into an anti-bot prompt.
        GB_WARN("Captcha detected — suspending all input");
        coord.engage_critical("captcha");
        g_bot_enabled.store(false);
        brain.stop();
        input.release_all();
    });

    // ---- game window ----
    if (!input.find_game_window(app.window_title, app.window_class)) {
        std::fprintf(stderr,
            "\nGame window not found (looking for title containing '%s').\n"
            "Start the game first, or pass --window with a matching substring.\n",
            app.window_title.c_str());
        if (!opt.status_only) return 1;
    } else {
        WindowFocusMonitor::instance().start(input.target_window());
        WindowFocusMonitor::instance().add_listener([&input](FocusState s) {
            if (s != FocusState::Focused) {
                // Keys held when focus leaves stay held in the game. Release
                // them or the character runs into a wall indefinitely.
                input.release_all();
            }
        });
    }

    // ---- capture ----
    PacketCapture capture;
    capture.set_framing(framing);
    // The client frames its half differently, and its half is the one that
    // carries this character's own position.
    if (client_found) capture.set_client_framing(client_framing);
    capture.set_batch_interval_ms(app.batch_interval_ms);
    capture.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        protocol.on_packets(packets);
    });
    capture.set_disconnect_callback([&] {
        GB_WARN("Traffic stopped — disabling bot");
        g_bot_enabled.store(false);
        brain.stop();
        input.release_all();
    });

    // Forward only what we can interpret; everything else is noise.
    capture.set_opcode_filter(registry.mapped_opcodes());

    CaptureConfig ccfg;
    ccfg.interface_name  = app.interface_name;
    ccfg.bpf_filter      = app.bpf_filter;
    ccfg.server_ip_start = app.server_ip_start;
    ccfg.server_ip_end   = app.server_ip_end;
    ccfg.port_start      = app.port_start;
    ccfg.port_end        = app.port_end;

    if (!capture.start(ccfg)) {
        std::fprintf(stderr, "\nCapture failed to start. Running as Administrator?\n");
        return 1;
    }

    executor.update_config(bot);
    executor.start();

    std::thread hotkeys;
    if (!opt.status_only) {
        hotkeys = std::thread(hotkey_loop, bot.key_start_stop, std::ref(brain), std::ref(executor));
        std::printf("\nReady. Press %s in game to toggle the bot. Ctrl+C to quit.\n\n",
                    InputManager::key_name(bot.key_start_stop).c_str());
    } else {
        std::printf("\nStatus mode — no input will be sent. Ctrl+C to quit.\n\n");
    }

    // ---- status loop ----
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        const auto cs = capture.stats();
        const auto ps = protocol.stats();
        const Self me = state.self();

        std::printf("\r[%s] hp %d/%d mp %d/%d  pos (%.0f,%.0f)  mobs %zu  "
                    "frames %llu  decoded %llu  unknown %llu  resync %llu   ",
                    opt.status_only ? "status"
                                    : (g_bot_enabled.load() ? to_string(brain.phase()) : "off"),
                    me.hp, me.max_hp, me.mp, me.max_mp,
                    me.pos.x, me.pos.y,
                    state.monster_count(),
                    static_cast<unsigned long long>(cs.frames_decoded),
                    static_cast<unsigned long long>(ps.decoded),
                    static_cast<unsigned long long>(ps.unknown),
                    static_cast<unsigned long long>(cs.resync_bytes));
        std::fflush(stdout);
    }

    std::printf("\n\nShutting down...\n");

    g_stop.store(true);
    if (hotkeys.joinable()) hotkeys.join();

    brain.stop();
    executor.stop();
    capture.stop();
    WindowFocusMonitor::instance().stop();
    input.release_all();

    const auto ps = protocol.stats();
    std::printf("\nSession: %llu packets, %llu decoded, %llu unknown\n",
                static_cast<unsigned long long>(ps.total),
                static_cast<unsigned long long>(ps.decoded),
                static_cast<unsigned long long>(ps.unknown));

    if (!ps.unknown_opcodes.empty()) {
        std::printf("\nUnmapped opcodes seen (candidates for protocol.ini):\n");
        std::vector<std::pair<std::uint16_t, std::uint64_t>> sorted(
            ps.unknown_opcodes.begin(), ps.unknown_opcodes.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 15); ++i) {
            std::printf("  0x%04X  x%llu\n", sorted[i].first,
                        static_cast<unsigned long long>(sorted[i].second));
        }
    }
    return 0;
}

