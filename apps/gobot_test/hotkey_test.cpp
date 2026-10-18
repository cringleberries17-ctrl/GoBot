// gobot_test -- GoBot's own keys: F10, F11, F12, "Dọn túi ngay" and "Vào game
// ngay", and which window has to be in front for each.
//
// The decision only: which keys went down, which window was in front, what
// the rest of GoBot was doing -- and what the window's tick() is told to do.
// No key is read from Windows and nothing is pressed.

#include "gobot/hotkeys.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

/// A file in this run's own temp folder, which main.cpp makes and removes:
/// see temp_file there.
std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

constexpr std::uint32_t kF10 = VK_F10, kF11 = VK_F11, kF12 = VK_F12;
constexpr std::uint32_t kTrashKey = 'K', kLoginKey = 'L';

HotkeySet keys() {
    HotkeySet k;
    k.toggle = kF10;
    k.hunt = kF11;
    k.panic = kF12;
    k.trash_now = kTrashKey;
    k.login_now = kLoginKey;
    return k;
}

/// A keyboard whose keys are held or not by the test.
struct Keyboard {
    std::set<std::uint32_t> held;
    std::function<bool(std::uint32_t)> down() {
        return [this](std::uint32_t vk) { return held.count(vk) != 0; };
    }
};

HotkeyEdges press_once(std::uint32_t vk) {
    HotkeyEdges e;
    e.toggle = vk == kF10;
    e.hunt = vk == kF11;
    e.panic = vk == kF12;
    e.trash_now = vk == kTrashKey;
    e.login_now = vk == kLoginKey;
    return e;
}

HotkeyState state(HotkeyFront front) {
    HotkeyState s;
    s.front = front;
    s.game_window = true;
    s.world = HotkeyWorld::No;
    return s;
}

const char* front_name(HotkeyFront f) {
    switch (f) {
        case HotkeyFront::None:  return "nothing";
        case HotkeyFront::Game:  return "the game";
        case HotkeyFront::GoBot: return "GoBot";
        case HotkeyFront::Other: return "another window";
    }
    return "?";
}

void test_edges() {
    section("Hotkeys: a key held down is one press");
    HotkeyEdgeTracker tracker;
    Keyboard kb;
    const HotkeySet k = keys();

    check(!tracker.update(k, kb.down()).any(), "nothing held: no edges");
    kb.held.insert(kTrashKey);
    const HotkeyEdges first = tracker.update(k, kb.down());
    check(first.trash_now && !first.toggle && !first.panic && !first.login_now,
          "the trash key goes down: one edge, for it alone");
    // Held for three seconds at sixty ticks a second.
    int more = 0;
    for (int i = 0; i < 180; ++i) more += tracker.update(k, kb.down()).trash_now ? 1 : 0;
    check(more == 0, "held down for 3 s: still only that one press (" + std::to_string(more) + " more)");
    kb.held.clear();
    check(!tracker.update(k, kb.down()).any(), "let go: nothing");
    kb.held.insert(kTrashKey);
    check(tracker.update(k, kb.down()).trash_now, "pressed again: a second press");

    // A key of 0 is no key, whatever Windows would say about it.
    HotkeySet none = k;
    none.trash_now = 0;
    none.login_now = 0;
    HotkeyEdgeTracker fresh;
    int asked_zero = 0;
    const auto everything_down = [&](std::uint32_t vk) {
        if (vk == 0) ++asked_zero;
        return true;
    };
    const HotkeyEdges e = fresh.update(none, everything_down);
    check(!e.trash_now && !e.login_now && asked_zero == 0,
          "keys set to 0 never go down, and Windows is never asked about key 0");
    check(e.toggle && e.hunt && e.panic, "while F10, F11 and F12 do");
}

