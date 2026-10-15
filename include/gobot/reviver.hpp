#pragma once

#include "gobot/input_manager.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gobot {

/// Revives a dead character on the spot, with a revive item, the way GoC does.
///
/// When the character dies the game puts up its death panel, "Trạng thái bất
/// tỉnh", with three buttons: revive in the village (it costs 2% EXP), use a
/// revive item, and revive at once for Cash. Only the middle one is ever
/// pressed. Then the game asks "Bạn có muốn sử dụng giấy hồi sinh và bùa hồi
/// sinh không?" and Xác Nhận is pressed on that. Nothing else is clicked, ever:
/// not the village, not Cash, not the Khóa ticks under them, and nothing at all
/// while one of the trash bin's dialogs is up -- the revive question's picture
/// is close enough to the bin's that an answer meant for one could go to the
/// other, and the bin's Xác Nhận deletes an item.
///
/// The panel is found as GoC finds it: by two 16 pixel corners cut from the
/// panel's own picture, the top-left corners of the village button and the
/// item button, the second 121-126 pixels right of the first. Corners carry no
/// words, so they match the Vietnamese panel and the English one alike, and the
/// pair is never seen on a screen without the panel. The whole picture of the
/// Vietnamese panel is the fallback.
///
/// The question is answered only at a death: once the panel has been seen at
/// it. Every [Xác Nhận][Hủy Bỏ] dialog has the same frame, and one that opens
/// with the same words -- a question about using an item the player clicked --
/// matches the revive question's picture nearly as well; with nobody dead it
/// is left alone. GoC, too, only looks for the OK after it has pressed the
/// panel's item button.
///
/// The first click of an attempt is aimed from a look taken after whoever runs
/// this has let it go ahead and the game is in front: the look that decided it
/// may be from before a wait for the mouse, and a trash pass that held it may
/// have left the bin's own question where the revive's was.
///
/// Three tries for each death, as GoC's player_revive_max_attempts. After that
/// nothing more is clicked until the character is seen alive again; the
/// player's rule is that the bot stops then rather than revive in the village,
/// and `on_out_of_tries` is where whoever runs this hears it. A revive counts
/// once the panel and the question have stayed away -- for a moment when HP
/// reads above 0, for kSettleBlindMs when it cannot be read -- and its try is
/// only given back once the character has stayed up for kAliveMs: a panel that
/// comes back at once is the same death, so a game that only hides the panel
/// for a moment when there is no revive item cannot keep it trying for ever.
///
/// Everything it does to the game goes through a grab, a click and a clock,
/// each replaceable, so the whole of it is tried on a pretend game in the
/// tests -- the same way BagCleaner is.
class Reviver {
public:
    using Grab = std::function<bool(Frame&)>;
    using Report = std::function<void(const std::string&)>;

    // ---- what is measured ----

    /// GoC's anchors: 16x16 corners of the panel's picture
    /// (inven/0/dialog_Panic.spr/0), at the village button and at the item
    /// button. Both have to match this well, the second this far from the
    /// first.
    static constexpr int   kAnchorSize = 16;
    static constexpr int   kAnchor1X = 36, kAnchor1Y = 61;
    static constexpr int   kAnchor2X = 159, kAnchor2Y = 61;
    static constexpr float kAnchorScore = 0.95f;
    static constexpr int   kAnchorDxMin = 121, kAnchorDxMax = 126;
    static constexpr int   kAnchorDyMin = -2, kAnchorDyMax = 3;
    /// "Sử dụng vật phẩm hồi sinh", from its top-left corner -- the second
    /// anchor.
    static constexpr int   kItemW = 105, kItemH = 42;
    /// The fallback: the player's own panel (grabbed/chardeadpanel), and
    /// where the village and item buttons sit in it. It is looked for round
    /// the few places the village corner matched best, as long as that was
    /// this well, the pixels of slack either side -- so a panel whose corners
    /// match a little worse is still found, without matching a picture that
    /// size over the whole screen on every look.
    static constexpr float kPanelScore = 0.90f;
    static constexpr int   kPanelVillageX = 34, kPanelItemX = 157, kPanelItemY = 58;
    /// The Cash button's corner, which the village corner's picture matches
    /// at 0.985: with the village button lit under the pointer, the strongest
    /// place left, so the whole panel is looked for round it as well.
    static constexpr int   kPanelCashX = 280;
    static constexpr float kCornerNear = 0.85f;
    static constexpr std::size_t kCornerPlaces = 4;
    static constexpr int   kPanelSlack = 8;

