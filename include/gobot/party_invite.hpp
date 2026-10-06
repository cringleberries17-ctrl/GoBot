#pragma once

#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <string>

namespace gobot {

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

} // namespace gobot
