#include "gobot/auto_login.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/session_manager.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

namespace gobot {
namespace {

/// Hex, because a config file has to survive being opened in an editor and a
/// blob with a newline in it does not.
std::string to_hex(const std::vector<std::uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

bool from_hex(const std::string& text, std::vector<std::uint8_t>& out) {
    if (text.size() % 2 != 0) return false;
    out.clear();
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        int value = 0;
        for (int half = 0; half < 2; ++half) {
            const char c = text[i + half];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            value = value * 16 + digit;
        }
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return true;
}

/// For pointing at when a page wants no class or process filter.
const std::string kNothing;

} // namespace

const char* AutoLogin::step_name(Step s) {
    switch (s) {
        case Step::Idle:           return "idle";
        case Step::StartLauncher:  return "starting the launcher";
        case Step::FindLauncher:   return "finding the launcher";
        case Step::LoginPage:      return "login page";
        case Step::GameList:       return "choosing the game";
        case Step::StartGame:      return "start game";
        case Step::ClientLauncher: return "client launcher";
        case Step::GameWindow:     return "waiting for the game window";
        case Step::Connect:        return "connect";
        case Step::Channel:        return "choosing a channel";
        case Step::ConnectAgain:   return "connect again";
        case Step::Character:      return "choosing the character";
        case Step::InWorld:        return "in the world";
        case Step::Blocked:        return "blocked";
    }
    return "?";
}

const char* AutoLogin::on_name(On w) {
    switch (w) {
        case On::Publisher: return "the publisher's launcher";
        case On::Client:    return "the game launcher";
        case On::Game:      return "the game";
    }
    return "?";
}

const char* AutoLogin::pick_name(Pick p) {
    switch (p) {
        case Pick::One:  return "one";
        case Pick::Row:  return "row";
        case Pick::Slot: return "slot";
        case Pick::Step: return "step";
    }
    return "one";
}

AutoLogin::Pick AutoLogin::pick_from(const std::string& word, Pick fallback) {
    if (word == "one")  return Pick::One;
    if (word == "row")  return Pick::Row;
    if (word == "slot") return Pick::Slot;
    if (word == "step") return Pick::Step;
    return fallback;
}

std::vector<AutoLogin::Page> AutoLogin::default_pages() {
    // Timings from a real crossing: the launcher takes about ten seconds to
    // show its window, the client another sixteen to appear after the start
    // button, and the world about twenty after the character is picked. The
    // waits here are the generous version of those.
    //
    // No pictures are named here. Which picture marks which page belongs to
    // the install, not to this program -- see configs/login.ini.
    auto page = [](Step step, const char* key, On on, int wait_s, float score) {
        Page p;
        p.step   = step;
        p.key    = key;
        p.on     = on;
        p.wait_s = wait_s;
        p.score  = score;
        return p;
    };

    std::vector<Page> out{
        page(Step::LoginPage,      "login_page",    On::Publisher, 60, 0.55f),
        page(Step::GameList,       "game_list",     On::Publisher, 60, 0.70f),
        page(Step::StartGame,      "start_game",    On::Publisher, 60, 0.70f),
        page(Step::ClientLauncher, "client_start",  On::Client,    90, 0.70f),
        page(Step::Connect,        "connect",       On::Game,      90, 0.70f),
        page(Step::Channel,        "channel_list",  On::Game,      60, 0.70f),
        // "Kết Nối" a second time. Picking a channel closes the list and puts
        // the same connect button back up; without this press the character
        // screen never comes, which showed as the flow waiting out its whole
        // minute on 'character' and then starting the crossing over. The
        // second attempt then worked -- because restarting is what supplied
        // the missing press.
        page(Step::ConnectAgain,   "connect_again", On::Game,      60, 0.70f),
        page(Step::Character,      "character",     On::Game,      60, 0.70f),
    };

    // The account's games and the channels are lists. The characters stand
    // side by side on a stage and are picked where they stand, then started
    // with a button in the corner -- two clicks, which is what Slot is. The
    // measurements live in the config file, because they belong to the
    // client's artwork rather than to this program.
    for (Page& p : out) {
        if (p.step == Step::GameList || p.step == Step::Channel) p.pick = Pick::Row;
        if (p.step == Step::Character)                           p.pick = Pick::Slot;
    }
    return out;
}

// ------------------------------------------------------------------ settings

bool AutoLogin::Settings::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) return false;

