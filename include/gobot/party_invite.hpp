#pragma once

#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/party.hpp"
#include "gobot/react_wait.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gobot {

class InputManager;   // gobot/input_manager.hpp

/// Somebody has asked you to join their party, and there is nobody there to
/// answer.
///
/// The client draws it as a small modal at the bottom of the screen:
///
///     +---------------------------------------+
///     |        Yêu cầu của ExampleWin.        |
///     |                [Xác Nhận] [Hủy Bỏ]    |
///     +---------------------------------------+
///
/// "A request from ExampleWin", confirm or cancel. It is up for about ten
/// seconds and then it is gone, which is why an unattended bot misses every
/// one of them and why this had to be caught on a capture loop rather than
/// photographed at leisure.
///
/// Two things are read rather than assumed.
///
/// The dialog is found by its FOOTER -- the two buttons and the border beside
/// them -- not by the whole box. The box contains the inviter's name, so a
/// picture of the whole thing only ever matches the one player it was taken
/// from. The footer is the part that does not change.
///
/// The name is then read out of the line above, with the game's own bitmap
/// font, and checked against the rules before anything is clicked. That is the
/// difference between accepting invitations and accepting everything: the same
/// modal shape carries other requests, and a bot that presses the confirm
/// button on whatever appears will eventually confirm something else.
class PartyInvite {
public:
    struct Settings {
        /// The footer picture, and how well it must match.
        std::string picture = "grabbed/party_invite_buttons";
        float       score = 0.80f;

        /// The two buttons, from the picture's top-left corner.
        int accept_dx = 31,  accept_dy = 23;
        int decline_dx = 73, decline_dy = 23;

        /// Where the line of text sits, from the same corner. Negative,
        /// because the text is up and to the left of the footer.
        int name_dx = -82, name_dy = -26;
        int name_w = 182,  name_h = 30;

        /// What comes before the name on that line. Everything after it, up to
        /// the full stop, is the player.
        std::string prefix = "c\xE1\xBB\xA7""a";     // "của"

        /// Accepting takes a click; this is the pause before it, so the dialog
        /// has settled and so two invitations in a row are not both answered
        /// inside the same frame.
        int settle_ms = 400;

        void load(const IniFile& ini);
        void save(IniFile& ini) const;
    };

    /// What was on the screen.
    struct Seen {
        bool        up = false;
        ScreenRect  at;          ///< where the footer matched
        float       score = 0.0f;
        std::string line;        ///< the whole line as read
        std::string who;         ///< the name pulled out of it

        /// Where to click, in client coordinates. Only meaningful when `up`.
        int accept_x = 0, accept_y = 0;
        int decline_x = 0, decline_y = 0;
    };

    /// What to do about a dialog, and why, in words for the log.
    enum class Answer { LeaveAlone, Decline, Accept };
    struct Verdict {
        Answer      answer = Answer::LeaveAlone;
        std::string why;
    };

    /// The decision and nothing else, by the GUI's party worker's own rules.
    ///
    /// The footer picture is the two buttons every dialog has, so never one of
    /// the trash bin's dialogs -- Xác Nhận there deletes an item -- and never a
    /// dialog whose line names nobody, whatever accept_all says. The name is
    /// checked whenever a rule turns on it. `rules.auto_accept` is not looked
    /// at: whether to answer at all is the caller's.
    static Verdict decide(const Seen& seen, bool bin_asking, const Party::Settings& rules);

    /// Whether the look taken just before the click still shows the same
    /// invitation from the same player: up, not the bin's, the same name read
    /// on both, and the footer within a few pixels of where it was.
    static bool same_invitation(const Seen& first, const Seen& again, bool bin_asking_again);

    /// Answer `seen`: wait settle_ms when accepting, look again through
    /// `grab`, and click only if that look shows the same invitation. Needs
    /// the font, because the second look compares names. Never brings the
    /// game forward -- that is the caller's, which knows whether it may take
    /// the foreground. `stop` is asked through the pause and by the click.
    /// True when the click went out; `why`, when given, says why not.
    bool answer(InputManager& input, const std::function<bool(Frame&)>& grab,
                const TemplateStore& store, const GlyphFont* font, const Seen& seen,
                bool accept, const std::function<bool()>& stop = {},
                std::string* why = nullptr) const;

