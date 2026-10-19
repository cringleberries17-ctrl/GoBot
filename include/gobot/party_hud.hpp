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
/// HUD's top row, and marks whoever leads with a crown: a gold medallion on
/// the top-left corner of that member's panel, or of the player's own 290 by
/// 44 panel at the top left when it is the player who leads, a red frame round
/// the panel with it. GoC tells who leads by that crown, with the game's own
/// two pictures of the mark (Party_King_My.spr on the player's own panel,
/// Party_King_Other.spr on another member's), and finds the panels with the
/// game's Dialog_PartyInfo.spr; so does this. The game's two skins, inven\0
/// and inven\1, are its English and Vietnamese pictures -- the same size at
/// either client size, and differing only where a picture has words in it --
/// so their copies of these are the same pixels. The one live picture of a
/// panel the other helper has, party_leader_key2, is GoC's member panel to the
/// pixel, with GoC's leader's mark over it at the same place, apart from its
/// two bars and a one-pixel shift of the cut.
///
/// Four saved frames of the live client have a party on them, so where the
/// panels sit is searched for rather than assumed: anywhere in the left of the
/// screen, below the top of the HUD. A panel is matched by its frame alone --
/// the bars, the name box and the portrait box are left out, since what is in
/// them changes, and so are its last columns, under the bag where the player
/// keeps it. On the 1,058 saved frames of the live client without a party, a
/// member panel's frame scores 0.71 at best anywhere there; the live party's
/// panel 0.93, and drawn in, with bars, a name and a face, 0.99 and 1.00.
///
/// Who leads goes by the crown, in colour, on the corner of the player's own
/// panel and of each member's -- the player's own without its part over the
/// HUD's seal slot, whose icon may be drawn over it (kSealSlot). The red
/// frame is read as well, the crown's corner left out of it, as supporting
/// evidence for the log: on the four saved frames of the live party it
/// agrees with the crown -- 1.00 round the panel with the crown on it, and
/// nothing like it, under 0.3, round the player's own.
///
/// What it cannot see it does not guess: the bag's window over the panels'
/// column leaves a party at least as big as what is seen, and nobody seen
/// under it is no reading at all. Another window over all of the panels and
/// their crowns -- one of the game's own, or its tooltip, which has no tab
/// strip to be found by -- looks like no party at all, and reads as alone:
/// alone is never taken as proof that the player could lead
/// (party_invite_gate).
///
/// Read only: nothing here clicks or presses anything.

enum class PartyState {
    Unknown,    ///< the HUD is not on screen, what is there makes no sense, or the bag or a window hides the panels
    /// The HUD is up and no party panel is on it, nor a crown, nor the bag
    /// over where they go -- or another window over all of them, which looks
    /// the same.
    Alone,
    Member,     ///< other members' panels, another member's crown on the corner of one of them
    Leader,     ///< the crown on the corner of the player's own panel
    /// Other members' panels, and no crown on any of them nor on the player's
    /// own. A party always has its one leader, so this is a reading that
    /// failed -- the crown not matched, or the leader's panel under a window --
    /// and who leads is not known: in a party, but never taken for the
    /// leader, nor for a member who does not lead.
    InParty,
};
/// "unknown", "alone", "member", "leader", "in a party", for the log.
const char* party_state_name(PartyState s);

/// One other member's panel.
struct PartyPanel {
    ScreenRect at;                  ///< the 130 by 42 panel, in frame coordinates
    float      score = 0.0f;        ///< how alike its frame is to a member panel's; -1 when hidden
    bool       leader = false;      ///< the crown is on its corner: this member leads
    float      crown_score = -1.0f; ///< how alike its corner is to the crown, -1 when nothing like it
    /// How alike the red frame round it is to the leader's: supporting
    /// evidence, for the log; who leads goes by the crown.
    float      frame_score = -1.0f;
    /// Its crown alone was seen, on the left of the screen where a member's
    /// panel goes, and not the panel under it -- a window over all of the
    /// panel but its corner. Still a member, and the one who leads.
    bool       hidden = false;
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
    bool own_leader = false;        ///< the crown on the corner of the player's own panel
    float own_crown = -1.0f;        ///< how alike that corner is to the crown, -1 when not looked for
    float own_frame = -1.0f;        ///< the red frame round the player's own panel: for the log
    std::vector<PartyPanel> panels; ///< the other members', top to bottom
    /// Part of where the members' panels are drawn is hidden -- the bag's
    /// window over it, or a member's panel seen by its crown alone: there may
    /// be more of them than were seen. `size` is then the least the party
    /// has, and a reading of nobody under the bag is Unknown, never Alone.
    bool covered = false;
    std::string why;                ///< when Unknown, why, in English for the log
};