void test_switches() {
    section("Hotkeys: F10 and F11 with the game or GoBot in front; F12 always (HK-A1, HK-A2)");
    for (const HotkeyFront f :
         { HotkeyFront::Game, HotkeyFront::GoBot, HotkeyFront::Other, HotkeyFront::None }) {
        const bool ours = f == HotkeyFront::Game || f == HotkeyFront::GoBot;
        const HotkeyActions toggle = hotkey_action(press_once(kF10), state(f));
        const HotkeyActions hunt = hotkey_action(press_once(kF11), state(f));
        const HotkeyActions panic = hotkey_action(press_once(kF12), state(f));
        check(toggle.toggle == ours && !toggle.hunt && !toggle.panic,
              std::string("F10 with ") + front_name(f) + " in front: " + (ours ? "start/stop" : "nothing"));
        check(hunt.hunt == ours && !hunt.toggle,
              std::string("F11 with ") + front_name(f) + " in front: " + (ours ? "hunting on/off" : "nothing"));
        check(panic.panic && !panic.toggle && !panic.hunt,
              std::string("F12 with ") + front_name(f) + " in front: everything stops");
    }
    HotkeyState listening = state(HotkeyFront::GoBot);
    listening.listening = true;
    check(!hotkey_action(press_once(kF10), listening).toggle &&
              !hotkey_action(press_once(kF11), listening).hunt,
          "a key box waiting for a key: F10 and F11 do nothing");
    check(hotkey_action(press_once(kF12), listening).panic, "and F12 still stops everything");
    HotkeyState held = state(HotkeyFront::Game);
    held.held = true;
    check(hotkey_action(press_once(kF10), held).toggle && hotkey_action(press_once(kF12), held).panic,
          "a hold of GoBot's on: F10 and F12 still act -- they stop it");
}

void test_trash_key() {
    section("Hotkeys: \"Dọn túi ngay\" -- the Dọn ngay button, with the game in front (HK-A3)");
    const HotkeyActions go = hotkey_action(press_once(kTrashKey), state(HotkeyFront::Game));
    check(go.trash && go.trash_note_vi.empty(), "game in front, nothing in the way: one pass asked for");
    {
        // Held down for three seconds, as a player resting a finger on it.
        HotkeyEdgeTracker tracker;
        Keyboard kb;
        kb.held.insert(kTrashKey);
        int passes = 0;
        for (int i = 0; i < 180; ++i)
            passes += hotkey_action(tracker.update(keys(), kb.down()), state(HotkeyFront::Game)).trash ? 1 : 0;
        check(passes == 1, "held down for 3 s: one pass, not " + std::to_string(passes));
    }
    for (const HotkeyFront f : { HotkeyFront::GoBot, HotkeyFront::Other, HotkeyFront::None }) {
        const HotkeyActions a = hotkey_action(press_once(kTrashKey), state(f));
        check(!a.trash && a.trash_note_vi.empty(),
              std::string(front_name(f)) + " in front: nothing, not even a note");
    }
    struct Gate {
        const char* what;
        void (*set)(HotkeyState&);
        const char* note;
    };
    const Gate gates[] = {
        { "a pass already under way", [](HotkeyState& s) { s.trash_busy = true; }, hotkey_text::kTrashBusy },
        { "the chat line in doubt", [](HotkeyState& s) { s.chat_doubt = true; }, hotkey_text::kTrashChat },
        { "no game window", [](HotkeyState& s) { s.game_window = false; }, hotkey_text::kTrashNoGame },
        { "an empty list", [](HotkeyState& s) { s.trash_list_empty = true; }, hotkey_text::kTrashEmpty },
    };
    for (const Gate& g : gates) {
        HotkeyState s = state(HotkeyFront::Game);
        g.set(s);
        const HotkeyActions a = hotkey_action(press_once(kTrashKey), s);
        check(!a.trash && a.trash_note_vi == g.note,
              std::string(g.what) + ": no pass, a note -- \"" + a.trash_note_vi + "\"");
    }
    HotkeyState listening = state(HotkeyFront::Game);
    listening.listening = true;
    check(!hotkey_action(press_once(kTrashKey), listening).trash, "a key box listening: nothing");
    HotkeyState held = state(HotkeyFront::Game);
    held.held = true;
    const HotkeyActions typing = hotkey_action(press_once(kTrashKey), held);
    check(!typing.trash && typing.trash_note_vi.empty(),
          "a hold of GoBot's on -- a chat line being typed, with a k in it: nothing, and no note");
    check(std::string(hotkey_text::kTrashEmpty) == "Danh sách đồ bỏ đi đang trống, nên không có gì để dọn.",
          "the empty list's note is the trash worker's own words");
}