    /// The revive question (grabbed/hoisinh_confirm, 198x81). The bin's
    /// question scores 0.85 against it on the live client and the drop
    /// question 0.87, so it is held well above both. Xác Nhận is at this
    /// offset from its top-left, and its pair of buttons at the next.
    static constexpr float kQuestionScore = 0.93f;
    static constexpr int   kConfirmDx = 123, kConfirmDy = 65;
    static constexpr int   kPairDx = 103, kPairDy = 55;

    /// How long one attempt waits: for the question after the item button,
    /// and for the panel and the question to go after the last click.
    static constexpr int kQuestionWaitMs = 3000;
    static constexpr int kReviveWaitMs = 10000;
    /// How long the panel and the question have to stay away to count as a
    /// revive when HP cannot be read -- the HUD hidden, or no reader running.
    /// With HP read above 0, two looks running are enough.
    static constexpr int kSettleBlindMs = 3000;
    /// HP in percent under this reads as nothing left: still dead. The same
    /// line the GUI draws for "HP at 0".
    static constexpr float kHpZero = 0.5f;
    /// How often it looks meanwhile.
    static constexpr int kLookMs = 250;
    /// How far the place to click may have moved between the look that
    /// decided an attempt and the one taken just before its first click.
    static constexpr int kAimSlack = 3;
    /// Seen alive this long and the next death is a new one, with its own
    /// three tries. Longer than a flicker of the panel during an attempt.
    static constexpr int kAliveMs = 5000;
    /// Tries per death.
    static constexpr int kAttempts = 3;

    /// One look at the game, as far as reviving goes.
    struct Seen {
        bool       panel = false;        ///< the death panel is up
        bool       by_anchors = false;   ///< found by GoC's two corners, not by its picture
        float      panel_score = 0.0f;   ///< the weaker corner's score, or the picture's
        ScreenRect item;                 ///< "Sử dụng vật phẩm hồi sinh"
        bool       question = false;     ///< the revive question is up, and it is not the bin's
        ScreenRect question_at;          ///< its picture
        /// How alike the best place was to the question's picture, taken or
        /// not -- for saying how near a dialog came.
        float      question_score = 0.0f;
        bool       bin_asking = false;   ///< one of the bin's dialogs is up: hands off
        /// Other dialogs with [Xác Nhận][Hủy Bỏ]: never answered, and nothing
        /// is clicked where one of them lies.
        std::vector<ScreenRect> others;

        /// The panel or the question up, as one look sees it. A turn counts
        /// the question alone as a death only once the panel has been seen
        /// at it.
        bool dead() const { return panel || question; }
        int  item_x() const { return item.x + item.w / 2; }
        int  item_y() const { return item.y + item.h / 2; }
        int  confirm_x() const { return question_at.x + kConfirmDx; }
        int  confirm_y() const { return question_at.y + kConfirmDy; }
    };

    /// What one turn came to.
    enum class Turn {
        Blind,      ///< the game could not be looked at; nothing done
        Alive,      ///< no panel and no question up
        Waiting,    ///< dead, and nothing may be clicked now: the bin's dialog, or another over the button
        Revived,    ///< clicked, and the panel and the question went
        Failed,     ///< clicked, and the character is still dead
        OutOfTries, ///< every try at this death was used: nothing more is clicked
        Stopped,    ///< asked to stop
    };
    static const char* turn_name(Turn t);

    /// What the turn holds whoever runs it to: `take` just before the first
    /// click of an attempt -- for holding everything else off, the mouse
    /// included -- and `done` after its last. False from `take` puts the
    /// attempt off: nothing is clicked and no try is counted.
    struct Guard {
        std::function<bool()> take;
        std::function<void()> done;
    };

    /// What the reviver does to the game: a click at a client point. The
    /// game's own mouse unless set; setting it also skips the check that the
    /// game is in front, which a test has no game for.
    struct Hands {
        std::function<bool(int, int)> click;
    };

    /// The time as the reviver reads it, and its waits.
    struct Clock {
        std::function<std::chrono::steady_clock::time_point()> now;
        std::function<void(int)>                                 sleep;   ///< milliseconds
    };

    Reviver(InputManager& input, const TemplateStore& store, Grab grab);

    /// Whether the pictures it needs are loaded: the panel's (GoC's sprite
    /// for the corners, or the player's own), the question's, and the bin's
    /// dialogs to tell them from it. `why` gets the Vietnamese when not.
    bool ready(std::string* why = nullptr) const;

    /// One look, deciding nothing and clicking nothing. For the worker's
    /// tests and for `gobot_cli --routine revive --dry`. A turn looks the
    /// same way, except that it looks for the question only at a death --
    /// the panel up, or seen at it -- since that is the only time it is
    /// answered: twice the time of the rest of a look, every 1.2 s all night.
    Seen look(const Frame& f) const { return see(f, true); }