/// The most a party holds, the player included.
constexpr int kPartyMax = 6;

/// Reads the party HUD with the game's own pictures of it.
class PartyHudReader {
public:
    /// How alike a member panel's frame has to be. Real frames without a
    /// party reach 0.71 at most, the live party's panel 0.93, a panel drawn in
    /// 0.99.
    static constexpr float kPanelScore = 0.85f;
    /// The crown in both of GoC's pictures of the leader's mark: the
    /// medallion on its top-left corner, with its dark rim and the corner of
    /// the red frame under it -- 355 of these pixels are drawn, the same in
    /// both pictures but for the frame's shading down the left edge. Where a
    /// panel is, the crown is this far into it.
    static constexpr ScreenRect kCrown{ 0, 0, 19, 20 };
    /// The HUD's top-left slot, the phong ấn seal's, on the player's own
    /// panel: its icon lies within this box on every saved frame of the live
    /// client, at either client size, and the bottom right of the player's
    /// own crown lies over the box's top left. No saved frame shows which of
    /// the two the game draws last -- none has the player leading -- and
    /// drawn over the crown, the icons of five of GoC's 22 seals take it below
    /// kCrownScore, this player's own seal to 0.84. So the player's own crown
    /// is matched without its part over this box: the 197 of its pixels left,
    /// its top rows and its left edge, still score 1.00 with any of 134 of
    /// GoC's seal, pet and item icons drawn in the slot over it, as nothing
    /// in the slot has a part in the score.
    static constexpr ScreenRect kSealSlot{ 6, 7, 34, 32 };
    /// How alike a corner has to be to the crown, in colour, for the crown to
    /// be seen there. Where the game draws it, on the four saved frames of
    /// the live party, it scores 1.00. On the 1,062 saved frames of the live
    /// client, the player's own corner scores under 0.3; on the 1,058 without
    /// a party, anywhere the members' panels go scores 0.63 at most. A crown
    /// with the bag dragged over all but its first eleven columns scores
    /// 0.68, and is not seen.
    static constexpr float kCrownScore = 0.80f;
    /// How alike the red frame round a panel has to be, in colour, the
    /// crown's corner left out, to agree with a crown there. Supporting
    /// evidence for the log alone: who leads goes by the crown.
    static constexpr float kFrameScore = 0.70f;
    /// Where panels are looked for: the left of the screen, from the bottom of
    /// the player's own panel down -- more than twice as wide as a panel, and
    /// deep enough for five of them under the HUD's second row at either
    /// client size. A search of the left of a 1280x720 frame costs a tenth
    /// of a second or so, and is done every few seconds at most.
    static constexpr int kSearchTop = 40;
    static constexpr int kSearchRight = 320;
    static constexpr int kSearchBottom = 480;
    /// A member's panel, and the red frame over a leader's, are matched by
    /// their columns left of this. The bag where the player keeps it -- its
    /// tab strip at 134,293 on every saved round of the live client -- has its
    /// left edge at x=121, over the last nine of a panel's 130 columns: its
    /// right border, without which no panel was found. Left of it the live
    /// panels still score 0.92 to 1.00, and no saved frame without a party
    /// more than 0.71.
    static constexpr int kPanelMatchW = 118;
    /// The bag's window round its tab strip, as bag::find finds the strip:
    /// from 16 left of it and 230 above it, 280 wide and 440 high -- its left
    /// edge measured 13 left of the strip and its top 225 above it, where the
    /// player keeps it and dragged to the screen's left edge alike. A bag
    /// reaching left of kPanelMatchW, anywhere down the panels' column, hides
    /// panels.
    static constexpr int kBagLeft = 16, kBagUp = 230, kBagW = 280, kBagH = 440;
    /// How alike the tab strip has to be: what bag::find takes.
    static constexpr float kBagStripScore = 0.62f;
    /// Read off the screen with another program's window over the left of the
    /// game, the panels' column -- left of kPanelMatchW, kSearchTop down to
    /// kSearchBottom -- is that window: mostly one plain colour, as nothing the
    /// game draws there is. On the 1,057 saved frames of the live client with
    /// the HUD up and nothing over the game, the column's commonest colour is
    /// 14% of it at most; on the ten read off the screen with a terminal or
    /// another program over the game, 39% to 64%. A column this plain is no
    /// reading at all: party_frame_trusted asks the windows, this the pixels.
    static constexpr float kPlainShare = 0.30f;
    /// The part of the screen a reading looks at: the player's own panel, and
    /// the members' panels down the left. A frame read off the screen is
    /// trusted for the party when this part of the game is all on a screen
    /// with nothing over it (party_frame_trusted).
    static constexpr ScreenRect kArea{ 0, 0, kSearchRight, kSearchBottom };