void test_login_key() {
    section("Hotkeys: \"Vào game ngay\" -- one crossing, with the game in front (HK-A4)");
    const HotkeyActions go = hotkey_action(press_once(kLoginKey), state(HotkeyFront::Game));
    check(go.login == HotkeyActions::Login::Start, "game in front, HUD missing: one crossing asked for");
    HotkeyState unknown = state(HotkeyFront::Game);
    unknown.world = HotkeyWorld::Unknown;
    check(hotkey_action(press_once(kLoginKey), unknown).login == HotkeyActions::Login::Start,
          "no HP reading to tell by: asked for, and the keeper looks before it crosses");
    HotkeyState in = state(HotkeyFront::Game);
    in.world = HotkeyWorld::Yes;
    const HotkeyActions note = hotkey_action(press_once(kLoginKey), in);
    check(note.login == HotkeyActions::Login::None && note.login_note_vi == "Nhân vật đã ở trong game.",
          "the HUD up: no crossing, \"" + note.login_note_vi + "\"");
    HotkeyState crossing = state(HotkeyFront::Game);
    crossing.crossing = true;
    crossing.key_crossing = true;
    check(hotkey_action(press_once(kLoginKey), crossing).login == HotkeyActions::Login::Cancel,
          "pressed again during the crossing it started: cancelled, as GoC's F8");
    crossing.held = true;
    check(hotkey_action(press_once(kLoginKey), crossing).login == HotkeyActions::Login::Cancel,
          "and stopping it is allowed whatever holds are on");
    crossing.held = false;
    crossing.front = HotkeyFront::GoBot;
    check(hotkey_action(press_once(kLoginKey), crossing).login == HotkeyActions::Login::Cancel,
          "from GoBot's window too, the crossing having brought other windows forward");
    crossing.front = HotkeyFront::Other;
    check(hotkey_action(press_once(kLoginKey), crossing).login == HotkeyActions::Login::None,
          "but not from another window");
    HotkeyState keeper = state(HotkeyFront::Game);
    keeper.crossing = true;
    const HotkeyActions busy = hotkey_action(press_once(kLoginKey), keeper);
    check(busy.login == HotkeyActions::Login::None && busy.login_note_vi == hotkey_text::kCrossing,
          "the keeper's own crossing after a crash: not this key's to stop -- a note");
    for (const HotkeyFront f : { HotkeyFront::GoBot, HotkeyFront::Other, HotkeyFront::None }) {
        const HotkeyActions a = hotkey_action(press_once(kLoginKey), state(f));
        check(a.login == HotkeyActions::Login::None && a.login_note_vi.empty(),
              std::string(front_name(f)) + " in front: nothing");
    }
    HotkeyState held = state(HotkeyFront::Game);
    held.held = true;
    check(hotkey_action(press_once(kLoginKey), held).login == HotkeyActions::Login::None,
          "a hold of GoBot's on: no crossing started");
    HotkeySet none = keys();
    none.login_now = 0;
    HotkeyEdgeTracker tracker;
    const HotkeyEdges e = tracker.update(none, [](std::uint32_t) { return true; });
    check(hotkey_action(e, state(HotkeyFront::Game)).login == HotkeyActions::Login::None,
          "key 0: never a crossing");
}

void test_taken() {
    section("Hotkeys: a one-job key GoBot presses itself is never heard (S-RS2-2)");
    // The trash key set, in the full screen's macro table, to a skill's key
    // too: the macro's own presses go down as the player's would.
    HotkeyState s = state(HotkeyFront::Game);
    s.trash_key_use = "phím kỹ năng";
    const HotkeyActions a = hotkey_action(press_once(kTrashKey), s);
    check(!a.trash && a.trash_taken && a.trash_note_vi.empty(),
          "the trash key that is a skill's key too, the game in front: no pass, and taken");
    {
        // The macro pressing it for a tenth of a second every 3 s for a
        // minute, the game in front: never a pass.
        HotkeyEdgeTracker tracker;
        Keyboard kb;
        int passes = 0, taken = 0;
        for (int tick = 0; tick < 60 * 60; ++tick) {
            kb.held.clear();
            if (tick % 180 < 6) kb.held.insert(kTrashKey);
            const HotkeyActions x = hotkey_action(tracker.update(keys(), kb.down()), s);
            passes += x.trash ? 1 : 0;
            taken += x.trash_taken ? 1 : 0;
        }
        check(passes == 0 && taken == 20,
              "pressed by the macro every 3 s for a minute: " + std::to_string(passes) + " passes, " +
                  std::to_string(taken) + " presses not heard");
    }
    // The login key as a potion's key: no crossing -- nor a cancel of one
    // the key started, which every potion would otherwise be.
    HotkeyState l = state(HotkeyFront::Game);
    l.login_key_use = "bình HP";
    const HotkeyActions b = hotkey_action(press_once(kLoginKey), l);
    check(b.login == HotkeyActions::Login::None && b.login_taken && b.login_note_vi.empty(),
          "the login key that is a potion's key too: no crossing, and taken");
    l.crossing = l.key_crossing = true;
    check(hotkey_action(press_once(kLoginKey), l).login == HotkeyActions::Login::None,
          "nor a cancel of the crossing it started");
    // F10 written into app.ini by hand as the trash key, which no box checks:
    // F10 still starts and stops the bot, and the trash key is not heard.
    {
        HotkeySet same = keys();
        same.trash_now = kF10;
        HotkeyEdgeTracker tracker;
        Keyboard kb;
        kb.held.insert(kF10);
        HotkeyState f10 = state(HotkeyFront::Game);
        f10.trash_key_use = "bật/tắt bot";
        const HotkeyActions c = hotkey_action(tracker.update(same, kb.down()), f10);
        check(c.toggle && !c.trash && c.trash_taken,
              "F10 as the trash key too, by hand: start/stop as ever, no pass (the 2A tester's problem 15)");
    }
    check(hotkey_action(press_once(kTrashKey), state(HotkeyFront::Game)).trash &&
              !hotkey_action(press_once(kTrashKey), state(HotkeyFront::Game)).trash_taken,
          "a key GoBot does not press itself: a pass, as before");
    check(hotkey_text::key_taken_vi("F5", "phím kỹ năng", true) ==
              "Phím F5 cũng là phím GoBot tự bấm (phím kỹ năng), nên không dùng làm phím dọn ngay được. Hãy "
              "chọn phím khác.",
          "and the screen says why: " + hotkey_text::key_taken_vi("F5", "phím kỹ năng", true));
}

