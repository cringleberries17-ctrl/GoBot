#pragma once

#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
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
/// their crowns -- one of the game's own, a dialog, or its tooltip, which
/// have no tab strip to be found by -- looks like no party at all, and reads
/// as alone. So alone is said to be sure (PartyHud::sure_alone) only on
/// positive evidence: a frame that was the game's own pixels all over when it
/// was grabbed (PartyLook), with nothing up on it that could hide a panel --
/// no window of the game, X or none (kWindowSigns), no dialog of any kind
/// (kDialogSigns) and no tooltip anywhere on it, the first member's place
/// not one plain colour -- grabbed long enough after the last thing that
/// changed the party for the game to have drawn it, and well after a party
/// may have been joined (PartyEvents), and the game's own word, once
/// something reads it, not saying otherwise (PartyMembership). Only then
/// does alone count for an invite with the tick (party_invite_gate), or for
/// Tự chat's "Chỉ khi chưa có tổ đội". A window of the game with no X and no
/// sign here -- one no picture of which GoBot has -- is not seen: the live
/// checks are where the next one would show.
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

    /// Alone, and surely so: nothing that could hide a member's panel is up
    /// (PartyHudReader::read says what is looked for), and nothing since has
    /// changed the party (read_party). Only ever true with the state Alone.
    /// When Alone and not sure, what may be hiding one: in English for the
    /// log, and in Vietnamese for the Tổ đội tab.
    bool sure_alone = false;
    std::string alone_doubt;
    std::string alone_doubt_vi;

    /// The frame was grabbed before the last thing that changes the party,
    /// or may have -- an invitation accepted or declined by GoBot, an invite
    /// sent, an invitation gone with no answer from GoBot (PartyEvents): what
    /// it shows may be gone, and no invite goes on it, ticked or not
    /// (party_invite_gate). Set by read_party; what that was, for the log and
    /// the tab.
    bool stale = false;
    std::string stale_en;
    std::string stale_vi;
};

/// The most a party holds, the player included.
constexpr int kPartyMax = 6;

/// The client's two drawings of a window's close button in their other
/// states, by the names the template store gives them: under the pointer (/1)
/// and held down (/2). "Surely alone" looks for these as well as for the
/// buttons at rest (bag::kCloseButtons), each by its own picture: with the
/// pointer resting on it, the box's X (btn_CloseBox) scores 0.798 against its
/// drawing at rest, and held down 0.729 -- under what a close button is taken
/// at (PartyHudReader::kWindowXScore) -- so a window over the panels with the
/// pointer on its X would otherwise pass for none. The dialog's X
/// (btn_CloseDialog) scores 0.998 and 0.971 against its own, and is looked
/// for in each state all the same.
inline const char* const kCloseButtonStates[] = { "inven/1/btn_CloseDialog.spr/1", "inven/1/btn_CloseDialog.spr/2",
                                                  "inven/1/btn_CloseBox.spr/1", "inven/1/btn_CloseBox.spr/2" };

/// A sign of one of the game's dialogs, or of a window it is not told by its
/// X, as "surely alone" looks for it anywhere on the frame: a dialog has no
/// close button, so the close buttons do not rule one out, and a dialog over
/// the panels hides them as a window does. Any one found, and alone is not
/// sure.
struct DialogSign {
    const char* name;       ///< the picture, by the name the template store gives it
    /// The part of it looked for, from its top-left; empty for all of it.
    ScreenRect  part;
    float       score;      ///< how alike, at least
    const char* what;       ///< what it is, in English for the log
    /// Looked for from this row of the frame down: for a picture the HUD
    /// itself shows above it, where it is no sign of anything.
    int         from_y = 0;
    /// And only this far or more left of the frame's right edge: for a
    /// window that has its own place against that edge, where it can cover
    /// nothing of the party's -- found anywhere else, it was put there.
    int         right_margin = 0;
    /// The dark of a dialog's panel of words left see-through in the part: a
    /// word written next to what is looked for leaves it as it is.
    bool        words_out = false;
};