    launcher_path   = ini.get("Login", "launcher", launcher_path);
    launcher_window = ini.get("Login", "launcher_window", "VTCZone");
    client_window   = ini.get("Login", "client_window", client_window);
    client_class    = ini.get("Login", "client_class", client_class);
    client_process  = ini.get("Login", "client_process", client_process);
    game_window     = ini.get("Login", "game_window", "GhostOnlineVTC");
    game_class      = ini.get("Login", "game_class", game_class);
    game_process    = ini.get("Login", "game_process", game_process);

    username           = ini.get("Login", "username", {});
    password_protected = ini.get("Login", "password", {});

    channel_index   = ini.get_int("Login", "channel_index", channel_index);
    character_index = ini.get_int("Login", "character_index", character_index);
    game_slot     = ini.get_int("Login", "game_slot", game_slot);
    attempts      = std::clamp(ini.get_int("Login", "attempts", attempts), 1, 5);

    heal_on_arrival = ini.get_bool("Login", "heal_on_arrival", heal_on_arrival);
    heal_hp_key = static_cast<std::uint32_t>(ini.get_int("Login", "heal_hp_key", 0));
    heal_mp_key = static_cast<std::uint32_t>(ini.get_int("Login", "heal_mp_key", 0));

    pages = default_pages();
    for (Page& page : pages) {
        const std::string section = "Page." + page.key;
        page.picture  = ini.get(section, "picture", page.picture);
        page.click_dx = ini.get_int(section, "click_dx", page.click_dx);
        page.click_dy = ini.get_int(section, "click_dy", page.click_dy);
        page.wait_s   = ini.get_int(section, "wait_s", page.wait_s);
        page.score    = ini.get_float(section, "score", page.score);
        page.pick     = pick_from(ini.get(section, "pick", pick_name(page.pick)),
                                  page.pick);
        page.row_step = ini.get_int(section, "row_step", page.row_step);
        page.row_across = ini.get_bool(section, "row_across", page.row_across);
        page.row_dx   = ini.get_int(section, "row_dx", page.row_dx);
        page.row_dy   = ini.get_int(section, "row_dy", page.row_dy);
        page.row_count  = ini.get_int(section, "row_count", page.row_count);
        page.step_dx  = ini.get_int(section, "step_dx", page.step_dx);
        page.step_dy  = ini.get_int(section, "step_dy", page.step_dy);
        page.step_pause_ms = std::max(
            100, ini.get_int(section, "step_pause_ms", page.step_pause_ms));
    }
    return true;
}

bool AutoLogin::Settings::save(const std::string& path) const {
    IniFile ini;
    ini.set("Login", "launcher", launcher_path);
    ini.set("Login", "launcher_window", launcher_window);
    ini.set("Login", "client_window", client_window);
    ini.set("Login", "client_class", client_class);
    ini.set("Login", "client_process", client_process);
    ini.set("Login", "game_window", game_window);
    ini.set("Login", "game_class", game_class);
    ini.set("Login", "game_process", game_process);
    ini.set("Login", "username", username);
    ini.set("Login", "password", password_protected);
    ini.set_int("Login", "channel_index", channel_index);
    ini.set_int("Login", "character_index", character_index);
    ini.set_int("Login", "game_slot", game_slot);
    ini.set_int("Login", "attempts", attempts);
    ini.set_bool("Login", "heal_on_arrival", heal_on_arrival);
    ini.set_int("Login", "heal_hp_key", static_cast<int>(heal_hp_key));
    ini.set_int("Login", "heal_mp_key", static_cast<int>(heal_mp_key));

    for (const Page& page : pages) {
        const std::string section = "Page." + page.key;
        ini.set(section, "picture", page.picture);
        ini.set_int(section, "click_dx", page.click_dx);
        ini.set_int(section, "click_dy", page.click_dy);
        ini.set_int(section, "wait_s", page.wait_s);
        ini.set_float(section, "score", page.score);
        ini.set(section, "pick", pick_name(page.pick));
        ini.set_int(section, "row_step", page.row_step);
        ini.set_bool(section, "row_across", page.row_across);
        ini.set_int(section, "row_dx", page.row_dx);
        ini.set_int(section, "row_dy", page.row_dy);
        ini.set_int(section, "row_count", page.row_count);
        ini.set_int(section, "step_dx", page.step_dx);
        ini.set_int(section, "step_dy", page.step_dy);
        ini.set_int(section, "step_pause_ms", page.step_pause_ms);
    }

    return ini.save(path,
        "GoBot auto-login\n"
        "\n"
        "The crossing from a cold machine to a character in the world. The\n"
        "sequence is fixed; what each page looks like is not, because those\n"
        "pages belong to the publisher's launcher and differ between installs.\n"
        "\n"
        "Each [Page.*] names the picture that identifies it. Fill one in by\n"
        "getting the launcher onto that page and running:\n"
        "\n"
        "  gobot_cli --window <launcher title> --tplsrc <picture set> --survey\n"
        "\n"
        "whereupon it prints what it found and where. Put the name here.\n"
        "\n"
        "click_dx/click_dy say where to click relative to the picture's top\n"
        "left corner; -1 for both means the middle of it. For the two pages\n"
        "that are a list -- the account's games and the channel list --\n"
        "row_step is how far apart the entries are, and the index is taken\n"
        "from game_slot and channel_index above.\n"
        "\n"
        "The password is encrypted with this Windows account's own key, so the\n"
        "file is of no use to another user or on another machine. Write it in\n"
        "with `gobot_cli --setlogin`, not by hand.\n");
}

