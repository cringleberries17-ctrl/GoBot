#pragma once

#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <string>
#include <vector>

namespace gobot {

/// The party as the game draws it on its own screen, read off a frame: whether
/// the player is alone, a member or the leader, and how many are in it.
///
/// The game shows each other member as a panel 130 by 42 pixels -- a name box,
/// the HP and MP bars, a portrait box -- down the left of the screen below the
/// HUD's top row, and frames whoever leads in red with a coin: that member's
/// panel, or the player's own 290 by 44 panel at the top left when it is the
/// player who leads. GoC knows its party from packets and checks the screen
/// with these same pictures of the game's (Dialog_PartyInfo.spr,
/// Party_King_Other.spr); the other helper reads the same panels. The one live
/// picture of a panel there is -- the other helper's party_leader_key2 -- is
/// GoC's member panel to the pixel, with GoC's leader frame over it at the
/// same place, apart from its two bars and a one-pixel shift of the cut.
///
/// No saved frame of the live client has a party on it, so where the panels
/// sit is searched for rather than assumed: anywhere in the left of the
/// screen, below the top of the HUD. A panel is matched by its frame alone --
/// the bars, the name box and the portrait box are left out, since what is in
/// them changes -- and the red frame is looked for in colour, which is what
/// tells it from the grey of a member's. On the 1,065 saved frames of the live
/// client, all of them without a party, a member panel's frame scores 0.69 at
/// best anywhere there and the red frames 0.42; drawn in, with bars, a name
/// and a face, 0.99 and 1.00.
///
/// Read only: nothing here clicks or presses anything.

enum class PartyState {
    Unknown,    ///< the HUD is not on screen, or what is there makes no sense
    Alone,      ///< the HUD is up and no party panel is on it
    Member,     ///< other members' panels, and the player does not lead
    Leader,     ///< the player's own panel wears the leader's frame
};
/// "unknown", "alone", "member", "leader", for the log.
const char* party_state_name(PartyState s);

/// One other member's panel.
struct PartyPanel {
    ScreenRect at;                  ///< the 130 by 42 panel, in frame coordinates
    float      score = 0.0f;        ///< how alike its frame is to a member panel's
    bool       leader = false;      ///< the leader's red frame is over it
    float      leader_score = -1.0f;
};

/// What one look at the party HUD saw.
struct PartyHud {
    PartyState state = PartyState::Unknown;
    /// Members, the player among them: the other members' panels and the
    /// player's own, which is the panel at the top left and never one of the
    /// list. 0 when Unknown.
    int  size = 0;
    /// Six, as the game holds at most. Only ever said of a party seen, never
    /// of one that could not be read: a false "full" would stop invites.
    bool full = false;
    bool own_leader = false;        ///< the player's own panel wears the red frame
    float own_score = -1.0f;        ///< how alike to it, -1 when not looked for
    std::vector<PartyPanel> panels; ///< the other members', top to bottom
    std::string why;                ///< when Unknown, why, in English for the log
};

/// The most a party holds, the player included.
constexpr int kPartyMax = 6;

/// Reads the party HUD with the game's own pictures of it.
class PartyHudReader {
public:
    /// How alike a member panel's frame has to be. Real frames without a
    /// party reach 0.69 at most, a panel drawn in 0.99.
    static constexpr float kPanelScore = 0.85f;
    /// How alike the red frame has to be, in colour, where a panel or the
    /// player's own panel is. A member's grey panel scores 0.13 against it;
    /// the live helper's picture of a leader's panel 0.87.
    static constexpr float kLeaderScore = 0.70f;
    /// Where panels are looked for: the left of the screen, from the bottom of
    /// the player's own panel down -- more than twice as wide as a panel, and
    /// deep enough for five of them under the HUD's second row at either
    /// client size. A search of the left of a 1280x720 frame costs a tenth
    /// of a second or so, and is done every few seconds at most.
    static constexpr int kSearchTop = 40;
    static constexpr int kSearchRight = 320;
    static constexpr int kSearchBottom = 480;

    /// Takes what it needs from `store`: the game's member panel, the two
    /// leader frames (either skin -- the pictures are the same) and the HUD's
    /// Level plate, which says the HUD is up at all. False, with what is
    /// missing in `missing`, when one of them is not there.
    bool load(const TemplateStore& store, std::string* missing = nullptr);
    bool ready() const { return ready_; }

    /// Just those pictures, into `into` under the names load() looks for, each
    /// from the first place TemplateStore::load_best would take it from --
    /// without loading the thousand others. False, with the first that is
    /// missing, when one is not there.
    static bool load_pictures(TemplateStore& into, const std::string& templates_ini,
                              std::string* missing = nullptr);

    /// `trusted`: the frame is the game's own picture of itself -- not read
    /// off the screen with something over the game, which would be read as
    /// the game. An untrusted frame is Unknown: see party_frame_trusted.
    PartyHud read(const Frame& f, bool trusted = true) const;

    /// The member panel as it is matched: the game's picture with the bars,
    /// the name box, the portrait box and the band the leader's frame covers
    /// left see-through. For tests, and for gobot_cli --party-hud.
    const Frame& panel_picture() const { return panel_; }

private:
    Frame panel_;       ///< Dialog_PartyInfo, what changes made see-through
    Frame other_;       ///< Party_King_Other: the red frame over a member's panel
    Frame mine_;        ///< Party_King_My: the red frame over the player's own
    Frame landmark_;    ///< hpmp_indicator2, the HUD's Level plate
    bool  ready_ = false;
};

/// Whether a frame of `window` can be read for its party: one the game drew
/// itself always; one read off the screen (ScreenCapture::from_screen) only
/// with the game in front and nothing over it -- the crossing's own rule for
/// trusting a frame. Another window over the left of the screen would hide
/// the panels and read as a player alone.
bool party_frame_trusted(bool from_screen, void* window);

/// Whether an invite can work now, by what the HUD says, as GoC's chat
/// invites go by its party list ("Bỏ qua: Party đã đầy (6/6).", "Đang trong
/// party nhưng không phải chủ party."):
///
/// - a party seen full: no;
/// - a member who does not lead, with `always_leader` ("Chỉ mời khi đang làm
///   chủ tổ đội"): no;
/// - anything else -- alone, leading, a member with the tick off, or a HUD
///   that could not be read -- yes, as GoBot invited before it read the HUD.
struct PartyGate {
    bool        allowed = true;
    std::string reason_vi;          ///< why not, for the screen; empty when allowed
    std::string reason_en;          ///< the same for the log
};
PartyGate party_invite_gate(const PartyHud& hud, bool always_leader);

/// The words the screen uses, so the tab and the tests say the same thing.
namespace party_text {
inline const char* const kFull = "Tổ đội đã đủ 6 người";
inline const char* const kNotLeader = "Đang trong tổ đội nhưng không phải chủ tổ đội";
inline const char* const kUnread = "không đọc được khung tổ đội";
} // namespace party_text

/// "Tổ đội: một mình", "Tổ đội: thành viên (3/6)", "Tổ đội: chủ tổ đội (3/6)",
/// "Tổ đội: không đọc được": the state card's line.
std::string party_hud_line_vi(const PartyHud& hud);

/// One line for the log: the state, the size, and where each panel is.
std::string party_hud_text(const PartyHud& hud);

} // namespace gobot