/// Every sign of a dialog there is a picture of, and what each is found at:
///  - a dialog's two buttons, [Xác Nhận][Hủy Bỏ], in both of the client's
///    drawings of the pair, as bag::find_dialogs finds them (0.82): the bin's
///    and the registration's questions, the revive question, a shop's;
///  - a party invitation's buttons and the border beside them, as the party
///    worker finds an invitation (PartyInvite, 0.80);
///  - a notice with Xác Nhận alone under it, as bag::notice_up finds one
///    (0.75) -- its picture has no words, and matches the two-button
///    dialogs as well;
///  - the client's own dialog frames, the frames those dialogs are drawn in
///    whatever their buttons and words -- inven/1/dialog (its dialog.png,
///    the bin's and the registration's questions among them) and the taller
///    inven/1/dialog_2 -- each by its top-left corner: the rounded outline,
///    its band, and the corner of the panel of words with its knot, above
///    where any word is written and the same however wide the dialog is. The
///    taller frame's panel starts three rows lower, so its corner is a sign
///    of its own. Unlike the buttons, a frame is seen with a button held
///    down, which takes the pair below what it is found at. On the 1,849
///    saved frames of the live client the first corner scores 1.00 on all
///    229 with a dialog up -- the very frames the other signs find -- and
///    0.78 at most anywhere else (the ESC menu's corner). kDialogFrameScore
///    says how alike.
///  - the knots on the top-left and bottom-right corners of those frames'
///    panel of words, inside the outline and apart from it: every cut GoBot
///    or the live client has of a dialog drawn in the client's frame starts
///    at most 4 pixels right of its outline and 6 below it -- the bin's
///    quantity question, the party's name question, the notices -- so a cut
///    with no outline and no button, laid anywhere, is still found by a knot,
///    the same in the taller frame. A notice's words come right up to its
///    top knot, so the bottom one is looked for with the panel's dark, where
///    words go, left out. Each knot scores 1.00 on the 231 saved frames of
///    the live client the first corner finds a dialog on, and under 0.70 on
///    all the rest of the 1,851;
///  - the death panel, "Trạng thái bất tỉnh", by its title tab from the tab's
///    40th column on, so that a panel partly past the screen's left edge is
///    still found -- the client's picture and the live client's
///    (grabbed/chardeadpanel) the same pixels there, and no button in it to
///    light under the pointer, as the reviver's two corners do.
/// The pictures are the client's own (inven/1) and GoBot's grabbed copies of
/// the live client's (grabbed/), named in the shipped templates.ini; without
/// any one of them alone is never sure.
inline constexpr float kDialogFrameScore = 0.90f;