std::vector<const AutoLogin::Page*> AutoLogin::Settings::unidentified() const {
    std::vector<const Page*> out;
    for (const Page& page : pages)
        if (!page.identified()) out.push_back(&page);
    return out;
}

// -------------------------------------------------------------- the password

std::string AutoLogin::protect(const std::string& plain) {
    if (plain.empty()) return {};

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"GoBot login", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    std::vector<std::uint8_t> bytes(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return to_hex(bytes);
}

std::string AutoLogin::unprotect(const std::string& stored) {
    if (stored.empty()) return {};

    std::vector<std::uint8_t> bytes;
    if (!from_hex(stored, bytes) || bytes.empty()) return {};

    DATA_BLOB in{};
    in.pbData = bytes.data();
    in.cbData = static_cast<DWORD>(bytes.size());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

// ------------------------------------------------------------------ the flow

AutoLogin::AutoLogin() = default;

void AutoLogin::set_settings(const Settings& s) { settings_ = s; }

void AutoLogin::say(Step s, const std::string& detail) {
    step_.store(s);
    GB_INFO("AutoLogin [%s] %s", step_name(s), detail.c_str());
    if (report_) report_(s, detail);
}

bool AutoLogin::nap(int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (stop_now()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !stop_now();
}

bool AutoLogin::wait_for_page(const Page& page, ScreenRect& found) {
    if (!store_) { trouble_ = "no pictures loaded"; return false; }
    const Frame* picture = store_->get(page.picture);
    if (!picture) {
        trouble_ = "there is no picture called '" + page.picture + "'";
        return false;
    }

    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::seconds(std::max(1, page.wait_s));
    Frame frame;

    while (std::chrono::steady_clock::now() < until) {
        if (stop_now()) { trouble_ = "cancelled"; return false; }

        if (input_.window_valid() &&
            capture_.grab(input_.target_window(), frame) && frame.valid()) {
            const auto hit = TemplateMatcher::find(
                frame, ScreenRect{ 0, 0, frame.width, frame.height },
                *picture, page.score);
            if (hit.found) { found = hit.at; return true; }
        }
        if (!nap(900)) { trouble_ = "cancelled"; return false; }
    }

    trouble_ = "waited " + std::to_string(page.wait_s) + "s for '" + page.key +
               "' and it never appeared";
    return false;
}

bool AutoLogin::run() {
    cancelled_.store(false);
    trouble_.clear();

    // Refuse before touching anything, rather than halfway through. A flow
    // that starts the launcher and then discovers it cannot read page three
    // has left a window open and a person to close it.
    const auto missing = settings_.unidentified();
    if (!missing.empty()) {
        std::string list;
        for (const Page* page : missing) {
            if (!list.empty()) list += ", ";
            list += page->key;
        }
        trouble_ = "these pages have no picture named for them yet: " + list +
                   ".  Get the launcher onto each and run --survey to find out "
                   "what marks it, then write the name into configs/login.ini.";
        say(Step::Blocked, trouble_);
        return false;
    }
    if (settings_.launcher_path.empty()) {
        trouble_ = "no launcher is configured";
        say(Step::Blocked, trouble_);
        return false;
    }

    // The account is only wanted if the login page is actually going to be
    // used. A launcher that is already signed in never shows one, and refusing
    // to run for want of a password that will not be typed is refusing for no
    // reason -- which is the state this was in.
    const bool need_account = std::any_of(
        settings_.pages.begin(), settings_.pages.end(),
        [](const Page& p) { return p.step == Step::LoginPage; });
    if (need_account &&
        (settings_.username.empty() || settings_.password_protected.empty())) {
        GB_INFO("AutoLogin: no account written down. That is only a problem if "
                "the launcher asks to sign in; it will be skipped if it does not.");
    }

    for (int attempt = 1; attempt <= settings_.attempts; ++attempt) {
        if (stop_now()) return false;
        GB_INFO("AutoLogin: attempt %d of %d", attempt, settings_.attempts);

        // ---- the publisher's launcher ----
        say(Step::StartLauncher, settings_.launcher_path);
        if (!SessionManager::process_running("VTCZone.exe") &&
            !SessionManager::start_program(settings_.launcher_path)) {
            trouble_ = "could not start " + settings_.launcher_path;
            continue;
        }

        say(Step::FindLauncher, settings_.launcher_window);
        bool attached = false;
        for (int i = 0; i < 30 && !attached; ++i) {
            attached = input_.find_game_window(settings_.launcher_window, {}, {}) &&
                       input_.window_valid();
            if (!attached && !nap(1000)) return false;
        }
        if (!attached) {
            trouble_ = "the launcher's window never appeared";
            continue;
        }
        input_.focus_window();
        if (!nap(1200)) return false;

        // ---- its pages, in order, starting from wherever it already is ----
        //
        // A launcher that is already open is usually part way through: signed
        // in, sitting on the game list, or waiting on the start button. Walking
        // from the first page would then mean waiting a minute for a login
        // screen that will never come, and failing.
        //
        // So before starting, look at what is on screen and begin at the last
        // page that is visible. The helper this is modelled on does the same
        // and logs it as skipping forward.
        // Look at all three windows, not just the publisher's.
        //
        // The crossing passes through three of them, and the later pages are
        // not drawn on the first. Scanning only the publisher's launcher means
        // a client that is already started and sitting on its own START button
        // looks like no progress at all, so the flow presses "Chơi game" again
        // and a second launcher appears on top of the first.
        std::size_t from = 0;
        {
            InputManager client, game;
            Frame frames[3];
            if (input_.window_valid()) capture_.grab(input_.target_window(), frames[0]);
            if (client.find_game_window(settings_.client_window, settings_.client_class,
                                        settings_.client_process))
                capture_.grab(client.target_window(), frames[1]);
            if (game.find_game_window(settings_.game_window, settings_.game_class,
                                      settings_.game_process))
                capture_.grab(game.target_window(), frames[2]);

            for (std::size_t i = 0; i < settings_.pages.size(); ++i) {
                // A picture that marks two different pages cannot say which
                // of them you are on. Both connect steps are the same button,
                // so seeing it proves only that you have reached the first --
                // skipping to the second on the strength of it would walk
                // past the channel list.
                bool ambiguous = false;
                for (std::size_t j = 0; j < i && !ambiguous; ++j)
                    ambiguous = settings_.pages[j].picture == settings_.pages[i].picture;
                if (ambiguous) continue;

                const Frame* picture = store_->get(settings_.pages[i].picture);
                if (!picture) continue;
                for (const Frame& now : frames) {
                    if (!now.valid()) continue;
                    const auto hit = TemplateMatcher::find(
                        now, ScreenRect{ 0, 0, now.width, now.height }, *picture,
                        settings_.pages[i].score);
                    if (hit.found) { from = i; break; }   // the last one up, not the first
                }
            }
            // Never skip past the page the flow was asked to stop at.
            //
            // Skipping forward is right for a launcher that is already signed
            // in -- those pages never come, and waiting for them fails. It is
            // wrong when somebody asked to be left on the channel list and is
            // taken to the character select instead, which is the opposite of
            // what they asked for.
            if (!halt_at_.empty()) {
                for (std::size_t i = 0; i < settings_.pages.size(); ++i)
                    if (settings_.pages[i].key == halt_at_) from = std::min(from, i);
            }

            if (from > 0)
                say(settings_.pages[from].step,
                    "already as far as '" + settings_.pages[from].key +
                    "', so the " + std::to_string(from) + " page(s) before it are skipped");
        }

        bool crossed = true;
        On aimed_at = On::Publisher;      // where `input_` is pointed right now
        for (std::size_t step_index = from; step_index < settings_.pages.size();
             ++step_index) {
            const Page& page = settings_.pages[step_index];
            if (stop_now()) return false;

            // Reached the login page after all, with no account to type.
            if (page.step == Step::LoginPage &&
                (settings_.username.empty() || settings_.password_protected.empty())) {
                trouble_ = "the launcher is asking to sign in and no account is "
                           "written down -- run `gobot_cli --setlogin`";
                crossed = false;
                break;
            }

            // Attach to the window this page is drawn on.
            //
            // Per page, not at a fixed step. The old rule switched windows on
            // reaching ClientLauncher and Connect, which is right only when
            // the flow walks the whole crossing: resuming into the middle --
            // skip-forward landing on the channel list, say -- left it looking
            // for a page of the game on the publisher's launcher, where it
            // waited out the whole minute and then started over.
            //
            // The game's launcher is found by class and program rather than by
            // name, because it has no name: it is a bare #32770 dialog put up
            // by GhostOnlineVTC.exe.
            if (page.on != aimed_at) {
                const std::string* want  = &settings_.launcher_window;
                const std::string* klass = &kNothing;
                const std::string* exe   = &kNothing;
                if (page.on == On::Client) {
                    want  = &settings_.client_window;
                    klass = &settings_.client_class;
                    exe   = &settings_.client_process;
                } else if (page.on == On::Game) {
                    want  = &settings_.game_window;
                    klass = &settings_.game_class;
                    exe   = &settings_.game_process;
                }

                say(page.step, std::string("switching to ") + on_name(page.on));
                bool here = false;
                for (int i = 0; i < 60 && !here; ++i) {
                    here = input_.find_game_window(*want, *klass, *exe) &&
                           input_.window_valid();
                    if (!here && !nap(1000)) return false;
                }
                if (!here) {
                    std::string which = want->empty() ? std::string{} : "'" + *want + "'";
                    if (!klass->empty())
                        which += (which.empty() ? "" : " ") + ("class " + *klass);
                    if (!exe->empty())
                        which += (which.empty() ? "" : " ") + ("from " + *exe);
                    trouble_ = which + " never appeared";
                    crossed = false;
                    break;
                }
                input_.focus_window();
                aimed_at = page.on;
                if (!nap(800)) return false;
            }

            say(page.step, "looking for '" + page.picture + "'");
            ScreenRect at;
            if (!wait_for_page(page, at)) { crossed = false; break; }

            // Asked to stop here. The page is up and found; leave it that way.
            if (!halt_at_.empty() && page.key == halt_at_) {
                say(page.step, "stopping here as asked -- '" + page.key +
                               "' is up at " + std::to_string(at.x) + "," +
                               std::to_string(at.y) + " " + std::to_string(at.w) +
                               "x" + std::to_string(at.h));
                return true;
            }

            // Where to click: the middle of the picture unless told otherwise,
            // plus one row per index for the pages that are a list.
            //
            // The offsets are measured from the picture, not from the window,
            // because the picture is the only thing whose position is known --
            // these screens are drawn centred in a client area whose size the
            // player chooses.
            int cx = at.x + (page.click_dx >= 0 ? page.click_dx : at.w / 2);
            int cy = at.y + (page.click_dy >= 0 ? page.click_dy : at.h / 2);

            if (page.pick != Pick::One) {
                int index = 0;
                switch (page.step) {
                    case Step::Channel:   index = settings_.channel_index; break;
                    case Step::Character: index = settings_.character_index; break;
                    default:              index = settings_.game_slot; break;
                }
                index = std::max(0, index);

                // An index past the end is a typo, and clicking where the
                // ninth of eight entries would be lands on whatever is below
                // the list. Say so and use the last one.
                if (page.row_count > 0 && index >= page.row_count) {
                    GB_WARN("AutoLogin: '%s' has %d entries and number %d was asked "
                            "for; using the last one", page.key.c_str(),
                            page.row_count, index + 1);
                    index = page.row_count - 1;
                }
                say(page.step, "entry " + std::to_string(index + 1) + " of " +
                               (page.row_count > 0 ? std::to_string(page.row_count)
                                                   : std::string("?")));

                if (page.pick == Pick::Row) {
                    (page.row_across ? cx : cy) += page.row_step * index;
                } else if (page.pick == Pick::Slot) {
                    // Two clicks: the character where it stands, then the
                    // button that starts the game. The pause between them is
                    // not politeness -- confirming in the same instant as the
                    // choice is how you start the previous one.
                    int sx = at.x + page.row_dx;
                    int sy = at.y + page.row_dy;
                    (page.row_across ? sx : sy) += page.row_step * index;
                    input_.click_at(sx, sy);
                    say(page.step, "slot " + std::to_string(index + 1) + " at " +
                                   std::to_string(sx) + "," + std::to_string(sy));
                    if (!nap(page.step_pause_ms)) return false;
                } else {
                    // A carousel. Every entry is shown in the same place, so
                    // the index cannot move the click -- it says how many
                    // times to press the arrow first.
                    const int ax = at.x + page.step_dx;
                    const int ay = at.y + page.step_dy;
                    for (int n = 0; n < index; ++n) {
                        if (stop_now()) return false;
                        input_.click_at(ax, ay);
                        say(page.step, "next (" + std::to_string(n + 1) + " of " +
                                       std::to_string(index) + ") at " +
                                       std::to_string(ax) + "," + std::to_string(ay));
                        if (!nap(page.step_pause_ms)) return false;
                    }
                }
            }

            // The login page is the one that needs more than a click.
            if (page.step == Step::LoginPage) {
                const std::string password = unprotect(settings_.password_protected);
                if (password.empty()) {
                    trouble_ = "the stored password could not be read back -- it was "
                               "saved by a different Windows user or on a different "
                               "machine.  Run `gobot_cli --setlogin` again.";
                    crossed = false;
                    break;
                }
                // Username field, then password field, then the button. The
                // fields sit above the button the picture marks; how far above
                // is what click_dy on this page is for.
                input_.click_at(cx, cy - 80);
                if (!nap(300)) return false;
                input_.type_text(settings_.username);
                if (!nap(300)) return false;
                input_.click_at(cx, cy - 40);
                if (!nap(300)) return false;
                input_.type_text(password);
                if (!nap(400)) return false;
            }

            input_.click_at(cx, cy);
            say(page.step, "clicked at " + std::to_string(cx) + "," +
                           std::to_string(cy));
            if (!nap(1500)) return false;
        }
        if (!crossed) {
            GB_WARN("AutoLogin: attempt %d got as far as [%s] -- %s",
                    attempt, step_name(step_.load()), trouble_.c_str());
            if (!nap(5000)) return false;
            continue;
        }

        // ---- and then the world ----
        //
        // Waited for by looking for the HUD rather than for a picture of the
        // success screen, because the HUD is what "in the world" actually
        // means. It needs no template, and it is the same test the session
        // watcher uses to notice the opposite.
        say(Step::GameWindow, settings_.game_window);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        Frame frame;
        bool arrived = false;
        while (!arrived && std::chrono::steady_clock::now() < until) {
            if (stop_now()) return false;
            if (input_.find_game_window(settings_.game_window, {}, {}) &&
                input_.window_valid() &&
                capture_.grab(input_.target_window(), frame))
                arrived = SessionManager::hud_present(frame);
            if (!arrived && !nap(1500)) return false;
        }
        if (!arrived) {
            trouble_ = "the character never reached the world";
            GB_WARN("AutoLogin: attempt %d -- %s", attempt, trouble_.c_str());
            continue;
        }

        if (settings_.heal_on_arrival) {
            input_.focus_window();
            if (!nap(400)) return false;
            if (settings_.heal_hp_key) input_.key_press(settings_.heal_hp_key);
            if (settings_.heal_mp_key) input_.key_press(settings_.heal_mp_key);
        }

        say(Step::InWorld, "logged in");
        return true;
    }

    say(Step::Blocked, trouble_.empty() ? "out of attempts" : trouble_);
    return false;
}

} // namespace gobot
