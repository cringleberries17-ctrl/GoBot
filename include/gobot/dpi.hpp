#pragma once

// Windows' display scale, and the pixels GoBot works in.
//
// GoBot reads the game off the screen pixel for pixel and clicks where it read
// something. Above 100% display scale Windows scales every program that does
// not say it handles DPI itself: it is shown window sizes and screen pixels
// resampled to what it would see at 100%. GoBot was such a program, so at 125%
// every frame it read came back resampled and every size scaled, the pictures
// the readers look for matched nothing, and HP, monsters, dialogs and clicks
// all failed together. So GoBot works in real screen pixels everywhere, and
// only its own window is left to Windows' scaling, so it looks as it always has.
//
// A game that does not handle DPI itself is enlarged by Windows the same way:
// drawn at 1280x720 and shown at 1600x900, blurred. No reader can work on that.
// window_scaling() tells; the per-program override below is the cure, as the
// Compatibility tab's "Override high DPI scaling behaviour: Application".
//
// Windows that draw themselves for GoBot -- VTCZone and the game's launcher,
// which PrintWindow can picture -- draw at their own size, which is the size
// of their pictures. GoBot looks at those and clicks them in their own pixels
// (DpiScope with the window), as it did before it saw real ones.

#include <string>

namespace gobot {

/// This process sees real screen pixels (per-monitor DPI aware), first thing in
/// main(), before any window exists. Only where Windows can also let one
/// thread see scaled pixels again -- Windows 10 1607 and later -- because
/// GoBot's own window is laid out in the pixels Windows scales for it; on an
/// older Windows nothing is changed. Per-monitor v2 from Windows 10 1703, and
/// the first per-monitor way on 1607, which has no v2. Runs once; true when
/// the threads GoBot starts see real pixels -- false as well when Windows held
/// the process to a way of its own before main(): a high-DPI override on this
/// program's Compatibility tab.
bool become_dpi_aware();

/// How large the main screen looked, at start, to the threads GoBot starts:
/// 100 when they see real pixels, or when Windows does not scale them. Over
/// 100 Windows hands them every size scaled and every frame resampled, and no
/// reader works -- before Windows 10 1607 at a display scale over 100%, and
/// when become_dpi_aware() was refused. Call that first.
int threads_view_percent();

/// Whether this Windows can give GoBot real pixels while its own window stays
/// scaled (10 1607 and later). When it can and threads_view_percent() is over
/// 100, something held GoBot to Windows' scaling.
bool real_pixels_possible();

/// For as long as it lives, the calling thread sees real screen pixels, or the
/// scaled ones a program that does not handle DPI is shown, or the ones a given
/// window sees itself in; the thread's own way is put back after. Nothing is
/// changed where Windows cannot do it.
class DpiScope {
public:
    enum class Mode { RealPixels, Scaled };
    explicit DpiScope(Mode mode);
    /// `window`'s own pixels: real ones for a window that handles DPI, the
    /// scaled ones for one that does not -- so that a picture it draws of
    /// itself (PrintWindow) and a click worked out from that picture are in the
    /// same pixels. Nothing changes for a null window.
    explicit DpiScope(void* window);
    ~DpiScope();
    DpiScope(const DpiScope&) = delete;
    DpiScope& operator=(const DpiScope&) = delete;

private:
    void* previous_ = nullptr;   ///< the context to put back; null when none was changed
};

/// The same for good, for a thread that keeps one way: the GUI thread that owns
/// GoBot's window.
void set_thread_dpi(DpiScope::Mode mode);

/// Whether the calling thread sees real screen pixels now.
bool thread_sees_real_pixels();

/// A window's client area in real screen pixels, and as the window itself
/// sees it. The two differ when Windows enlarges a window that does not handle
/// DPI itself -- the game at 125% display scale -- and GoBot cannot read a game
/// shown like that.
struct WindowScaling {
    bool known = false;     ///< false: no window, or a Windows too old to say
    int  real_w = 0, real_h = 0;
    int  own_w = 0, own_h = 0;
    int  percent = 100;     ///< how large Windows shows it: real over own
    /// Its program handles DPI itself, by its own say or by the override below.
    /// Such a window is still scaled on a screen whose scale is not the one it
    /// drew for: the main screen's, as it was when the program started -- or,
    /// as Windows may keep it, when Windows was signed in to.
    bool handles_dpi = false;
    bool on_main_screen = true;   ///< most of it is on the main screen
    /// The screen most of it is on has the main screen's scale -- the one the
    /// override has a program draw at -- or that is not known.
    bool screen_like_main = true;