void test_clashes() {
    section("Hotkeys: no key does two jobs (HK-A5)");
    // The new boxes refuse every key GoBot presses or watches: the window
    // gives them this list.
    std::vector<NamedKey> game_keys = {
        { kF10, "bật/tắt bot" }, { kF11, "bật/tắt đánh quái" }, { kF12, "dừng ngay" },
        { '1', "bình HP" },      { '2', "bình MP" },           { 'A', "buff" },
        { VK_CONTROL, "đánh" },  { 'Z', "nhặt đồ" },           { 'I', "mở túi đồ" },
        { 'U', "mở cửa sổ Nhiệm Vụ" },
    };
    for (const std::uint32_t vk : { kF10, kF11, kF12, static_cast<std::uint32_t>('1'),
                                    static_cast<std::uint32_t>('2'), static_cast<std::uint32_t>('A'),
                                    static_cast<std::uint32_t>(VK_CONTROL),
                                    static_cast<std::uint32_t>('Z'), static_cast<std::uint32_t>('I'),
                                    static_cast<std::uint32_t>('U') }) {
        const NamedKey* taken = key_taken(vk, game_keys);
        check(taken != nullptr, "a new key box refuses " + std::to_string(vk) + ": already " +
                                    (taken ? taken->what_vi : std::string("?")));
    }
    check(key_taken('K', game_keys) == nullptr && key_taken(0, game_keys) == nullptr,
          "a free key, and no key at all, are taken by nothing");
    game_keys.push_back({ 0, "nothing" });
    check(key_taken(0, game_keys) == nullptr, "an unset key in the list takes nothing either");

    // Every box refuses GoBot's own keys, the new two among them -- each box
    // its own key excepted.
    const HotkeySet own = keys();
    const auto use = [&](std::uint32_t got, std::uint32_t mine) {
        const char* u = own_key_use_vi(got, mine, own);
        return u ? std::string(u) : std::string();
    };
    check(use(kF10, '1') == "bật/tắt bot" && use(kF11, '1') == "bật/tắt bot" && use(kF12, '1') == "bật/tắt bot",
          "a potion's box refuses F10, F11 and F12");
    check(use(kTrashKey, '1') == "dọn túi ngay" && use(kLoginKey, '1') == "vào game ngay",
          "and the two new keys once they are set: " + use(kTrashKey, '1') + ", " + use(kLoginKey, '1'));
    check(use(kLoginKey, kTrashKey) == "vào game ngay" && use(kTrashKey, kLoginKey) == "dọn túi ngay",
          "the two new boxes refuse each other's key");
    check(use(kTrashKey, kTrashKey).empty() && use('5', '1').empty() && use(0, '1').empty(),
          "a box's own key, a free key and no key are refused by nothing");
    HotkeySet unset = own;
    unset.trash_now = unset.login_now = 0;
    check(own_key_use_vi(kTrashKey, '1', unset) == nullptr,
          "with the new keys unset, K is any other key");
}