    void set_settings(const Settings& s) { settings_ = s; }
    Settings settings() const { return settings_; }

    /// Look at one frame. `font` may be null, in which case the dialog is
    /// still found but the name comes back empty -- and a caller that cannot
    /// read the name should not be accepting on a whitelist.
    Seen look(const Frame& frame, const TemplateStore& store,
              const GlyphFont* font) const;

    /// Pull the player's name out of the line.
    ///
    /// "Yêu cầu của ExampleWin." -> "ExampleWin". Takes what follows the
    /// prefix and drops a trailing full stop. Returns "" when the line does
    /// not have the shape -- which is the right answer for a modal that is not
    /// this one, and the reason the prefix is checked at all.
    static std::string name_in(const std::string& line, const std::string& prefix);

    /// Lowercase, strip the accents, drop the spaces.
    ///
    /// For comparing text that was read off a screen against text somebody
    /// typed into a config file. The client writes "Yêu cầu của"; a config
    /// file has to be editable by somebody who cannot type ủ; and the bitmap
    /// reader gets a mark wrong far more often than it gets the letter wrong.
    static std::string fold(const std::string& text);

private:
    Settings settings_;
};

/// The game's answer to an invite when it comes as a message of its own: the
/// client's one message box, only Xác Nhận under it (bag::lone_ok) --
/// "Đối phương đã hủy yêu cầu tham gia nhóm." (grabbed/invite_fail2), and the
/// one the owner met inviting somebody already in a party (2026.11.14b: "nếu
/// chẳng may pt trúng người đã vào pt thì sẽ bị hiện dialog mà không tự động
/// tắt => chỉ cần ấn enter hoặc click xác nhận trong dialog"). Closed with its
/// own Xác Nhận, clicked -- never a key: Enter opens the game's chat line, and
/// whatever is typed next would go into it.
///
/// Its hands are the caller's, so that it can be tried with no game: a look,
/// a click on the game (click_on_game with the caller's stop), a move of the
/// pointer, a wait that says false once stopped, and the stop itself.
struct InviteReply {
    struct Hands {
        std::function<bool(Frame&)>   grab;
        std::function<bool(int, int)> click;
        std::function<void(int, int)> move;
        std::function<bool(int)>      nap;
        std::function<bool()>         stop;
        /// A clock in milliseconds (the review of 2026.11.14b): the watch and
        /// the wait for the box to go end by it as well as by the naps' total,
        /// so that slow looks -- a grab and a whole-screen search each, on a
        /// 1-CPU guest -- do not stretch them. Unset, the naps alone count.
        std::function<std::uint64_t()> now_ms;
    };
    /// None: no such message within the watch, nothing pressed. Closed: its
    /// Xác Nhận clicked and it seen gone on two looks. NotClosed: still up
    /// after kClicks clicks. NotClicked: the click was refused (the game not
    /// in front, covered). Stopped: a stop came first.
    enum class How { None, Closed, NotClosed, NotClicked, Stopped };
    struct Result {
        How         how = How::None;
        ScreenRect  label;          ///< its Xác Nhận's label, where lone_ok found it
        int         clicks = 0;
        int         seen_after_ms = 0;  ///< from the watch's start to its first look of it
        /// From the first click to the first look that saw it gone, when it
        /// was seen gone (Closed); -1 otherwise. For the log: a slow game's.
        int         gone_after_ms = -1;
        std::string words;          ///< its text, read with the font, for the log
        Frame       shot;           ///< the look it was taken on
    };
    /// Look every kLookMs, for `watch_ms` (by the naps, and by Hands::now_ms
    /// when set), for a lone Xác Nhận anywhere: taken
    /// only once two looks kLookMs apart find it within kSlack pixels. Then
    /// its words read (with `font`, when given), its Xác Nhận clicked in the
    /// middle, the pointer moved kParkPx under it so that it is not lit, and
    /// looked at until two looks kLookMs apart, within kFirstGoneMs, see
    /// neither it nor bag::notice_up. Once more on a fresh look that still
    /// finds its label, a pixel aside, watched kGoneMs; no third time. No
    /// key, ever.
    ///
    /// `first_gone_ms` and `clicks` (2026.11.17b): how long the first click
    /// is watched, and how many clicks this one watch makes. The party worker
    /// passes kCloseHoldMs and 1: the game's mouse is held all through the
    /// watch, and a message that stays up is clicked again on a later pass
    /// with the mouse let go in between (kCloseTries), not waited on here for
    /// kReactMs while every other job of GoBot's waits for the mouse.
    static Result watch(const TemplateStore& store, const GlyphFont* font, const Hands& hands, int watch_ms,
                        int first_gone_ms = kFirstGoneMs, int clicks = kClicks);
    static const char* how_name(How h);