    /// One turn: a fresh look and, when the character is dead and nothing
    /// forbids it, one attempt -- Xác Nhận on the question when it is up;
    /// otherwise the item button, the question waited for up to 3 s and
    /// Xác Nhận on it; then up to 10 s for the panel and the question to go,
    /// and HP to read above 0 where it can be read (where it cannot, for them
    /// to stay gone for kSettleBlindMs).
    Turn turn();

    void set_guard(Guard g) { guard_ = std::move(g); }
    void set_hands(Hands h) { hands_ = std::move(h); }
    void set_clock(Clock c) { clock_ = std::move(c); }
    /// HP in percent, or below 0 when it cannot be read: the vitals reader's.
    void set_hp(std::function<float()> hp) { hp_ = std::move(hp); }
    /// Asked between every wait: true stops the turn under way.
    void set_stop(std::function<bool()> stop) { stop_ = std::move(stop); }
    void cancel() { cancelled_ = true; }

    /// Lines for the log, in English, as it goes.
    void on_report(Report r) { report_ = std::move(r); }
    /// Called once for each revive.
    void on_revived(std::function<void()> f) { revived_hook_ = std::move(f); }
    /// Called once for each death whose tries ran out, with the reason in
    /// English.
    void on_out_of_tries(std::function<void(const std::string&)> f) { out_hook_ = std::move(f); }

    /// Where to keep the frame of an attempt that did not take, the newest
    /// kKeptFrames of them. Empty keeps none.
    void set_debug_dir(const std::string& dir) { debug_dir_ = dir; }
    static constexpr std::size_t kKeptFrames = 20;

    /// Tries used at this death -- and at the last revive, until the
    /// character has stayed up for kAliveMs.
    int  tries() const { return tries_; }
    int  revived() const { return revived_; }        ///< since this was made
    bool out_of_tries() const { return tries_ >= kAttempts; }

    /// The Vietnamese said when the tries ran out: why the bot stopped.
    static std::string out_of_tries_vi();

private:
    /// Look until `want` says yes or `ms` runs out; `last` gets the last look.
    /// False when stopped or blind for the whole of it.
    bool watch(int ms, const std::function<bool(const Seen&)>& want, Seen& last, Frame& frame);
    bool fresh(Frame& f, Seen& s);
    /// The look: the question looked for when the panel is up, or always
    /// with `question_anyway`.
    Seen see(const Frame& f, bool question_anyway) const;
    /// Whether a look shows a death: the panel, or the question while the
    /// panel has been seen at this death.
    bool dead_at(const Seen& s) const { return s.panel || (s.question && death_on_); }
    /// Where an attempt would click on this look -- Xác Nhận when the
    /// question is up, else the item button -- or false, with why not: the
    /// bin's dialog up, or another dialog over the button.
    static bool aim(const Seen& s, int frame_w, int frame_h, int& x, int& y, bool& on_question,
                    std::string& why_not);
    /// Every try at this death used: says so, and calls `on_out_of_tries`,
    /// once for the death.
    Turn give_up();
    /// Click a client point of the game; false, and `why_not` says why, when
    /// the click did not go out.
    bool click(int x, int y, const char* what, std::string* why_not = nullptr);
    bool mouse_ok(int x, int y, std::string* why_not);
    void bring_forward();
    bool stop_now() const;
    bool wait_ms(int ms);
    std::chrono::steady_clock::time_point now() const;
    void sleep(int ms);
    void say(const std::string& line) const;
    void keep(const Frame& f);
    /// Whether a point lies on one of the other dialogs, or off the frame.
    static bool in_the_way(const Seen& s, int x, int y, int w, int h);

    const Frame* picture(const char* name, const char* path) const;

    InputManager&        input_;
    const TemplateStore& store_;
    Grab                 grab_;
    Guard                guard_;
    Hands                hands_;
    Clock                clock_;
    std::function<float()> hp_;
    std::function<bool()>  stop_;
    std::atomic<bool>    cancelled_{false};
    Report               report_;
    std::function<void()> revived_hook_;
    std::function<void(const std::string&)> out_hook_;
    std::string          debug_dir_;

    /// GoC's two corners, cut once from the panel's picture; empty without it.
    Frame anchor1_, anchor2_;

    int  tries_ = 0;
    int  revived_ = 0;
    bool said_waiting_ = false;
    /// The panel has been seen at this death: the question may be answered.
    bool death_on_ = false;
    /// `on_out_of_tries` has been called for this death.
    bool gave_up_ = false;
    /// When the character was first seen alive since it was last seen dead.
    std::chrono::steady_clock::time_point alive_since_{};
    bool alive_seen_ = false;
};

} // namespace gobot