void test_config() {
    section("Hotkeys: [Hotkeys] in configs/app.ini (HK-A6)");
    const std::string path = temp_path("gobot_hotkeys_test.ini");
    write_all(path, "# mine\n[Simple]\nrefocus=true\n");
    check(HotkeyConfig::save_key(path, "trash_now", VK_F7) && HotkeyConfig::save_key(path, "login_now", VK_F8),
          "both keys written");
    IniFile ini;
    HotkeyConfig c;
    check(ini.load(path), "the file reads back");
    c.load(ini);
    check(c.trash_now == VK_F7 && c.login_now == VK_F8, "and round-trips: F7 and F8");
    const std::string text = read_all(path);
    check(text.find("# mine") != std::string::npos && text.find("refocus=true") != std::string::npos,
          "the rest of the file is left as it was");
    check(HotkeyConfig::save_key(path, "trash_now", 0), "a key cleared");
    IniFile cleared;
    HotkeyConfig z;
    cleared.load(path);
    z.load(cleared);
    check(z.trash_now == 0 && z.login_now == VK_F8, "reads back as no key, the other kept");
    write_all(path, "[Hotkeys]\ntrash_now=999\nlogin_now=-4\n");
    IniFile bad;
    HotkeyConfig b;
    bad.load(path);
    b.load(bad);
    check(b.trash_now == 0 && b.login_now == 0, "numbers that are no key: no key");
    // A letter or digit written in by hand, which the boxes refuse: typed in
    // the game's chat it would fire the key.
    write_all(path, "[Hotkeys]\ntrash_now=75\nlogin_now=49\n");
    IniFile typed;
    HotkeyConfig t;
    typed.load(path);
    // And the log says so, each key by its name: a key that does nothing is
    // otherwise a fault to the player.
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(50);
    log.clear_ring();
    log.set_level(LogLevel::Warn);
    t.load(typed);
    log.set_level(was);
    int said = 0;
    for (const LogLine& l : log.ring())
        said += l.text.find("not an F-key") != std::string::npos &&
                        (l.text.find("trash_now=75") != std::string::npos ||
                         l.text.find("login_now=49") != std::string::npos)
                    ? 1
                    : 0;
    log.set_ring_capacity(0);
    check(t.trash_now == 0 && t.login_now == 0, "K and 1 written in by hand: no key (R2A-1)");
    check(said == 2, "and a line in the log for each: " + std::to_string(said) + " of 2 (R2A-1)");
    DeleteFileA(path.c_str());

    // What the two boxes take: F1 to F12 only, as their notes say, or none.
    check(one_job_key_allowed(0) && one_job_key_allowed(VK_F1) && one_job_key_allowed(VK_F9) &&
              one_job_key_allowed(VK_F12),
          "no key, F1, F9 and F12 may be a one-job key");
    check(!one_job_key_allowed(VK_F13) && !one_job_key_allowed(VK_F24),
          "F13 and F24 may not: the boxes and the notes say F1 to F12 (the round 2 tester's problem 7)");
    bool text_refused = true;
    for (const std::uint32_t vk : { std::uint32_t('K'), std::uint32_t('1'), std::uint32_t(VK_SPACE),
                                    std::uint32_t(VK_OEM_1), std::uint32_t(VK_OEM_PERIOD),
                                    std::uint32_t(VK_NUMPAD1), std::uint32_t(VK_MULTIPLY),
                                    std::uint32_t(VK_RETURN), std::uint32_t(VK_ESCAPE),
                                    std::uint32_t(VK_HOME), std::uint32_t(VK_BACK) })
        text_refused = text_refused && !one_job_key_allowed(vk);
    check(text_refused, "letters, digits, Space, punctuation, the numpad, Enter, Esc, Home and "
                        "Backspace may not (R2A-1)");

#ifdef GOBOT_SOURCE_DIR
    IniFile shipped;
    HotkeyConfig s;
    s.trash_now = s.login_now = 1;
    const bool read = shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/app.ini");
    s.load(shipped);
    check(read && shipped.has_key("Hotkeys", "trash_now") && shipped.has_key("Hotkeys", "login_now") &&
              s.trash_now == 0 && s.login_now == 0,
          "the shipped app.ini has [Hotkeys] trash_now=0 and login_now=0: no key");
#endif
}

} // namespace

void test_hotkeys(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_edges();
    test_switches();
    test_trash_key();
    test_login_key();
    test_taken();
    test_clashes();
    test_config();
}
