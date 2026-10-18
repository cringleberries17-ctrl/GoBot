#pragma once

#include "gobot/config.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gobot {

/// GoBot's own keys, watched by GoBot itself while the game has the keyboard:
/// start/stop, hunting, the panic stop, and the two that do one job at once --
/// "Dọn túi ngay" and "Vào game ngay".
///
/// Watched with GetAsyncKeyState rather than registered with RegisterHotKey,
/// which GoC does: a registered key is swallowed and never reaches the game,
/// and a key the player shares with the game would stop working there. The
/// price is that the key goes to whatever window is in front as well, so what
/// each key does depends on which window that is. GoC checks it only for its
/// shopping key ("[HOTKEY] Ignoring: Game window not focused"); here every key
/// but the panic stop checks it.
///
/// - Start/stop and hunting act with the game or GoBot's own window in front,
///   and never while a key box on the simple screen is waiting for a key.
/// - The two one-job keys act only with the game in front: they are meant to
///   be pressed while playing, and a key typed into a browser must not empty
///   the bag or start the game.
/// - The panic stop always acts. Being able to stop everything from anywhere
///   is the point of it.
///
/// Everything here decides; nothing presses or starts anything. The window's
/// tick() reads the keys, asks hotkey_action, and does what it says.

/// Which window is in front when a key goes down.
enum class HotkeyFront {
    None,       ///< nothing, or a window that could not be told
    Game,       ///< the game's own window
    GoBot,      ///< GoBot's window
    Other,      ///< anything else: a browser, a chat, the desktop
};

/// The keys, as virtual-key codes. 0 is no key: it never goes down.
struct HotkeySet {
    std::uint32_t toggle = 0;       ///< BẮT ĐẦU / DỪNG LẠI, F10 unless changed
    std::uint32_t hunt = 0;         ///< hunting on and off, F11
    std::uint32_t panic = 0;        ///< stop everything, F12
    std::uint32_t trash_now = 0;    ///< "Dọn túi ngay": one trash pass now
    std::uint32_t login_now = 0;    ///< "Vào game ngay": one login crossing now
};

/// Which keys went down since the last look. Only the going down: a key held
/// for three seconds is one press, not a hundred and eighty.
struct HotkeyEdges {
    bool toggle = false;
    bool hunt = false;
    bool panic = false;
    bool trash_now = false;
    bool login_now = false;

    bool any() const { return toggle || hunt || panic || trash_now || login_now; }
};

/// Turns the keys' up and down, looked at on every tick, into edges.
class HotkeyEdgeTracker {
public:
    /// `down(vk)` says whether a key is held now; it is not asked about a key
    /// of 0. A key changed while it is held goes down again only once it has
    /// been seen up.
    HotkeyEdges update(const HotkeySet& keys, const std::function<bool(std::uint32_t)>& down);

private:
    bool was_[5] = {};
};

/// Whether the character is in the world, as far as the window can tell
/// without a look of its own: the HP reader's word, or nothing.
enum class HotkeyWorld { Unknown, Yes, No };

/// What the rest of GoBot is doing, as far as the keys care.
struct HotkeyState {
    HotkeyFront front = HotkeyFront::None;
    /// A key box on the simple screen is waiting for a key. The key then
    /// belongs to the box: binding a key must not also start the bot.
    bool listening = false;
    /// A hold of GoBot's own is on -- a chat line typed, a quest round, a
    /// trash pass. GoBot presses keys then, and GetAsyncKeyState cannot tell
    /// its presses from the player's: a chat line with a "k" in it would be
    /// the K key going down. So the two one-job keys go unheard meanwhile.
    bool held = false;

    // "Dọn túi ngay" has the "Dọn ngay" button's gates.
    bool trash_busy = false;        ///< a pass runs, or one has been asked for
    bool chat_doubt = false;        ///< the game's chat line may be open
    bool game_window = false;       ///< the game's window is known
    /// The trash list has been read and holds nothing. False when it has not
    /// been read: the worker reads it itself, and says so when it is empty.
    bool trash_list_empty = false;

    // "Vào game ngay".
    HotkeyWorld world = HotkeyWorld::Unknown;
    bool crossing = false;          ///< a login crossing is under way, whoever started it
    bool key_crossing = false;      ///< and this key started it

    /// What GoBot itself presses the trash key or the login key for, in
    /// Vietnamese -- "phím kỹ năng", "bình HP", "bật/tắt bot" -- or empty when
    /// nothing: a key set to one of GoBot's own in the full screen's macro
    /// table, or by hand in a file, neither of which checks the way the key
    /// boxes do. Every press of GoBot's own goes down as the player's would,
    /// so such a key is not heard at all.
    std::string trash_key_use;
    std::string login_key_use;
};