    /// Takes what it needs from `store`: the game's member panel, the two
    /// pictures of the leader's mark with the crown on them -- GoBot's own
    /// copies, grabbed/party_crown_other and grabbed/party_crown_mine, or
    /// either skin's (the pictures are the same) -- the HUD's Level plate,
    /// which says the HUD is up at all, and the bag's tab strip, which says
    /// where the bag's window is. False, with what is missing in `missing`,
    /// when one of them is not there.
    bool load(const TemplateStore& store, std::string* missing = nullptr);
    bool ready() const { return ready_; }

    /// Just those pictures, into `into` under the names load() looks for, each
    /// from the first place TemplateStore::load_best would take it from --
    /// without loading the thousand others -- and under the first of its names
    /// that is there. False, with the first name of one that is missing.
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

    /// Where the bag's window is in `f`, when its tab strip is there: what
    /// read() goes by to tell a party it cannot see all of. For tests.
    bool bag_window(const Frame& f, ScreenRect& out) const;

private:
    Frame panel_;       ///< Dialog_PartyInfo, what changes made see-through
    Frame other_;       ///< Party_King_Other's red frame round a member's panel, its crown left out
    Frame mine_;        ///< Party_King_My's red frame round the player's own, its crown left out
    Frame other_crown_; ///< Party_King_Other's crown, kCrown of it
    Frame own_crown_;   ///< Party_King_My's crown, kCrown of it, without its part over kSealSlot
    Frame landmark_;    ///< hpmp_indicator2, the HUD's Level plate
    Frame strip_;       ///< the bag's tab strip, grabbed/inventory
    bool  ready_ = false;
};

/// Whether a frame of `window` can be read for its party: one the game drew
/// itself always; one read off the screen (ScreenCapture::from_screen) only
/// when the part of the game the party is read from (PartyHudReader::kArea)
/// is all on a screen with nothing over it -- whichever window is in front,
/// so that GoBot's own window beside the game does not stop a reading.
/// Another window over the left of the screen would hide the panels and read
/// as a player alone; past a screen's edge reads black, which is no panel.
bool party_frame_trusted(bool from_screen, void* window);

/// Whether an invite can work now, by what the HUD says, as GoC's chat
/// invites go by its party list ("Bỏ qua: Party đã đầy (6/6).", "Đang trong
/// party nhưng không phải chủ party."):
///
/// - a party seen full: no, ticked or not;
/// - with `always_leader` ("Chỉ mời khi đang làm chủ tổ đội") ticked, by the
///   crown, as the player decided on 2026-10-19: yes only when the player's
///   own crown is seen. No for a member under another member's crown, for a
///   party with no crown seen on any panel, for a HUD that could not be
///   read, and for a player read as alone: no crown is seen there at all,
///   and a member whose party's panels are under another window reads as
///   alone too -- the screen cannot tell the two apart. Each says why;
/// - with the tick off, anything but full -- alone, leading, a member, a
///   party whose leader could not be told, or a HUD that could not be read --
///   yes, as GoBot invited before it read the HUD. An invite from alone,
///   which starts a party the player leads, goes only with the tick off.
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
/// Ticked, in a party, and no crown on any panel.
inline const char* const kNoCrown = "Đang trong tổ đội nhưng không thấy vương miện chủ tổ đội ở ô nào";
/// Ticked, and the HUD not read: the player's own crown was not seen.
inline const char* const kUnreadNoCrown = "Không đọc được khung tổ đội nên không thấy vương miện của bạn";
/// Ticked, and read as alone: no crown at all, and no telling alone from a
/// party whose panels are under a window.
inline const char* const kAloneNoCrown =
    "Không thấy vương miện chủ tổ đội của bạn (chưa có tổ đội, hoặc khung tổ đội bị che)";
} // namespace party_text

/// "Tổ đội: một mình", "Tổ đội: thành viên (3/6)", "Tổ đội: chủ tổ đội (3/6)",
/// "Tổ đội: đang trong tổ đội (3/6), không thấy vương miện chủ tổ đội",
/// "Tổ đội: không đọc được": the state card's line.
std::string party_hud_line_vi(const PartyHud& hud);

/// One line for the log: the state, the size, and where each panel is.
std::string party_hud_text(const PartyHud& hud);

} // namespace gobot