    bool stretched() const { return known && (real_w != own_w || real_h != own_h); }
};
WindowScaling window_scaling(void* hwnd);

/// The scale a window draws itself at, in percent: 100 for one that does not
/// handle DPI, whatever Windows then shows it at; its screen's (or the main
/// screen's) for one that does. 0 when Windows will not say (before 10 1607).
int drawn_at_percent(void* hwnd);

/// What would have Windows show a window pixel for pixel.
enum class ScaleCure {
    None,              ///< it is shown so already, or nothing is known
    SetOverride,       ///< it does not handle DPI: its program's override, then start it again
    SetOverrideOrMove, ///< the same, on another screen with the main one's scale: or a screen at 100%
    RestartGame,       ///< the override is set, but the program was started before it was
    OverrideIgnored,   ///< the override is set and the program started since, yet it does not take
    MoveToMainScreen,  ///< on a screen whose scale is not the main one's, which no override cures
    RestartOrSignIn,   ///< it handles DPI, drawn for a scale the main screen had before
};
/// `overridden`: whether the window's program has the override
/// (has_high_dpi_override); `started_since`: whether the program was started
/// after the override was set.
ScaleCure cure_for(const WindowScaling& s, bool overridden, bool started_since = false);
/// The same for a window, asking the registry and the window's program.
ScaleCure cure_for(void* hwnd, const WindowScaling& s);
/// The cure in a clause, for the log, gobot_cli and the Capture tab.
const char* cure_advice(ScaleCure cure);

/// The program a window belongs to: its full path in UTF-8, or empty when
/// Windows will not say.
std::string window_program(void* hwnd);

/// This program's own exe, by its full path in UTF-8.
std::string own_exe_path();

/// Where Windows keeps each program's compatibility settings for this user,
/// the Compatibility tab's among them, by the program's full path. Read when
/// the program starts.
inline constexpr const char* kCompatLayersKey =
    "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";

/// A program's compatibility value with the high-DPI override in it, as the
/// Compatibility tab writes "Override high DPI scaling behaviour: Application":
/// HIGHDPIAWARE added, the other two choices there (DPIUNAWARE, GDIDPISCALING)
/// taken out, every other setting kept, and "~" first.
std::string with_high_dpi_override(const std::string& layers);

/// Whether `exe` has the override.
bool has_high_dpi_override(const std::string& exe, const char* key = kCompatLayersKey);

/// Where GoBot notes when it set a program's override (REG_QWORD, a FILETIME,
/// by the program's full path): a game started since is told from one started
/// before by it, as the Layers key's own time moves with every program's.
inline constexpr const char* kDpiStampKey = "Software\\GoBot\\HighDpiOverrideSet";

/// Gives `exe` the override: Windows no longer enlarges it, and it draws at
/// its own size, pixel for pixel. It takes effect when the program is started
/// again. False, with why, when the registry refused.
bool set_high_dpi_override(const std::string& exe, std::string* why = nullptr,
                           const char* key = kCompatLayersKey, const char* stamp_key = kDpiStampKey);

/// A program's compatibility value with all three of the tab's high-DPI
/// choices taken out and every other setting kept: empty when nothing else is
/// left.
std::string without_dpi_override(const std::string& layers);

/// Whether `exe` has any of those choices, for this user or for all users.
bool has_any_dpi_override(const std::string& exe, const char* key = kCompatLayersKey);

/// Takes them out, for this user and for all users -- GoBot's own exe, held by
/// one to Windows' scaling. It takes effect when the program is started
/// again. False, with why, when the registry refused.
bool clear_dpi_override(const std::string& exe, std::string* why = nullptr,
                        const char* key = kCompatLayersKey);

} // namespace gobot