/// What to do about one tick's edges.
struct HotkeyActions {
    bool toggle = false;            ///< as BẮT ĐẦU / DỪNG LẠI
    bool hunt = false;              ///< hunting on or off
    bool panic = false;             ///< stop everything, let go of every key
    bool trash = false;             ///< one trash pass now, as "Dọn ngay"

    enum class Login {
        None,
        Start,      ///< one crossing now
        Cancel,     ///< stop the crossing this key started, as GoC's F8 does
    };
    Login login = Login::None;

    /// Why a key that was meant for the game did nothing, in Vietnamese, for
    /// the screen. Empty when it acted, or when it was not meant for the game
    /// at all -- pressed with another window in front, which is no mistake.
    std::string trash_note_vi;
    std::string login_note_vi;

    /// The trash key or the login key went down, and it is one GoBot presses
    /// itself (HotkeyState::trash_key_use, login_key_use): nothing was done
    /// for it. The window says why, with hotkey_text::key_taken_vi.
    bool trash_taken = false;
    bool login_taken = false;
};

HotkeyActions hotkey_action(const HotkeyEdges& edges, const HotkeyState& s);

/// The words the notes use, so the tab and the tests say the same thing.
namespace hotkey_text {
inline const char* const kTrashBusy = "Đang dọn rác rồi.";
inline const char* const kTrashChat = "Ô chat trong game có thể đang mở: chưa dọn rác được cho đến "
                                      "khi ô chat đóng (xem thẻ Tự chat).";
inline const char* const kTrashNoGame = "Chưa thấy cửa sổ game.";
inline const char* const kTrashEmpty = "Danh sách đồ bỏ đi đang trống, nên không có gì để dọn.";
inline const char* const kInWorld = "Nhân vật đã ở trong game.";
inline const char* const kCrossing = "GoBot đang tự vào lại game.";
/// "Phím F5 cũng là phím GoBot tự bấm (phím kỹ năng), nên không dùng làm phím
/// dọn ngay được. Hãy chọn phím khác." -- `key` as the screen writes it, `use`
/// what GoBot presses it for, `trash` which of the two keys it is.
std::string key_taken_vi(const std::string& key, const std::string& use, bool trash);
} // namespace hotkey_text

/// Whether `vk` may be one of the two one-job keys: no key, or a function key,
/// F1 to F12 -- what the key boxes and their notes ask for, and what a keyboard
/// has. Never a letter, a digit, Space, punctuation or a numpad key: those
/// are text, and with the game's chat line open the player typing that
/// character would be pressing the key as well -- a trash pass started mid
/// sentence, its bag key typed into the line. GoC's own are F8 to F11.
bool one_job_key_allowed(std::uint32_t vk);

/// The two one-job keys, from [Hotkeys] in configs/app.ini, as key codes.
/// Both ship with no key: a key that acts on the game is the player's to give.
struct HotkeyConfig {
    std::uint32_t trash_now = 0;
    std::uint32_t login_now = 0;

    /// A missing key, one that is no key code (0, or above 254), or one that
    /// one_job_key_allowed refuses -- a letter written into the file by hand --
    /// is none.
    void load(const IniFile& ini);
    /// One key written into `path` with IniFile::set_in_place, so the rest of
    /// the file -- its comments above all -- stays as it is.
    static bool save_key(const std::string& path, const char* key, std::uint32_t vk);
};

/// A key and what it is already used for, in Vietnamese: "bình HP", "đánh".
struct NamedKey {
    std::uint32_t vk = 0;
    std::string   what_vi;
};

/// What `vk` is already used for among `taken`, or null when it is free. A
/// key of 0 is never taken, and never takes anything.
const NamedKey* key_taken(std::uint32_t vk, const std::vector<NamedKey>& taken);

/// Which of GoBot's own keys `got` is -- start/stop, the panic stop, hunting,
/// and the two one-job keys -- as the simple screen's key boxes refuse them
/// as game keys: "bật/tắt bot", "dọn túi ngay", "vào game ngay". Null when it
/// is none of them, or is `mine`, the box's own key. GoBot watches its own
/// keys with GetAsyncKeyState, which cannot tell its own press of one from the
/// player's: a potion key that was the panic key would stop the bot every
/// time HP ran low.
const char* own_key_use_vi(std::uint32_t got, std::uint32_t mine, const HotkeySet& own);

} // namespace gobot