/// Windows of the game told by something besides their X: for the moment that
/// X is drawn held down, which none of the client's drawings of it matches,
/// and for the windows that have none. Found, each is a window open, as an X
/// is:
///  - the monster book by its first tab (bag::kBookTitle), as bag::find_book
///    finds it -- 1.00 on all 299 saved frames of the live client with the
///    book open, its X held down on three of them, and 0.58 at most anywhere
///    else;
///  - the party's menu (Delegate, Expel, Leave), which the client draws over
///    the panel clicked -- the leader's, crown and all, for a member -- with
///    no X; and the party's row of buttons (Mời Vào Đội, Ủy Quyền Đội, Trục
///    Xuất, Rời Đội), with none either. Each by its words and frame, at a
///    score a row lit under the pointer or greyed for a member still
///    reaches. The client's picture of the menu has English words in both
///    of its skins: a live client that wrote other words in it would not be
///    found by it, which the live checks are to settle;
///  - the NPC shop, "Cửa Hàng" -- Thoát closes it, no X -- by its title tab
///    and the band beside it, and by its row of buttons, Mua Lại, Mua and
///    Thoát: its rows of goods fill in as it is used;
///  - the ESC menu by its top, the band above its five buttons, which do
///    light under the pointer, and its X drawn over the band's corner;
///  - the area's player list by its empty rows ruled through, as the party
///    worker's PlayerList finds it open (grabbed/player_list_grid, 0.90) --
///    anywhere but its own place, hanging off its button against the
///    screen's right edge (956,139 at 1024x768, 1212,139 at 1280x720), where
///    it covers nothing of the party's: the party worker opens it there for
///    every invite. Found anywhere else, somebody moved it, or it is some
///    other window drawn the same way;
///  - a second character panel: the HUD's Level plate anywhere below the
///    HUD's own top row.
/// Each is looked for without its window's first columns, where it has any
/// to spare: a window over the party's panels is against the screen's left
/// edge, and may be drawn partly past it -- the round 7b reviewer laid them
/// up to 20 pixels past it. Each score is set on the 1,851 saved frames of
/// the live client, none of which shows the menu, the row of buttons, the
/// shop or the death panel, and on the client's pictures laid on frame.png,
/// whole and partly past the edge -- 1.00 each; see the scores beside each.
inline constexpr DialogSign kWindowSigns[] = {
    { "grabbed/monster_book_title", ScreenRect{}, 0.90f, "the monster book's tab" },
    // The menu but for its first 24 columns: 0.52 at most on the saved
    // frames; a row lit, or two rows greyed for a member and the third lit,
    // 0.74 or more.
    { "inven/1/dialog_partymenu.spr/0", ScreenRect{ 24, 0, 66, 62 }, 0.70f, "the party's menu" },
    // The row of buttons but for its first 24 columns: 0.64 at most; a
    // button lit, or two greyed and another lit, 0.84 or more.
    { "grabbed/party_panel", ScreenRect{ 24, 0, 266, 29 }, 0.75f, "the party's row of buttons" },
    // The shop's title tab: the bag's, drawn the same way but for its words,
    // scores 0.71 on it, the death panel's 0.70; its buttons, 180 columns in,
    // 0.43 at most.
    { "inven/1/Dialog_Shop.spr/0", ScreenRect{ 0, 0, 100, 24 }, 0.85f, "the NPC shop's title" },
    { "inven/1/Dialog_Shop.spr/0", ScreenRect{ 180, 385, 156, 32 }, 0.75f, "the NPC shop's buttons" },
    // The ESC menu's top but for its first 24 columns: 0.92 on the 15 saved
    // frames with it open, its X drawn over the band; under 0.75 on every
    // other. The band is the shop's and the death panel's too, 0.93 on each.
    { "inven/1/dialog_setting.spr/0", ScreenRect{ 24, 0, 162, 30 }, 0.85f,
      "the ESC menu's top, or another window's band like it" },
    // The empty rows but for their first 30 columns: 1.00 where the list is
    // open on the two saved frames that show it -- at its own place, its rows
    // ending 24 left of the edge -- and 0.47 at most anywhere else.
    { "grabbed/player_list_grid", ScreenRect{ 30, 0, 220, 60 }, 0.90f, "the area's player list, away from its place",
      0, 40 },
    // Below the HUD's own top row -- the player's panel is 44 high -- the
    // plate scores 0.52 at most on the saved frames of the game's own picture,
    // and 1.00 on the client's picture of the panel; 44 columns in.
    { "grabbed/hpmp_indicator2", ScreenRect{}, 0.75f, "a second character panel's Level plate", 48 },
};