    /// After an invite: invite() has waited for the menu to go already. Not a
    /// wait for the game's reaction to a click of GoBot's -- a message that
    /// often never comes (a good invite brings none) -- so not kReactMs: a
    /// late one is caught by kLateMs's looks, without the game's mouse held.
    static constexpr int kWatchMs = 2500;
    static constexpr int kLookMs = 150;
    static constexpr int kSlack = 3;
    /// How long the first click on its Xác Nhận is watched for the box to go:
    /// kReactMs (the owner, 2026-11-15: "sẽ có thêm 30 giây chờ phản ứng"),
    /// before the second click, which on a slow game would land on whatever
    /// came up under it. kGoneMs, as before, after the second.
    static constexpr int kFirstGoneMs = kReactMs;
    static constexpr int kGoneMs = 2500;
    static constexpr int kClicks = 2;
    static constexpr int kParkPx = 50;
    /// A message this soon after an invite is its answer (the invitee's own
    /// box lasts about ten seconds): looked for that long after one.
    static constexpr std::uint64_t kLateMs = 30'000;
    /// Whoever's invite brought one is held off by InviteTries since
    /// 2026.11.17b -- a minute, ten after two in a row -- not 2026.11.15b's
    /// 30 s: the owner chose the longer holds when an invite to somebody
    /// already in a party had the list opened again and again. A message
    /// that will not close stops the invites for kStuckMs.
    static constexpr std::uint64_t kStuckMs = 60'000;

    /// The party worker's watch after its click on the message's Xác Nhận
    /// (2026.11.17b, B): the game's mouse is held through it, and 30 s of it
    /// (kReactMs, as every other wait for the game has) left the other jobs
    /// standing -- the owner's "không phản ứng gì nữa". So a short look, about
    /// as long as kWatchMs, and the mouse let go.
    static constexpr int kCloseHoldMs = 2500;
    /// Clicks on one message in all, one a pass, kRetryGapMs apart at least,
    /// before the invites stop for kStuckMs: a slow game gets its time
    /// between them without the mouse held, and a message that will not go
    /// does not get clicked forever.
    static constexpr int kCloseTries = 3;
    static constexpr std::uint64_t kRetryGapMs = 1'000;
};

/// Who was invited, whether the invite led to a join, and who is held off
/// for one that did not (2026.11.17b, A). The owner's report: "khi pt người
/// đã có pt thì hiển thị thông báo sau đó bot không phản ứng gì nữa và cứ mở
/// khung pt" -- inviting somebody already in a party brought a notice, and the
/// list was opened again and again. Whether GoBot saw the notice or not, the
/// invite came to nothing, and the player asked again: so a player is held
/// off by what the invite led to, not by what the screen showed.
///
/// An invite is looked at again kJoinWaitMs after it went out, on the party
/// HUD of a later pass of the worker -- nothing waits for it: a member's
/// panel more than before the invite (PartyHud::size) is a join. The HUD
/// cannot name who joined, so two invites out at once share what it shows:
/// one more member counts for one of them only. A HUD that could not be
/// read, then or before the invite, cannot tell, and counts as no join.
/// No join -- or the game's message after the invite, which counts at once
/// -- holds that player off kFailHoldMs, and kFailHoldLongMs from the
/// kFailsForLong-th in a row; a join starts the count again.
///
/// Pure and window-free, on the caller's clock: the party worker hands it the
/// HUD's reading and the time, and holds the player off in PartyChat too
/// (hold_off), which is what keeps the list shut for them.
class InviteTries {
public:
    /// How long after an invite the party HUD is asked whether it led to a
    /// join: the invitee's own box is up about ten seconds, and the panel is
    /// drawn a moment after the answer.
    static constexpr std::uint64_t kJoinWaitMs = 15'000;
    /// A look grabbed before GoBot's last change to the party (PartyHud::
    /// stale) says nothing of it: waited past for up to this long more, a
    /// pass or two, then taken as it is.
    static constexpr std::uint64_t kStaleWaitMs = 10'000;
    /// The owner's choice: a minute after an invite that did not lead to a
    /// join, ten minutes once kFailsForLong have not in a row.
    static constexpr std::uint64_t kFailHoldMs = 60'000;
    static constexpr std::uint64_t kFailHoldLongMs = 600'000;
    static constexpr int kFailsForLong = 2;
    /// An asker not on the player list is sent no invite, so it is no miss:
    /// held off in the chat this long, the same every time, never counted --
    /// a friend still on the way, or a name the list misread, is not shut out
    /// for kFailHoldLongMs once there (review of 2026.11.17b).
    static constexpr std::uint64_t kNotListedHoldMs = 60'000;
    /// How alike two names have to be, folded, to be the same player: the
    /// chat's names and the list's are both read off the screen.
    static constexpr double kNameAlike = 0.7;

