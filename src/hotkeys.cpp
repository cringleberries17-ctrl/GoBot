#include "gobot/hotkeys.hpp"
#include "gobot/logger.hpp"

namespace gobot {

HotkeyEdges HotkeyEdgeTracker::update(const HotkeySet& keys,
                                      const std::function<bool(std::uint32_t)>& down) {
    const std::uint32_t vks[5] = { keys.toggle, keys.hunt, keys.panic, keys.trash_now,
                                   keys.login_now };
    bool went[5] = {};
    for (int i = 0; i < 5; ++i) {
        // A key of 0 is no key. Asking Windows about it would ask about
        // nothing in particular, and an unset key must never fire.
        const bool now = vks[i] != 0 && down && down(vks[i]);
        went[i] = now && !was_[i];
        was_[i] = now;
    }
    HotkeyEdges e;
    e.toggle = went[0];
    e.hunt = went[1];
    e.panic = went[2];
    e.trash_now = went[3];
    e.login_now = went[4];
    return e;
}

HotkeyActions hotkey_action(const HotkeyEdges& edges, const HotkeyState& s) {
    HotkeyActions a;

    // The panic stop works from anywhere, and even while a key box waits for
    // a key: when something misbehaves, finding the right window first is
    // exactly what there is no time for.
    a.panic = edges.panic;

    // A key box waiting for a key has it: binding a key is not pressing it.
    if (s.listening) return a;

    // Start/stop and hunting: with the game in front -- the point of a key --
    // or GoBot's own window, where it does what the button beside it does.
    // Typed into anything else it was meant for that, not for GoBot.
    const bool ours = s.front == HotkeyFront::Game || s.front == HotkeyFront::GoBot;
    a.toggle = edges.toggle && ours;
    a.hunt = edges.hunt && ours;

    // A one-job key that GoBot presses itself as well -- set to a skill's key
    // in the full screen's macro table, or by hand in a file -- goes down at
    // each of GoBot's own presses, which cannot be told from the player's: a
    // bag gone through again after every skill. Never heard, whatever is in
    // front, a hold or not.
    a.trash_taken = edges.trash_now && !s.trash_key_use.empty();
    a.login_taken = edges.login_now && !s.login_key_use.empty();
    const bool trash_now = edges.trash_now && !a.trash_taken;
    const bool login_now = edges.login_now && !a.login_taken;

    // "Vào game ngay" pressed again while the crossing it started is under
    // way stops that crossing, as GoC's F8 does -- from GoBot's window too,
    // since the crossing brings other windows forward. Stopping is always
    // allowed. A crossing the keeper started on its own, after a crash, is
    // not this key's to stop.
    if (login_now && s.key_crossing && ours) a.login = HotkeyActions::Login::Cancel;

    // While GoBot itself presses keys under a hold, a one-job key going down
    // may be one of its own presses -- a letter of a chat line -- and nothing
    // is started for it.
    if (s.held) return a;

    // "Dọn túi ngay": only with the game in front, and only when the "Dọn
    // ngay" button would be pressable. When it is not, say why: the player
    // pressed it in the game, meaning it for GoBot.
    if (trash_now && s.front == HotkeyFront::Game) {
        if (s.trash_busy)            a.trash_note_vi = hotkey_text::kTrashBusy;
        else if (s.chat_doubt)       a.trash_note_vi = hotkey_text::kTrashChat;
        else if (!s.game_window)     a.trash_note_vi = hotkey_text::kTrashNoGame;
        else if (s.trash_list_empty) a.trash_note_vi = hotkey_text::kTrashEmpty;
        else                         a.trash = true;
    }

    // "Vào game ngay": one crossing, with the game in front and nobody in the
    // world as far as the HP reader can tell. With no reading to tell by it
    // is asked for anyway: the keeper looks at the game itself before it
    // crosses.
    if (login_now && !s.key_crossing && s.front == HotkeyFront::Game) {
        if (s.crossing)                        a.login_note_vi = hotkey_text::kCrossing;
        else if (s.world == HotkeyWorld::Yes)  a.login_note_vi = hotkey_text::kInWorld;
        else                                   a.login = HotkeyActions::Login::Start;
    }
    return a;
}

std::string hotkey_text::key_taken_vi(const std::string& key, const std::string& use, bool trash) {
    return "Phím " + key + " cũng là phím GoBot tự bấm (" + use + "), nên không dùng làm phím " +
           (trash ? "dọn ngay" : "vào game ngay") + " được. Hãy chọn phím khác.";
}

bool one_job_key_allowed(std::uint32_t vk) {
    // 0x70 to 0x7B are VK_F1 to VK_F12: what the boxes and their notes ask
    // for. F13 to F24 a keyboard seldom has, and a file edited by hand with
    // one is told so as any other key that is not F1 to F12 is.
    return vk == 0 || (vk >= 0x70 && vk <= 0x7B);
}

void HotkeyConfig::load(const IniFile& ini) {
    const auto key = [&](const char* name) -> std::uint32_t {
        const int vk = ini.get_int("Hotkeys", name, 0);
        if (vk <= 0 || vk >= 255) return 0;
        // The boxes take nothing else; a file edited by hand gets no more --
        // and is told why, or a key that does nothing looks like a fault.
        if (one_job_key_allowed(static_cast<std::uint32_t>(vk))) return static_cast<std::uint32_t>(vk);
        GB_WARN("Hotkeys: [Hotkeys] %s=%d in app.ini is not an F-key from F1 to F12 -- a letter, digit or sign "
                "there is typed into the game's chat too -- so it is not used; pick F1 to F12 on the tab",
                name, vk);
        return 0;
    };
    trash_now = key("trash_now");
    login_now = key("login_now");
}

bool HotkeyConfig::save_key(const std::string& path, const char* key, std::uint32_t vk) {
    return IniFile::set_in_place(path, "Hotkeys", key, static_cast<int>(vk < 255 ? vk : 0));
}

const NamedKey* key_taken(std::uint32_t vk, const std::vector<NamedKey>& taken) {
    if (vk == 0) return nullptr;
    for (const NamedKey& k : taken)
        if (k.vk == vk) return &k;
    return nullptr;
}

const char* own_key_use_vi(std::uint32_t got, std::uint32_t mine, const HotkeySet& own) {
    if (got == 0 || got == mine) return nullptr;
    if (got == own.toggle || got == own.panic || got == own.hunt) return "bật/tắt bot";
    if (got == own.trash_now) return "dọn túi ngay";
    if (got == own.login_now) return "vào game ngay";
    return nullptr;
}

} // namespace gobot