inline constexpr DialogSign kDialogSigns[] = {
    { "grabbed/confirm_button", ScreenRect{}, 0.82f, "a dialog's two buttons" },
    { "grabbed/del_item_confirm_1", ScreenRect{ 101, 51, 85, 21 }, 0.82f, "a dialog's two buttons, drawn closer" },
    { "grabbed/party_invite_buttons", ScreenRect{}, 0.80f, "a party invitation's buttons" },
    { "grabbed/dialog_ok", ScreenRect{}, 0.75f, "a notice's Xác Nhận" },
    { "inven/1/dialog", ScreenRect{ 0, 0, 31, 13 }, kDialogFrameScore, "the client's dialog frame" },
    { "inven/1/dialog_2", ScreenRect{ 0, 0, 31, 16 }, kDialogFrameScore, "the client's taller dialog frame" },
    { "inven/1/dialog", ScreenRect{ 13, 7, 14, 12 }, kDialogFrameScore, "the top knot of the client's dialog frame" },
    { "inven/1/dialog", ScreenRect{ 168, 38, 22, 17 }, kDialogFrameScore,
      "the bottom knot of the client's dialog frame", 0, 0, true },
    // The death panel's title from its 40th column, "thái bất tỉnh" and the
    // end of its tab: 0.63 at most on the saved frames; the live client's
    // panel the same, 1.00.
    { "inven/1/dialog_Panic.spr/0", ScreenRect{ 40, 3, 130, 22 }, 0.80f, "the death panel's title" },
};

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

    // ---- alone, for sure (PartyHud::sure_alone) ----
    //
    // The player's decision of 2026-10-19: with "Chỉ mời khi đang làm chủ tổ
    // đội" ticked, a player alone invites, to start a party, as GoC does --
    // only when GoBot is sure the player is alone. A member whose panels are
    // all hidden reads as alone too, and what hides them is the bag (read()
    // sees it by its tab strip), a window of the game, a dialog, the game's
    // tooltip, or something laid over them in one colour. So "alone" has to
    // be shown, not merely not contradicted: a frame read as alone is sure
    // only when all of it is the game's own pixels -- drawn by the game
    // itself, or read off the screen with the whole of the game on a screen
    // and nothing over any of it (party_frame_whole): a window of the game
    // over the panels may reach far right of them, and the frame read off the
    // screen has none of what lies under another program's window or past
    // the screen's edge, its X perhaps among it -- and when, on it:
    //  - no close button is anywhere on the frame, in any of the client's
    //    drawings of one, at rest (bag::kCloseButtons), under the pointer or
    //    held down (kCloseButtonStates), at kWindowXScore: every window of
    //    the game wears one -- the bag, Kho Cash, the monster book, the ESC
    //    menu, the quest window -- and one over the panels shows its; nor
    //    a window told by something else (kWindowSigns): one whose X is held
    //    down, and the ones that wear none -- the party's menu and row of
    //    buttons, the NPC shop, the ESC menu's body, the area's player list
    //    away from its place, a second character panel;
    //  - no dialog of any kind is anywhere on it (kDialogSigns): none of a
    //    question, an invitation, a notice or the death panel wears an X, and
    //    the frame a dialog is drawn in is the client's own;
    //  - no edge of a tooltip is anywhere on it: the game draws every
    //    tooltip with a border three pixels thick in fixed colours,
    //    kTooltipOuter, kTooltipMiddle, kTooltipInner from the outside in,
    //    and a run of it at least kTooltipRun long, down or across, is a
    //    tooltip's edge. A tooltip is drawn over everything, a window's X
    //    among it, wherever that X is -- so one anywhere is enough;
    //  - the first member's place (kFirstSlot) is not one plain colour: its
    //    commonest colour is under kFirstSlotPlain of it. A party has its
    //    first other member there, so it is where anything over the party has
    //    to be.
    // Without every one of the close buttons' and the dialogs' pictures none
    // of it can be ruled out, and alone is never sure. The frame's time is
    // read_party's to judge: see PartyEvents.

    /// A close button, as the trash pass and the popup closer find one. The
    /// pointer over a window's X draws it a little dimmer, and the monster
    /// book's then scores 0.84 on GoC's picture of it, so lower than theirs.
    /// The two buttons the client draws in other states are each looked for
    /// by their own pictures of those states (kCloseButtonStates).
    static constexpr float kWindowXScore = 0.80f;
    /// The tooltip's border, outside in, as red, green, blue; how far a
    /// pixel may be from each in any channel; and how long a run of it is an
    /// edge. On the 1,870 saved frames of the live client the border is on
    /// 60, each time a whole tooltip, all four edges -- over the bag, the
    /// hotbar and the HUD -- and nowhere else in runs of 12 or more. It is
    /// looked for over the whole frame.
    static constexpr std::uint32_t kTooltipOuter = 0x838383, kTooltipMiddle = 0x22223F, kTooltipInner = 0x353557;
    static constexpr int kTooltipTolerance = 6;
    static constexpr int kTooltipRun = 12;
    /// The first other member's place: where the game draws that panel at
    /// both client sizes, give or take a few pixels.
    static constexpr ScreenRect kFirstSlot{ 0, 100, kPanelMatchW, 56 };
    /// How much of kFirstSlot one exact colour may be. The world there is
    /// never that plain on a saved frame read as alone: at most 0.62, a clear
    /// sky at 1024x768.
    static constexpr float kFirstSlotPlain = 0.90f;

    /// Takes what it needs from `store`: the game's member panel, the two
    /// pictures of the leader's mark with the crown on them -- GoBot's own
    /// copies, grabbed/party_crown_other and grabbed/party_crown_mine, or
    /// either skin's (the pictures are the same) -- the HUD's Level plate,
    /// which says the HUD is up at all, and the bag's tab strip, which says
    /// where the bag's window is. False, with what is missing in `missing`,
    /// when one of them is not there. And the client's close buttons, at rest
    /// and in their other states (bag::kCloseButtons, kCloseButtonStates),
    /// the other windows' signs (kWindowSigns) and the dialogs'
    /// (kDialogSigns), when they are all there: without any one of them
    /// every reading is made as before, and alone is never sure.
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
    /// `whole_seen`: all of the frame is the game's own pixels, not only the
    /// part the party is read from (party_frame_whole). Alone is never sure
    /// without it: a window of the game over the panels is ruled out by its
    /// close button being nowhere on the frame, and a close button under
    /// another program's window, or past the screen's edge, is not on the
    /// frame to be found. A frame read off the screen is to be given what
    /// party_frame_whole says of it; a saved frame is taken for the game's own.
    PartyHud read(const Frame& f, bool trusted = true, bool whole_seen = true) const;

    /// The member panel as it is matched: the game's picture with the bars,
    /// the name box, the portrait box and the band the leader's frame covers
    /// left see-through. For tests, and for gobot_cli --party-hud.
    const Frame& panel_picture() const { return panel_; }

    /// Where the bag's window is in `f`, when its tab strip is there: what
    /// read() goes by to tell a party it cannot see all of. For tests.
    bool bag_window(const Frame& f, ScreenRect& out) const;

    /// A tooltip's edge somewhere in `area` of `f`: where its run starts, in
    /// `at`. For tests, and read() asks it of the whole frame.
    static bool tooltip_edge(const Frame& f, const ScreenRect& area, ScreenRect* at = nullptr);

    /// A dialog of the game anywhere on `f`, by kDialogSigns: where its sign
    /// is, in `at`, and which sign, in `what`. False, too, without the
    /// pictures -- which `can_see_dialogs` says. For tests and gobot_cli
    /// --party-hud; read() asks it, beside the close buttons.
    bool dialog_up(const Frame& f, ScreenRect* at = nullptr, std::string* what = nullptr) const;
    bool can_see_dialogs() const { return !dialogs_.empty(); }