    /// The party HUD on one look: members seen (PartyHud::size; 0 when it
    /// could not be read), and whether the look is from before GoBot's last
    /// change to the party.
    struct Hud {
        int  size = 0;
        bool stale = false;
    };
    struct Outcome {
        std::string   who;
        bool          joined = false;
        int           fails = 0;        ///< in a row, this one included; 0 after a join
        std::uint64_t hold_ms = 0;      ///< how long the player is held off; 0 after a join
        std::string   why;              ///< in English, for the log
    };

    /// An invite went out to `who` at `now_ms` (or may have: Req Party
    /// clicked, its menu not seen gone), with `size_before` members on the
    /// HUD before it (0: not read). One to the same player before replaces it.
    void sent(const std::string& who, std::uint64_t now_ms, int size_before);
    /// An invite sent kJoinWaitMs ago or more is waiting to be judged.
    bool due(std::uint64_t now_ms) const;
    /// Judge each invite that is due by `hud`: a join, or a hold. A stale
    /// look leaves them for a later one, up to kStaleWaitMs.
    std::vector<Outcome> judge(const Hud& hud, std::uint64_t now_ms);
    /// `who`'s invite came to nothing (the game's message after it, or the
    /// HUD's): counted, the player held off, and an invite of theirs still
    /// waiting to be judged dropped.
    Outcome failed(const std::string& who, std::uint64_t now_ms, const std::string& why);
    /// Whether `who` (or a name alike) is held off at `now_ms`; `left_ms`
    /// gets how much longer.
    bool held(const std::string& who, std::uint64_t now_ms, std::uint64_t* left_ms = nullptr) const;
    /// Failures in a row so far for `who`, and whether an invite of theirs is
    /// waiting to be judged.
    int  fails(const std::string& who) const;
    bool waiting(const std::string& who) const;

private:
    struct Pending {
        std::string   who;
        std::uint64_t sent_ms = 0;
        int           size_before = 0;
        std::size_t   credits_before = 0;   ///< credits_.size() as it went out
    };
    /// A join credited to an invite, for the others out at once: when that
    /// invite went out, and the HUD's size before it.
    struct Credit {
        std::uint64_t sent_ms = 0;
        int           size_before = 0;
    };
    /// The key a name is kept under: one already kept that is alike, or itself.
    std::string key_of(const std::string& who) const;
    static bool same(const std::string& a, const std::string& b);
    /// Joins credited since `p` went out that `p`'s own size_before did not
    /// already show (review of 2026.11.17b): an older invitee who joined
    /// before `p`'s HUD was read is in that size already, and taken off
    /// again it would call `p`'s own join a miss.
    int credited_since(const Pending& p) const;

    std::vector<Pending>                 pending_;
    std::map<std::string, int>           fails_;
    std::map<std::string, std::uint64_t> held_until_;
    std::vector<Credit>                  credits_;   ///< joins credited while invites were out at once
};

} // namespace gobot