private:
    /// Whether a frame read as alone is surely so: `hud.sure_alone`, or why
    /// not in `hud.alone_doubt` and `hud.alone_doubt_vi`. `whole_seen` as
    /// read() takes it.
    void judge_alone(const Frame& f, bool whole_seen, PartyHud& hud) const;

    /// The client's close buttons, bag::kCloseButtons and kCloseButtonStates.
    std::vector<Frame> close_x_;
    /// The other windows' signs, kWindowSigns, and the dialogs',
    /// kDialogSigns: each the part of its picture looked for; all of them,
    /// or none.
    struct Sign {
        Frame       picture;
        float       score = 0.0f;
        const char* what = "";
        int         from_y = 0;
        int         right_margin = 0;
    };
    std::vector<Sign> windows_;
    std::vector<Sign> dialogs_;
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

/// Whether all of frame `f` of `window` is the game's own pixels, as
/// PartyHudReader::read takes `whole_seen`: one the game drew itself always;
/// one read off the screen only when the whole of the game's picture -- `f`,
/// its client area -- is on a screen with nothing over any of it, GoBot's own
/// window included. Being only beside the game, GoBot's window is no matter.
/// `in_the_way` names the program over it, "taskbar", or "off the screen",
/// as AutoLogin::part_seen does.
bool party_frame_whole(bool from_screen, void* window, const Frame& f, std::string* in_the_way = nullptr);

/// One look at the game for the party: a frame, and what was so of it when
/// it was grabbed -- not when it is read, which may be a second or more
/// later, a pass of the party worker having answered an invitation or read
/// the HUD meanwhile: another program's window over the game at the grab,
/// gone by then, would have the frame taken for the game's own.
struct PartyLook {
    Frame         frame;
    bool          from_screen = false;  ///< read off the screen, not drawn by the game itself
    /// party_frame_trusted, as it was at the grab: asked right before it and
    /// right after it, and only when both said so -- or the game drew the
    /// frame itself. A window that came or went in between makes it false.
    bool          trusted = false;
    /// party_frame_whole, the same way.
    bool          whole = false;
    /// What lay over the game, or "off the screen", when either was not so.
    std::string   in_the_way;
    /// When it was grabbed, on the steady clock (now_ms): the moment the grab
    /// began, so that whatever happened after this is after the frame.
    std::uint64_t grabbed_ms = 0;
};

/// How a look is taken: the game's picture, whether a part of the game is
/// seen, and the clock. The real game's is party_sight's; a test's is a
/// pretend game's.
struct PartySight {
    /// The game's picture, and whether it came off the screen; false when
    /// there is none.
    std::function<bool(Frame& out, bool& from_screen)> grab;
    /// Whether `part` of the game's client area is all on a screen with
    /// nothing over it, and when not what is (AutoLogin::part_seen).
    std::function<bool(const ScreenRect& part, std::string* in_the_way)> seen;
    std::function<std::uint64_t()> now;     ///< the steady clock, in ms
};
/// The real game's: `capture` of `window`, AutoLogin::part_seen on it, and
/// now_ms. `capture` and `window` must outlive what is returned.
PartySight party_sight(ScreenCapture& capture, void* window);

/// Takes one look with `sight`: whether the party's part of the game
/// (PartyHudReader::kArea) and all of it are seen, asked right before the
/// grab and right after it, and kept with the frame. False when nothing was
/// grabbed.
bool take_party_look(const PartySight& sight, PartyLook& out);

/// What has changed the party, or may have, and when -- what a look is held
/// up against. The game draws a change to the party a moment after it is
/// made -- the server's answer, then a frame -- so a frame grabbed before it,
/// or just after, still shows the party as it was. Round 7's final check
/// found the party worker accepting an invitation and then, in the same
/// pass, inviting by the frame from before it: "alone", while a member.
///
/// Three kinds:
///  - what GoBot itself did that changes the party (changed) -- declining an
///    invitation, sending an invite -- which a look must have been grabbed
///    after to count for an invite at all;
///  - a party the player may have joined (joined): GoBot accepting an
///    invitation, or an invitation going from the screen without GoBot having
///    answered it -- the player's own hand, perhaps (invitation). A change
///    too; and the game may take a while to draw the party, a while nobody
///    has measured on the live client, so alone is not taken for sure until
///    kJoinWaitMs after it, not merely kSettleMs;
///  - what may change the party without GoBot (seen) -- an invitation on
///    screen, which the player may answer by hand.
/// A look counts as alone, surely, only when grabbed kSettleMs or more after
/// the last of them all, and kJoinWaitMs or more after the last join.
///
/// Every look at the game that looks for an invitation says what it saw
/// (invitation): the party worker's on each pass, Tự chat's before a line
/// ticked "Chỉ khi chưa có tổ đội". One seen and then gone, unanswered by
/// GoBot (answered), may have been accepted by hand.
///
/// This is also where the game's own word on the party comes in, once
/// something reads it (P0-3, the packets): a party joined there is joined(),
/// left is changed(), and what it says of the party now is PartyMembership.
/// Safe to share between threads.
class PartyEvents {
public:
    /// How long after a change a look is too soon to say "alone" by: the
    /// server's answer and the game's next frame, with room to spare for a
    /// game that lags. An asker turned away meanwhile is told why.
    static constexpr std::uint64_t kSettleMs = 3000;
    /// How long after a party may have been joined: an invitation is up for
    /// about ten seconds (PartyInvite), and an accept that did not take -- the
    /// party full, the inviter gone -- leaves no party at all, so one not
    /// drawn by twice that long is taken to have not been joined.
    static constexpr std::uint64_t kJoinWaitMs = 20'000;

    struct Event {
        std::uint64_t at_ms = 0;    ///< on the steady clock; 0: none yet
        std::string   en;           ///< what, in English for the log
        std::string   vi;           ///< and in Vietnamese for the Tổ đội tab
    };
    /// GoBot did something at `at_ms` that changes the party, or may have.
    void changed(std::uint64_t at_ms, std::string en, std::string vi);
    /// The player may have joined a party at `at_ms`: a change, and the
    /// start of kJoinWaitMs.
    void joined(std::uint64_t at_ms, std::string en, std::string vi);
    /// Something that may change the party without GoBot was on screen at
    /// `at_ms`.
    void seen(std::uint64_t at_ms, std::string en, std::string vi);
    /// What a look grabbed at `at_ms` saw of an invitation: `up`, it is seen
    /// (seen); not, after one was seen up on an earlier look and GoBot has
    /// not answered it since, it went unanswered -- joined, as it may have
    /// been accepted by hand. A look older than the last that saw it up, or
    /// than GoBot's answer, says nothing new.
    void invitation(std::uint64_t at_ms, bool up);
    /// GoBot answered the invitation on screen at `at_ms`, accepting or
    /// declining it (with joined or changed): its going is GoBot's doing.
    void answered(std::uint64_t at_ms);
    /// The last change (a join among them), the last event of any kind, and
    /// the last join.
    Event last_change() const;
    Event last_any() const;
    Event last_join() const;

private:
    mutable std::mutex mutex_;
    Event change_, any_, join_;
    /// An invitation seen up and not seen gone since, nor answered by GoBot;
    /// the newest look that saw one up; GoBot's last answer.
    bool          invitation_up_ = false;
    std::uint64_t invitation_seen_ms_ = 0;
    std::uint64_t answered_ms_ = 0;
};

/// The party by the game's own word -- the seam where P0-3 gives the party
/// from the game's packets, which no window or tooltip can hide. Until then
/// nothing reads it, `known` is false, and the screen decides alone.
struct PartyMembership {
    bool known = false;         ///< something read the party off the game itself
    bool in_party = false;      ///< and the player is in one, whoever leads
};

/// The party as everything known has it, for one decision on `look` -- the
/// tick's invite, Tự chat's "Chỉ khi chưa có tổ đội", the tab's "Thử xem":
///  - the frame read as it was when grabbed (PartyHudReader::read, with the
///    look's trusted and whole);
///  - stale (PartyHud::stale) when grabbed before the last change GoBot made,
///    a join among them;
///  - alone surely only when also grabbed PartyEvents::kJoinWaitMs or more
///    after the last party that may have been joined -- GoBot's accept, an
///    invitation gone unanswered -- and PartyEvents::kSettleMs or more after
///    the last event of any kind, and the game's own word, when there is one,
///    does not say the player is in a party.
PartyHud read_party(const PartyHudReader& reader, const PartyLook& look, const PartyEvents& events,
                    const PartyMembership& game = {});

/// Whether an invite can work now, by what the HUD says, as GoC's chat
/// invites go by its party list ("Bỏ qua: Party đã đầy (6/6).", "Đang trong
/// party nhưng không phải chủ party."):
///
/// - a frame grabbed before GoBot's last change to the party (PartyHud::
///   stale): no, ticked or not -- it may show a party that is no more;
/// - a party seen full: no, ticked or not;
/// - with `always_leader` ("Chỉ mời khi đang làm chủ tổ đội") ticked, by the
///   crown, as the player decided on 2026-10-19: yes when the player's own
///   crown is seen, and yes for a player surely alone (PartyHud::sure_alone)
///   -- the invite starts a party the player leads, as GoC's does. No for a
///   member under another member's crown, for a party with no crown seen on
///   any panel, for a HUD that could not be read, and for a player read as
///   alone but not surely: a member whose party's panels are all hidden
///   reads as alone too. Each says why;
/// - with the tick off, anything but full -- alone, leading, a member, a
///   party whose leader could not be told, or a HUD that could not be read --
///   yes, as GoBot invited before it read the HUD.
struct PartyGate {
    bool        allowed = true;
    std::string reason_vi;          ///< why not, for the screen; empty when allowed
    std::string reason_en;          ///< the same for the log
};
PartyGate party_invite_gate(const PartyHud& hud, bool always_leader);

/// Whether a Tự chat line ticked "Chỉ khi chưa có tổ đội" (P2-5) may go on
/// this reading: only alone surely, by the rule the tick's invite goes by
/// (read_party) -- an advert asking people to join is no more to be typed by
/// a member whose party is hidden than an invite is to be sent. Alone and
/// not sure, the line waits, as in a party (alone_turn).
inline bool alone_for_chat(const PartyHud& hud) { return hud.state == PartyState::Alone && hud.sure_alone; }

/// Between the two looks Tự chat takes before a line ticked "Chỉ khi chưa có
/// tổ đội" -- the line goes only when both are surely alone, PartyEvents::
/// kSettleMs apart: the game looked at for an invitation with `sight` every
/// `every_ms` of its clock until `until`, each look noted in `events`
/// (PartyEvents::invitation). Whenever the party worker looks for none, Tự
/// chat is GoBot's only eye on the game, and an invitation the player answers
/// by hand in that while leaves a party the game has not drawn yet -- seen up
/// or gone meanwhile, the second look is not surely alone (read_party). `up`
/// tells whether one is up on a frame; `sleep_ms` waits on `sight`'s clock;
/// `stopped` cuts the watch short. False when it was cut short or a grab
/// failed: no second look is to be taken then. One up and answered within a
/// single `every_ms` is not seen; nothing that only looks at the screen can
/// see that.
bool watch_invitations(const PartySight& sight, const std::function<bool(const Frame&)>& up,
                       const std::function<void(std::uint32_t)>& sleep_ms, const std::function<bool()>& stopped,
                       PartyEvents& events, std::uint64_t until, std::uint32_t every_ms);

/// The words the screen uses, so the tab and the tests say the same thing.
namespace party_text {
inline const char* const kFull = "Tổ đội đã đủ 6 người";
inline const char* const kNotLeader = "Đang trong tổ đội nhưng không phải chủ tổ đội";
inline const char* const kUnread = "không đọc được khung tổ đội";
/// Ticked, in a party, and no crown on any panel.
inline const char* const kNoCrown = "Đang trong tổ đội nhưng không thấy vương miện chủ tổ đội ở ô nào";
/// Ticked, and the HUD not read: the player's own crown was not seen.
inline const char* const kUnreadNoCrown = "Không đọc được khung tổ đội nên không thấy vương miện của bạn";
/// Ticked, and read as alone but not surely: something up could be hiding a
/// party's panels. Followed by what, in brackets.
inline const char* const kAloneUnsure = "Có vẻ chưa có tổ đội, nhưng chưa chắc";
/// What, for each of the things PartyHudReader::read looks for.
inline const char* const kDoubtHidden = "một phần màn hình game bị cửa sổ khác che hoặc nằm ngoài màn hình";
inline const char* const kDoubtWindow = "đang có một cửa sổ mở trong game, có thể che khung tổ đội";
inline const char* const kDoubtDialog = "đang có một hộp thoại của game trên màn hình, có thể che khung tổ đội";
inline const char* const kDoubtTooltip = "đang có khung chú thích trên màn hình game, có thể che khung tổ đội";
inline const char* const kDoubtPlain = "chỗ khung tổ đội đầu tiên bị một mảng một màu che";
inline const char* const kDoubtNoPictures =
    "thiếu hình nút đóng cửa sổ hoặc hộp thoại của game để biết có cửa sổ nào đang mở không";
/// And for read_party's: a change to the party just now, the game perhaps
/// not having drawn it yet -- followed by what it was, after a colon -- a
/// party perhaps joined, which may take the game a while to draw -- the
/// same -- or the game's own word that the player is in a party.
inline const char* const kDoubtChanged = "tổ đội có thể vừa thay đổi, chờ game vẽ lại khung tổ đội";
inline const char* const kDoubtJoined = "có thể vừa vào một tổ đội mà game chưa vẽ khung tổ đội, chờ 20 giây";
inline const char* const kDoubtGameSays = "game báo bạn đang trong tổ đội";
/// Any frame grabbed before GoBot's last change to the party, ticked or not.
/// Followed by what the change was, in brackets.
inline const char* const kStale = "Tổ đội có thể đã thay đổi sau lần chụp màn hình này";
/// What the party worker's own changes, an invitation seen, and one gone
/// from the screen without GoBot answering it are called.
inline const char* const kEventAccepted = "vừa nhận lời mời vào tổ đội";
inline const char* const kEventDeclined = "vừa từ chối lời mời vào tổ đội";
inline const char* const kEventInvited = "vừa gửi lời mời vào tổ đội";
inline const char* const kEventInvitation = "có lời mời vào tổ đội trên màn hình";
inline const char* const kEventInvitationGone =
    "lời mời vào tổ đội vừa tắt mà GoBot không trả lời, có thể bạn đã nhận bằng tay";
/// The state card's alone when not sure, after "Tổ đội: một mình".
inline const char* const kCardUnsure = " (chưa chắc)";
} // namespace party_text

/// "Tổ đội: một mình", "Tổ đội: một mình (chưa chắc)", "Tổ đội: thành viên
/// (3/6)", "Tổ đội: chủ tổ đội (3/6)", "Tổ đội: đang trong tổ đội (3/6),
/// không thấy vương miện chủ tổ đội", "Tổ đội: không đọc được": the state
/// card's line. Alone is "chưa chắc" whenever it is not surely so: what the
/// tick's invite and Tự chat's "Chỉ khi chưa có tổ đội" go by.
std::string party_hud_line_vi(const PartyHud& hud);

/// One line for the log: the state, the size, and where each panel is.
std::string party_hud_text(const PartyHud& hud);

} // namespace gobot
