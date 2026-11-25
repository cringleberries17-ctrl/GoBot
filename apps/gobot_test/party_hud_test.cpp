// gobot_test -- the party HUD read off the game's screen, and whether an
// invite can work by it, by the player's rule of 2026-10-21: alone is the
// game showing no party -- no member's panel, no crown -- whatever else is on
// the screen. The look judged as it was grabbed, and a look from before
// GoBot's own last change to the party never used, on pretend games where a
// game is needed.
//
// Who leads goes by the crown the game draws on the corner of the leader's
// panel (the player's decision of 2026-10-19). Four saved frames of the live
// client have a party on them, another member leading with the crown on that
// member's panel; the rest are of a player alone, and must never read as a
// party. The other states are drawn: GoC's own pictures of the game's member
// panel and of the leader's mark -- the red frame with the crown on its
// corner, or either of the two alone -- onto frame.png and the live party's
// frames, with the bars, a name and a face in the panels as a live party has
// them; the other helper's one live picture of a leader's panel; and GoC's
// seal icons in the HUD's seal slot, over the player's own crown. Nothing is
// pressed anywhere.

#include "gobot/party_hud.hpp"
#include "gobot/auto_chat.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/player_list.hpp"
#include "gobot/react_wait.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

template <typename... Args>
std::string fmt(const char* f, Args... args) {
    char buf[512];
    std::snprintf(buf, sizeof buf, f, args...);
    return buf;
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// The build tree of the checkout these tests were built from, where the live
/// tests saved their rounds (live\shots, frozen\...). Never committed, and
/// never written by GoBot as it plays; what GoBot writes is read beside this
/// test, and only as saved (load_live_frame).
#ifdef GOBOT_SOURCE_DIR
const char* const kLiveBuild = GOBOT_SOURCE_DIR "/build/";
#else
const char* const kLiveBuild = "";
#endif

/// The saved frames of the live party, beside this test, where GoBot keeps
/// and prunes its debug frames -- read only as they were saved. Another
/// member leads: that member's panel at 0,106, the crown on its corner. The
/// first is the quick quest's look, nothing over the panels; the second the
/// trash cleaner's, the bag dragged over two more panels below it.
const char* const kPartyProbe = "quest_debug\\probe_20261016_234019_00.png";
const char* const kPartyUnderBag = "trash_debug\\20261016_223524_0_not_in_bin.png";
constexpr int kLiveLeaderY = 106;

/// Draw `pic` onto `f` at `x`, `y`, its see-through pixels left out.
void blit(Frame& f, const Frame& pic, int x, int y) {
    for (int py = 0; py < pic.height; ++py)
        for (int px = 0; px < pic.width; ++px) {
            const int fx = x + px, fy = y + py;
            if (fx < 0 || fy < 0 || fx >= f.width || fy >= f.height) continue;
            const std::uint8_t* s = &pic.pixels[(static_cast<std::size_t>(py) * pic.width + px) * 4];
            if (s[3] < TemplateMatcher::kOpaque) continue;
            std::uint8_t* d = &f.pixels[(static_cast<std::size_t>(fy) * f.width + fx) * 4];
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
}

/// `pic` drawn in the middle of `box` of `f` and cut to it, its see-through
/// pixels left out: an icon in its slot.
void blit_in(Frame& f, const Frame& pic, const ScreenRect& box) {
    const int x0 = box.x + (box.w - pic.width) / 2, y0 = box.y + (box.h - pic.height) / 2;
    for (int py = 0; py < pic.height; ++py)
        for (int px = 0; px < pic.width; ++px) {
            const int fx = x0 + px, fy = y0 + py;
            if (!box.contains(fx, fy) || fx >= f.width || fy >= f.height) continue;
            const std::uint8_t* s = &pic.pixels[(static_cast<std::size_t>(py) * pic.width + px) * 4];
            if (s[3] < TemplateMatcher::kOpaque) continue;
            std::uint8_t* d = &f.pixels[(static_cast<std::size_t>(fy) * f.width + fx) * 4];
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
}

/// A rectangle of one colour, given as red, green, blue.
void fill(Frame& f, int x, int y, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    for (int py = y; py < y + h; ++py)
        for (int px = x; px < x + w; ++px) {
            if (px < 0 || py < 0 || px >= f.width || py >= f.height) continue;
            std::uint8_t* d = &f.pixels[(static_cast<std::size_t>(py) * f.width + px) * 4];
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = 255;
        }
}

/// A plain dark frame with nothing on it.
Frame plain_frame(int w = 1280, int h = 720) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t i = 0; i < f.pixels.size(); i += 4) {
        f.pixels[i] = 30;
        f.pixels[i + 1] = 22;
        f.pixels[i + 2] = 20;
    }
    return f;
}

/// `r` of `f`, as a picture of its own.
Frame cut_of(const Frame& f, const ScreenRect& r) {
    Frame out;
    out.width = r.w;
    out.height = r.h;
    out.pixels.resize(static_cast<std::size_t>(r.w) * r.h * 4);
    for (int y = 0; y < r.h; ++y)
        std::copy_n(&f.pixels[(static_cast<std::size_t>(r.y + y) * f.width + r.x) * 4],
                    static_cast<std::size_t>(r.w) * 4, &out.pixels[static_cast<std::size_t>(y) * r.w * 4]);
    return out;
}

/// `f` with `r` of it see-through.
Frame without(const Frame& f, const ScreenRect& r) {
    Frame out = f;
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x) out.pixels[(static_cast<std::size_t>(y) * out.width + x) * 4 + 3] = 0;
    return out;
}

/// The first of `names` that `store` holds.
const Frame* first_in(const TemplateStore& store, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (const Frame* f = store.get(n)) return f;
    return nullptr;
}

bool same_pixels(const Frame& a, const Frame& b) {
    if (a.width != b.width || a.height != b.height) return false;
    for (std::size_t i = 0; i < a.pixels.size(); i += 4) {
        const std::uint8_t al = a.pixels[i + 3];
        if (al != b.pixels[i + 3]) return false;
        if (al != 0 && !std::equal(&a.pixels[i], &a.pixels[i] + 3, &b.pixels[i])) return false;
    }
    return true;
}

struct Kit {
    TemplateStore pictures;
    PartyHudReader reader;
    Frame panel, other, mine, live_leader;
    Frame close_x;                  ///< the client's close button, bag::kCloseButtons[0]: a window of the game open
    Frame other_crown, own_crown;   ///< the crowns alone: kCrown of the two marks
    Frame other_frame, own_frame;   ///< the red frames alone, their crowns left out
    Frame frame;                    ///< frame.png, 1280x720, from the live client
    bool ok = false;
    std::string why;
};

Kit& kit() {
    static Kit k;
    static const bool loaded = [] {
        std::string missing;
        if (!PartyHudReader::load_pictures(k.pictures, exe_relative("configs/templates.ini"), &missing) ||
            !k.reader.load(k.pictures, &missing)) {
            k.why = "a picture of the party HUD is not installed here: " + missing;
            return true;
        }
        // Under whichever of their names load_pictures found them.
        k.panel = *first_in(k.pictures, { "inven/1/Dialog_PartyInfo.spr/0", "inven/0/Dialog_PartyInfo.spr/0",
                                          "dialog_party_info" });
        k.other = *first_in(k.pictures, { "grabbed/party_crown_other", "inven/1/Party_King_Other.spr/0",
                                          "inven/0/Party_King_Other.spr/0" });
        k.mine = *first_in(k.pictures, { "grabbed/party_crown_mine", "inven/1/Party_King_My.spr/0",
                                         "inven/0/Party_King_My.spr/0" });
        k.other_crown = cut_of(k.other, PartyHudReader::kCrown);
        k.own_crown = cut_of(k.mine, PartyHudReader::kCrown);
        k.other_frame = without(k.other, PartyHudReader::kCrown);
        k.own_frame = without(k.mine, PartyHudReader::kCrown);
        TemplateStore::load_one("grabbed/party_leader_key2", exe_relative("configs/templates.ini"), k.live_leader);
        TemplateStore::load_one(bag::kCloseButtons[0], exe_relative("configs/templates.ini"), k.close_x);
        // Only as it was saved: gobot_cli --read rewrites frame.png.
        if (load_live_frame(exe_relative("frame.png"), k.frame) && (k.frame.width != 1280 || k.frame.height != 720))
            k.frame = Frame{};
        if (!k.frame.valid()) {
            k.why = "no frame.png from the live client here, or not the one these checks were written against";
            return true;
        }
        k.ok = true;
        return true;
    }();
    (void)loaded;
    return k;
}

bool skip_without_kit() {
    if (kit().ok) return false;
    skip("%s\n", kit().why.c_str());
    return true;
}

/// What of the leader's mark is drawn: none of it; the whole of it, as the
/// game draws it -- the red frame with the crown on its corner; the crown
/// alone; or the red frame alone.
enum class Mark { None, Whole, Crown, Frame };

/// `m` of the mark on the player's own panel, at the top-left corner.
void mark_own(Frame& f, Mark m) {
    const Kit& k = kit();
    if (m == Mark::Whole) blit(f, k.mine, 0, 0);
    if (m == Mark::Crown) blit(f, k.own_crown, PartyHudReader::kCrown.x, PartyHudReader::kCrown.y);
    if (m == Mark::Frame) blit(f, k.own_frame, 0, 0);
}

/// A member's panel as a live party shows it: the game's panel, its HP and MP
/// bars part full, a name in white, a face -- and `m` of the leader's mark
/// over it.
void draw_member(Frame& f, int x, int y, Mark m = Mark::None, int hp = 60, int mp = 80) {
    const Kit& k = kit();
    blit(f, k.panel, x, y);
    fill(f, x + 7, y + 27, hp * 80 / 100, 3, 220, 40, 40);
    fill(f, x + 7, y + 33, mp * 80 / 100, 3, 40, 120, 220);
    for (int i = 0; i < 9; ++i) fill(f, x + 14 + i * 7, y + 10, 4, 8, 250, 250, 250);   // a name
    fill(f, x + 93, y + 8, 28, 26, 200, 150, 90);                                     // a face
    if (m == Mark::Whole) blit(f, k.other, x, y);
    if (m == Mark::Crown) blit(f, k.other_crown, x + PartyHudReader::kCrown.x, y + PartyHudReader::kCrown.y);
    if (m == Mark::Frame) blit(f, k.other_frame, x, y);
}

/// The crown on the corner of a panel at `x`, `y` painted over in the name
/// box's grey, as though the game had not drawn it.
void paint_out_crown(Frame& f, int x, int y) {
    fill(f, x + PartyHudReader::kCrown.x, y + PartyHudReader::kCrown.y, PartyHudReader::kCrown.w,
         PartyHudReader::kCrown.h, 52, 56, 66);
}

std::string said(const PartyHud& h) { return party_hud_text(h); }

/// The panel nearest `y` among those read, or null.
const PartyPanel* panel_at(const PartyHud& h, int y) {
    for (const PartyPanel& p : h.panels)
        if (std::abs(p.at.y - y) <= 2) return &p;
    return nullptr;
}

void test_reading() {
    section("Party HUD: alone, a member, the leader, full (PH-A1, PH-A2)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        const PartyHud h = k.reader.read(k.frame);
        check(h.state == PartyState::Alone && h.size == 1 && !h.full && h.panels.empty() && !h.covered,
              "frame.png as it is: alone -- " + said(h));
    }
    {
        // A member of two: the other member's panel down the left, wearing
        // the leader's mark.
        Frame f = k.frame;
        draw_member(f, 0, 112, Mark::Whole);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Member && h.size == 2 && !h.full && h.panels.size() == 1 &&
                  h.panels[0].leader && std::abs(h.panels[0].at.x) <= 1 && std::abs(h.panels[0].at.y - 112) <= 1,
              "another member's panel with the leader's crown: member 2/6 -- " + said(h));
        check(party_hud_line_vi(h) == "Tổ đội: thành viên (2/6)", "said as " + party_hud_line_vi(h));
    }
    {
        // Leading three: the player's own panel with the mark, two members.
        Frame f = k.frame;
        mark_own(f, Mark::Whole);
        draw_member(f, 0, 112);
        draw_member(f, 0, 160, Mark::None, 30, 10);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Leader && h.own_leader && h.size == 3 && !h.full,
              "the crown on the own panel and two members: leader 3/6 -- " + said(h));
        check(party_hud_line_vi(h) == "Tổ đội: chủ tổ đội (3/6)", "said as " + party_hud_line_vi(h));
    }
    {
        Frame f = k.frame;
        mark_own(f, Mark::Whole);
        for (int i = 0; i < 5; ++i) draw_member(f, 0, 112 + i * 46, Mark::None, 20 * i, 100 - 15 * i);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Leader && h.size == 6 && h.full,
              "the crown on the own panel and five members: leader 6/6, full -- " + said(h));
    }
    {
        // A full party the player does not lead.
        Frame f = k.frame;
        for (int i = 0; i < 5; ++i) draw_member(f, 0, 112 + i * 46, i == 2 ? Mark::Whole : Mark::None);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Member && h.size == 6 && h.full,
              "five members, one wearing the crown: member 6/6, full -- " + said(h));
    }
    if (k.live_leader.valid()) {
        // The other helper's own picture of a leader's panel, cut off the
        // live client a pixel above the panel: found, and its crown with it.
        Frame f = k.frame;
        blit(f, k.live_leader, 0, 120);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Member && h.panels.size() == 1 && h.panels[0].leader &&
                  !h.panels[0].hidden,
              "the helper's live picture of a leader's panel: a member, led by that panel -- " + said(h));
    } else {
        skip("no grabbed/party_leader_key2 here\n");
    }
    {
        // At 1024x768, where the HUD is the same distance from the top-left.
        Frame shot;
        if (load_live_frame(exe_relative("playerlist.png"), shot) && shot.width == 1024) {
            check(k.reader.read(shot).state == PartyState::Alone, "playerlist.png (1024x768) as it is: alone");
            Frame member = shot;
            draw_member(member, 0, 112);
            draw_member(member, 0, 158, Mark::Whole);
            const PartyHud h = k.reader.read(member);
            check(h.state == PartyState::Member && h.size == 3,
                  "and two members drawn on it, the second crowned: member 3/6 -- " + said(h));
            Frame leading = shot;
            mark_own(leading, Mark::Whole);
            draw_member(leading, 0, 112);
            const PartyHud l = k.reader.read(leading);
            check(l.state == PartyState::Leader && l.size == 2 && l.own_crown >= 0.99f,
                  "and the crown on the own panel, one member: leader 2/6 -- " + said(l));
        } else {
            skip("no playerlist.png from the live client, as saved, here\n");
        }
    }
}

void test_crowns() {
    section("Party HUD: who leads, by the crown (P2-4, the player's rule of 2026-10-19)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        // The crown decides, on the player's own panel: the crown without
        // its red frame is the player leading; the red frame without its
        // crown is not, and the log says the two do not agree.
        Frame crowned = k.frame;
        mark_own(crowned, Mark::Crown);
        draw_member(crowned, 0, 112);
        const PartyHud c = k.reader.read(crowned);
        check(c.state == PartyState::Leader && c.own_leader && c.own_crown >= 0.99f && c.size == 2,
              "the own crown alone, one member: leader 2/6 -- " + said(c));
        Frame framed = k.frame;
        mark_own(framed, Mark::Frame);
        draw_member(framed, 0, 112);
        const PartyHud f = k.reader.read(framed);
        check(f.state == PartyState::InParty && !f.own_leader && f.own_crown < PartyHudReader::kCrownScore &&
                  f.own_frame >= PartyHudReader::kFrameScore &&
                  said(f).find("does not agree") != std::string::npos,
              "the own red frame without its crown, one member: in a party, no crown seen -- " + said(f));
    }
    {
        // And on a member's panel.
        Frame crowned = k.frame;
        draw_member(crowned, 0, 112, Mark::Crown);
        const PartyHud c = k.reader.read(crowned);
        check(c.state == PartyState::Member && c.panels.size() == 1 && c.panels[0].leader &&
                  c.panels[0].crown_score >= 0.99f,
              "a member's panel with the crown alone: member 2/6 -- " + said(c));
        Frame framed = k.frame;
        draw_member(framed, 0, 112, Mark::Frame);
        const PartyHud f = k.reader.read(framed);
        check(f.state == PartyState::InParty && f.panels.size() == 1 && !f.panels[0].leader &&
                  f.panels[0].frame_score >= PartyHudReader::kFrameScore,
              "a member's panel with the red frame alone: in a party, no crown seen -- " + said(f));
    }
    {
        // Neither: two plain panels and the player's own.
        Frame f = k.frame;
        draw_member(f, 0, 112);
        draw_member(f, 0, 160, Mark::None, 30, 10);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::InParty && h.size == 3 && !h.covered,
              "two members and no crown anywhere: in a party of 3 -- " + said(h));
        check(party_hud_line_vi(h) == "Tổ đội: đang trong tổ đội (3/6), không thấy vương miện chủ tổ đội",
              "said as " + party_hud_line_vi(h));
    }
    {
        // Covered: a window over a crowned member's panel but for its first
        // twenty columns. The panel is not found; its crown is, so that
        // member is there and leads -- and there may be more under it.
        Frame f = k.frame;
        draw_member(f, 0, 112, Mark::Whole);
        draw_member(f, 0, 158);
        fill(f, 20, 100, 300, 50, 200, 200, 210);
        const PartyHud h = k.reader.read(f);
        const PartyPanel* hidden = panel_at(h, 112);
        check(h.state == PartyState::Member && h.covered && h.size == 3 && hidden != nullptr && hidden->hidden &&
                  hidden->leader && party_hud_line_vi(h) == "Tổ đội: thành viên (ít nhất 3/6)",
              "a window over a crowned panel but for its crown: " + party_hud_line_vi(h) + " -- " + said(h));
        const PartyGate ticked = party_invite_gate(h, true), plain = party_invite_gate(h, false);
        check(!ticked.allowed && ticked.reason_vi == party_text::kNotLeader && plain.allowed,
              "and with the tick, not invited: \"" + ticked.reason_vi + "\"; without it, invited");
        // A crown with nothing at all under it but the window's own colour
        // is the same reading: nobody is taken for alone.
        Frame g = k.frame;
        draw_member(g, 0, 112, Mark::Whole);
        fill(g, 20, 100, 300, 50, 200, 200, 210);
        const PartyHud h2 = k.reader.read(g);
        check(h2.state == PartyState::Member && h2.covered && h2.size == 2,
              "the same with no other member: member, at least 2 -- " + said(h2));
    }

    // The live party, as saved: read as before, and the crown and the red
    // frame agree on it.
    Frame live;
    if (!load_live_frame(exe_relative(kPartyProbe), live) || live.width != 1280) {
        skip("no saved frame of the live party here\n");
        return;
    }
    {
        const PartyHud h = k.reader.read(live);
        const PartyPanel* p = panel_at(h, kLiveLeaderY);
        check(h.state == PartyState::Member && h.size == 2 && !h.covered && p != nullptr && p->leader &&
                  !p->hidden && p->crown_score >= 0.99f && p->frame_score >= PartyHudReader::kFrameScore &&
                  !h.own_leader && h.own_crown < PartyHudReader::kCrownScore &&
                  h.own_frame < PartyHudReader::kFrameScore,
              "the live party as saved: member 2/6, the crown and the red frame on the panel at 0,106 and on "
              "neither the own -- " + said(h));
    }
    {
        // Its crown painted over: the red frame is still round the panel, and
        // is no proof of who leads.
        Frame f = live;
        paint_out_crown(f, 0, kLiveLeaderY);
        const PartyHud h = k.reader.read(f);
        const PartyPanel* p = panel_at(h, kLiveLeaderY);
        check(h.state == PartyState::InParty && h.size == 2 && p != nullptr && !p->leader &&
                  p->frame_score >= PartyHudReader::kFrameScore,
              "the live party, the crown painted over: in a party, no crown seen -- " + said(h));
        const PartyGate ticked = party_invite_gate(h, true), plain = party_invite_gate(h, false);
        check(!ticked.allowed && ticked.reason_vi == party_text::kNoCrown && !ticked.reason_en.empty() &&
                  plain.allowed,
              "and with the tick, not invited: \"" + ticked.reason_vi + "\"; without it, invited");
        // And the player's own crown drawn on it: the player leads.
        Frame g = f;
        mark_own(g, Mark::Whole);
        const PartyHud l = k.reader.read(g);
        check(l.state == PartyState::Leader && l.size == 2 && l.own_crown >= 0.99f &&
                  party_invite_gate(l, true).allowed,
              "and the own crown drawn on it: leader 2/6, invited with the tick -- " + said(l));
    }
    {
        // The own crown drawn on it as it is: two crowns, which no party has.
        Frame f = live;
        mark_own(f, Mark::Whole);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Unknown && h.why.find("crowns") != std::string::npos,
              "the live party with a second crown, the player's own: unknown -- " + said(h));
    }
}

void test_crown_pictures() {
    section("Party HUD: the crowns GoBot ships (P2-4)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    // One crown in both of GoC's pictures: the same medallion, but for the
    // shading of the red frame down the left edge under it.
    int drawn = 0, differ = 0, differ_off_edge = 0;
    for (int y = 0; y < k.own_crown.height; ++y)
        for (int x = 0; x < k.own_crown.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * k.own_crown.width + x) * 4;
            const bool a = k.own_crown.pixels[i + 3] >= TemplateMatcher::kOpaque;
            drawn += a ? 1 : 0;
            const bool same = a == (k.other_crown.pixels[i + 3] >= TemplateMatcher::kOpaque) &&
                              std::equal(&k.own_crown.pixels[i], &k.own_crown.pixels[i] + 3,
                                         &k.other_crown.pixels[i]);
            differ += same ? 0 : 1;
            differ_off_edge += !same && x >= 4 ? 1 : 0;
        }
    check(k.own_crown.width == 19 && k.own_crown.height == 20 && drawn >= 300 && differ <= 16 &&
              differ_off_edge == 0,
          fmt("the crown, 19x20, %d pixels drawn; the two marks' differ in %d, all on the frame's left edge", drawn,
              differ));

    // GoBot's own copies are GoC's pictures, pixel for pixel, and the
    // shipped templates.ini names them, so that a package keeps them.
    const std::string ini = exe_relative("configs/templates.ini");
    const struct {
        const char* ours;
        const char* gocs;
    } copies[] = { { "grabbed/party_crown_mine", "inven/1/Party_King_My.spr/0" },
                   { "grabbed/party_crown_other", "inven/1/Party_King_Other.spr/0" } };
    for (const auto& c : copies) {
        Frame ours, gocs;
        std::string from;
        if (!TemplateStore::load_one(c.ours, ini, ours, &from)) {
            skip("no %s.png staged beside this test\n", c.ours);
            continue;
        }
        if (!TemplateStore::load_one(c.gocs, ini, gocs)) {
            skip("no %s.png here to compare %s with\n", c.gocs, c.ours);
            continue;
        }
        check(same_pixels(ours, gocs), std::string("the staged ") + c.ours + ".png is GoC's " + c.gocs +
                                           ".png, pixel for pixel (" + from + ")");
    }
#ifdef GOBOT_SOURCE_DIR
    const std::string names = read_all(std::string(GOBOT_SOURCE_DIR) + "/configs/templates.ini");
    check(names.find("party_crown_mine               = grabbed/party_crown_mine") != std::string::npos &&
              names.find("party_crown_other              = grabbed/party_crown_other") != std::string::npos,
          "templates.ini names both crowns, so that the package keeps them");
#endif

    // The reader takes them under GoBot's names or the game's, either skin.
    const Frame* plate = first_in(k.pictures, { "hpmp_indicator2", "grabbed/hpmp_indicator2" });
    const Frame* strip = first_in(k.pictures, { "grabbed/inventory", "inventory" });
    if (plate == nullptr || strip == nullptr) {
        skip("the Level plate or the bag's strip is not among the loaded pictures\n");
        return;
    }
    const auto store_with = [&](const char* other_name, const Frame& other, const char* mine_name,
                                const Frame& mine) {
        TemplateStore s;
        s.put("inven/0/Dialog_PartyInfo.spr/0", k.panel);
        s.put("hpmp_indicator2", *plate);
        s.put("grabbed/inventory", *strip);
        s.put(other_name, other);
        s.put(mine_name, mine);
        return s;
    };
    PartyHudReader ours, skin0, crownless;
    std::string missing;
    check(ours.load(store_with("grabbed/party_crown_other", k.other, "grabbed/party_crown_mine", k.mine)) &&
              skin0.load(store_with("inven/0/Party_King_Other.spr/0", k.other, "inven/0/Party_King_My.spr/0", k.mine)),
          "the reader loads from GoBot's copies alone, and from skin 0's pictures alone");
    const bool loaded = crownless.load(store_with("grabbed/party_crown_other", without(k.other, PartyHudReader::kCrown),
                                                  "grabbed/party_crown_mine", k.mine),
                                       &missing);
    check(!loaded && missing == "grabbed/party_crown_other",
          "and refuses a mark with no crown on its corner: missing " + missing);
}

void test_nonsense() {
    section("Party HUD: what no party looks like is not read as one");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        Frame f = k.frame;
        for (int i = 0; i < 6; ++i) draw_member(f, 0, 104 + i * 44);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Unknown && !h.full && h.size == 0,
              "six other panels, seven in all, more than a party holds: unknown, never full -- " + said(h));
    }
    {
        Frame f = k.frame;
        mark_own(f, Mark::Whole);
        draw_member(f, 0, 112, Mark::Whole);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Unknown, "two leaders' crowns: unknown -- " + said(h));
        Frame g = k.frame;
        draw_member(g, 0, 112, Mark::Crown);
        draw_member(g, 0, 160, Mark::Crown);
        check(k.reader.read(g).state == PartyState::Unknown, "two members' crowns: unknown");
    }
    {
        // The HUD gone: a member's panel on its own says nothing.
        Frame f = plain_frame();
        draw_member(f, 0, 112, Mark::Whole);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Unknown && h.why.find("HUD") != std::string::npos,
              "no HUD on screen: unknown -- " + said(h));
    }
    {
        // A panel half covered by a window is not counted, so a covered party
        // never reads as fuller than it is.
        Frame f = k.frame;
        mark_own(f, Mark::Whole);
        for (int i = 0; i < 5; ++i) draw_member(f, 0, 112 + i * 46);
        fill(f, 60, 112 + 2 * 46, 300, 60, 200, 200, 210);
        const PartyHud h = k.reader.read(f);
        check(!h.full && h.size < kPartyMax, "five members with one panel covered: not full -- " + said(h));
    }
    PartyHudReader empty;
    check(empty.read(k.frame).state == PartyState::Unknown, "a reader with no pictures: unknown");
    TemplateStore none;
    std::string missing;
    const bool loaded = empty.load(none, &missing);
    check(!loaded && !missing.empty(), "and loading from no pictures says what is missing: " + missing);
}

void test_leader_unread() {
    section("Party HUD: other members, and no crown anywhere (R2B-3, under the crown rule)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    // A party always has its one leader. Panels with no crown on any of them,
    // nor on the player's own, are a party whose leader was not seen: never a
    // member who does not lead, and never the leader either. Round 2 let the
    // tick invite on it, with a note; by the crown rule it does not.
    Frame f = k.frame;
    draw_member(f, 0, 112);
    draw_member(f, 0, 160, Mark::None, 30, 10);
    const PartyHud h = k.reader.read(f);
    check(h.state == PartyState::InParty && h.size == 3 && !h.full,
          "two plain panels and the player's own: in a party of 3, no crown seen -- " + said(h));
    const PartyGate ticked = party_invite_gate(h, true), plain = party_invite_gate(h, false);
    check(!ticked.allowed && ticked.reason_vi == "Đang trong tổ đội nhưng không thấy vương miện chủ tổ đội ở ô nào" &&
              ticked.reason_en.find("crown") != std::string::npos,
          "the tick on: not invited, the tab told \"" + ticked.reason_vi + "\", the log \"" + ticked.reason_en +
              "\"");
    check(plain.allowed && plain.reason_vi.empty(), "the tick off: invited, as before");
    // Tự chat's tick goes by "alone" only: a party whose leader is not known
    // is a party.
    check(h.state != PartyState::Alone, "and to Tự chat's tick it is a party, not alone");
    // Full still stops it: six seen is six, whoever leads.
    Frame six = k.frame;
    for (int i = 0; i < 5; ++i) draw_member(six, 0, 112 + i * 46, Mark::None, 20 * i, 100 - 15 * i);
    const PartyHud h6 = k.reader.read(six);
    const PartyGate g6 = party_invite_gate(h6, false);
    check(h6.state == PartyState::InParty && h6.full && !g6.allowed && g6.reason_vi == "Tổ đội đã đủ 6 người",
          "five plain panels: full, and not invited, whoever leads -- " + said(h6));
}

/// `rect` of `from` copied into `to` at the same place: a window of the live
/// client's laid over another frame, as the game draws it over the HUD.
void copy_over(const Frame& from, Frame& to, const ScreenRect& rect) {
    for (int y = std::max(0, rect.y); y < std::min({ rect.bottom(), from.height, to.height }); ++y)
        for (int x = std::max(0, rect.x); x < std::min({ rect.right(), from.width, to.width }); ++x)
            std::copy_n(&from.pixels[(static_cast<std::size_t>(y) * from.width + x) * 4], 4,
                        &to.pixels[(static_cast<std::size_t>(y) * to.width + x) * 4]);
}

void test_bag_over() {
    section("Party HUD: the bag over the panels (the 2B tester's problem 4)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    // Two frames of the live client with the bag open: where the player keeps
    // it, its left edge at x=121 over the panels' last columns, and dragged to
    // the screen's left edge, over all but their first few.
    Frame kept, dragged;
    if (!load_png(std::string(kLiveBuild) + "live\\shots\\r3_B_000.png", kept) ||
        !load_live_frame(exe_relative(kPartyUnderBag), dragged) || kept.width != 1280 ||
        dragged.width != 1280) {
        skip("no r3_B_000.png or the saved party frame here\n");
        return;
    }
    ScreenRect kept_bag, dragged_bag;
    check(k.reader.bag_window(kept, kept_bag) && kept_bag.x == 134 - PartyHudReader::kBagLeft &&
              k.reader.bag_window(dragged, dragged_bag) && dragged_bag.x == 27 - PartyHudReader::kBagLeft,
          "the bag's window found by its tab strip, where the player keeps it and dragged left");

    {
        // Two members under the bag where the player keeps it: their last
        // nine columns hidden, the rest read, the crown on the first.
        Frame f = k.frame;
        draw_member(f, 0, 112, Mark::Whole);
        draw_member(f, 0, 158);
        copy_over(kept, f, kept_bag);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Member && h.size == 3 && h.panels.size() == 2 && h.panels[0].leader &&
                  !h.covered,
              "two members, the bag where the player keeps it over their right edge: member 3/6 -- " + said(h));
    }
    {
        // Nobody, and the bag where the player keeps it: alone, as ever.
        Frame f = k.frame;
        copy_over(kept, f, kept_bag);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Alone && !h.covered, "alone, the bag where the player keeps it: alone -- " +
                                                              said(h));
    }
    {
        // The bag dragged over the panels, none of them showing but their
        // first eleven columns -- too little of a crown to be one: no telling
        // whether anybody is there, so never "alone", and Tự chat's "only
        // when alone" line is not sent on it.
        Frame f = k.frame;
        draw_member(f, 0, 160, Mark::Whole);
        draw_member(f, 0, 206);
        copy_over(dragged, f, dragged_bag);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Unknown && h.why.find("bag") != std::string::npos,
              "the bag dragged over two members' panels: unknown, never alone -- " + said(h));
        Frame g = k.frame;
        copy_over(dragged, g, dragged_bag);
        const PartyHud h2 = k.reader.read(g);
        check(h2.state == PartyState::Unknown, "nor with nobody under it -- " + said(h2));
        // Unread, the player's own crown is not seen: with the tick, no
        // invite; without it, an invite, as before the HUD was read.
        const PartyGate ticked = party_invite_gate(h, true), plain = party_invite_gate(h, false);
        check(!ticked.allowed && ticked.reason_vi == party_text::kUnreadNoCrown &&
                  ticked.reason_en.find("bag") != std::string::npos && plain.allowed,
              "and with the tick, not invited: \"" + ticked.reason_vi + "\" (" + ticked.reason_en +
                  "); without it, invited");
    }
    {
        // A member seen above the dragged bag, more maybe under it: a party of
        // at least two, never full on what cannot be seen.
        const PartyHud h = k.reader.read(dragged);
        check(h.state == PartyState::Member && h.covered && h.size == 2 && !h.full &&
                  party_hud_line_vi(h) == "Tổ đội: thành viên (ít nhất 2/6)",
              "the saved party, the bag over two more panels: " + party_hud_line_vi(h) + " -- " + said(h));
    }
}

/// A window of the game over `body` of `f`: a busy ground -- no one colour,
/// no tooltip's border, nothing like a member's panel -- and the client's
/// close button `x` at its top-right corner, where the game puts one. Where
/// that button is.
ScreenRect game_window(Frame& f, const ScreenRect& body, const Frame& x) {
    for (int y = body.y; y < body.bottom(); ++y)
        for (int px = body.x; px < body.right(); ++px) {
            const int dx = px - body.x, dy = y - body.y;
            fill(f, px, y, 1, 1, static_cast<std::uint8_t>(60 + (dx * 7 + dy * 3) % 90),
                 static_cast<std::uint8_t>(50 + (dx * 3 + dy * 11) % 70), static_cast<std::uint8_t>(40 + (dx * dy) % 60));
        }
    const ScreenRect at{ body.right() - x.width - 6, body.y + 6, x.width, x.height };
    blit(f, x, at.x, at.y);
    return at;
}

/// Another program's window over `r` of `f` -- GoBot's own, or a browser --
/// as a frame read off the screen has it: a dark ground, rows of light text,
/// a few blue rules.
void other_program(Frame& f, const ScreenRect& r) {
    fill(f, r.x, r.y, r.w, r.h, 37, 37, 38);
    for (int y = r.y + 20; y < r.bottom(); y += 22) fill(f, r.x + 12, y, std::max(0, r.w - 40), 9, 200, 200, 200);
    for (int y = r.y + 10; y < r.bottom(); y += 66) fill(f, r.x + 8, y, std::max(0, r.w - 16), 2, 66, 150, 250);
}

void test_party_under_window() {
    section("Party HUD: a party under a window of the game reads as alone -- the player's rule of 2026-10-21 takes "
            "that, and the notes say to close such a window when it matters");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    Frame live;
    if (!load_live_frame(exe_relative(kPartyProbe), live) || live.width != 1280) {
        skip("no saved frame of the live party here\n");
        return;
    }
    {
        // The live party of two, another member leading, with a window of the
        // game over the leader's panel from the screen's left edge, crown and
        // all, its X at its top right. Nothing of the party shows, and no tab
        // strip says a bag hides it: the game shows no party, and that is
        // alone -- with the tick, an invite goes. The rule's accepted cost.
        Frame f = live;
        ScreenRect at{};
        if (k.close_x.valid()) at = game_window(f, ScreenRect{ 0, 90, 300, 310 }, k.close_x);
        else fill(f, 0, 100, 150, 55, 200, 200, 210);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Alone && h.panels.empty() && !h.own_leader &&
                  party_invite_gate(h, true).allowed && party_invite_gate(h, false).allowed,
              fmt("the live party under a window of the game (its X at %d,%d): alone, and invited with the tick -- ",
                  at.x, at.y) + said(h));
    }
    {
        // The same window from twenty pixels in, the crown left showing: the
        // member who leads is seen by the crown, and the tick invites nobody.
        Frame f = live;
        fill(f, 20, 100, 130, 55, 200, 200, 210);
        const PartyHud h = k.reader.read(f);
        const PartyPanel* p = panel_at(h, kLiveLeaderY);
        check(h.state == PartyState::Member && h.covered && p != nullptr && p->hidden && p->leader &&
                  !party_invite_gate(h, true).allowed,
              "the window from x=20, the crown showing: a member, not invited with the tick -- " + said(h));
    }
    {
        // Another program's window over the game's left side, read off the
        // screen: no reading at all, never alone (party_frame_trusted).
        Frame f = live;
        other_program(f, ScreenRect{ 0, 60, 420, 600 });
        const PartyHud h = k.reader.read(f, false);
        check(h.state == PartyState::Unknown && !party_invite_gate(h, true).allowed,
              "another program over the game's left side, read off the screen: not read, not invited with the "
              "tick -- " + said(h));
    }
}

void test_own_crown_under_slot() {
    section("Party HUD: the player's own crown with the HUD's seal slot drawn over it (R5-2)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    const ScreenRect& slot = PartyHudReader::kSealSlot;
    // No saved frame shows whether the game draws the slot's icon over the
    // player's own crown or under it; over it, the crown still says the
    // player leads. First the slot as the live client drew it, its ground and
    // this player's own seal, over the whole mark, at both client sizes.
    Frame small;
    if (!load_live_frame(exe_relative("playerlist.png"), small))
        skip("no playerlist.png the checks were written against here (none has its hash recorded yet)\n");
    const Frame* const shots[] = { &k.frame, &small };
    for (const Frame* shot : shots) {
        if (!shot->valid() || shot->width < 1024) continue;
        Frame f = *shot;
        mark_own(f, Mark::Whole);
        copy_over(*shot, f, slot);
        draw_member(f, 0, 112);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Leader && h.own_crown >= 0.99f && h.size == 2,
              fmt("the slot as the live client drew it (%dx%d), over the own crown: leader 2/6 -- ", shot->width,
                  shot->height) + said(h));
    }

    // Then each of GoC's seals, what the slot holds, over the crown and
    // alone in the slot: staged beside this test, else in GoC's own folder.
    std::string dir;
    for (const std::string& d : { TemplateStore::default_dir(), TemplateStore::client_images() })
        if (dir.empty() && !d.empty() && !files_in(d + "\\seal\\", "*.png").empty()) dir = d + "\\seal\\";
    if (dir.empty()) {
        skip("no seal icons here, staged or in GoC's folder\n");
        return;
    }
    std::vector<std::string> names = files_in(dir, "*.png");
    std::sort(names.begin(), names.end());
    int seals = 0, led = 0, bare_alone = 0;
    float least = 2.0f, most_bare = -1.0f;
    std::string least_at, first_wrong, lowest_said;
    bool lowest_led = false;
    for (const std::string& n : names) {
        Frame icon;
        if (!load_png(dir + n, icon) || !icon.valid()) continue;
        ++seals;
        Frame over = k.frame;
        mark_own(over, Mark::Whole);
        blit_in(over, icon, slot);
        draw_member(over, 0, 112);
        const PartyHud h = k.reader.read(over);
        if (h.state == PartyState::Leader) ++led;
        else if (first_wrong.empty()) first_wrong = n + ": " + said(h);
        if (h.own_crown < least) {
            least = h.own_crown;
            least_at = n;
        }
        // The seal that took the whole crown lowest, to 0.63.
        if (n == "8590004.png") {
            lowest_said = said(h);
            lowest_led = h.state == PartyState::Leader && h.size == 2;
        }
        // And alone in the slot, no crown: nobody leads.
        Frame bare = k.frame;
        blit_in(bare, icon, slot);
        const PartyHud b = k.reader.read(bare);
        bare_alone += b.state == PartyState::Alone && !b.own_leader ? 1 : 0;
        most_bare = std::max(most_bare, b.own_crown);
    }
    check(seals > 0 && led == seals,
          fmt("%d of %d seals drawn over the own crown: leader, the crown %.2f at least (%s)", led, seals,
              static_cast<double>(least), least_at.c_str()) +
              (first_wrong.empty() ? std::string() : " -- first not: " + first_wrong));
    if (!lowest_said.empty())
        check(lowest_led, "seal 8590004 over the own crown: leader 2/6 -- " + lowest_said);
    check(seals > 0 && bare_alone == seals && most_bare < PartyHudReader::kCrownScore,
          fmt("%d of %d seals alone in the slot, no crown: alone, the own corner %.2f at most", bare_alone, seals,
              static_cast<double>(most_bare)));
}

void test_trust() {
    section("Party HUD: a frame read off the screen, trusted by the part of the game it reads");
    // The pure half: whether an area lies on the screens.
    const std::vector<AutoLogin::Box> one{ { 0, 0, 1920, 1080 } };
    const std::vector<AutoLogin::Box> two{ { 0, 0, 1920, 1080 }, { 1920, 0, 3200, 1024 } };
    check(AutoLogin::on_screens({ 10, 10, 330, 490 }, one), "all of it on the one screen: on");
    check(!AutoLogin::on_screens({ -40, 10, 280, 490 }, one), "40 pixels of it past the left edge: off");
    check(!AutoLogin::on_screens({ 10, 900, 330, 1380 }, one), "its bottom past the screen's: off");
    check(AutoLogin::on_screens({ 1800, 100, 2120, 580 }, two), "across two screens side by side: on");
    check(!AutoLogin::on_screens({ 1800, 900, 2120, 1380 }, two),
          "across two, its corner where neither reaches: off");
    check(!AutoLogin::on_screens({ 10, 10, 10, 490 }, one), "nothing at all: off");

    // A window of this test's own, put up off every screen and never brought
    // forward: none of it is seen, so nothing read off the screen for it is
    // trusted.
    const HINSTANCE self = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = self;
    wc.lpszClassName = "GoBotTestPartyWindow";
    RegisterClassA(&wc);        // already there from an earlier call: the same
    const int far_x = GetSystemMetrics(SM_XVIRTUALSCREEN) - 4000;
    const int far_y = GetSystemMetrics(SM_YVIRTUALSCREEN) - 4000;
    HWND w = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "GoBot test window",
                             WS_POPUP, far_x, far_y, 400, 500, nullptr, nullptr, self, nullptr);
    if (w == nullptr) {
        skip("could not put up a window of this test's own here\n");
        return;
    }
    ShowWindow(w, SW_SHOWNOACTIVATE);
    std::string why;
    const bool seen = AutoLogin::part_seen(w, PartyHudReader::kArea, &why);
    check(!seen && why == "off the screen", "the party's part of a window off every screen: not seen -- " + why);
    check(!party_frame_trusted(true, w) && party_frame_trusted(false, w),
          "a frame of it off the screen: not trusted; one it drew itself: trusted");
    check(!AutoLogin::part_seen(w, ScreenRect{ 500, 600, 50, 50 }) &&
              !AutoLogin::part_seen(nullptr, PartyHudReader::kArea),
          "a part outside the window, or no window: not seen");
    DestroyWindow(w);
}

void test_real_frames() {
    section("Party HUD: the saved live frames, and the ones with a party (PH-A3, PH-A4)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        // r2_B_000 was read off the screen with a terminal over most of the
        // game: its Level plate still shows, and the left of the screen, where
        // panels would be, is the terminal. No look at pixels can tell that
        // from a player alone; the capture can -- it came off the screen with
        // a window over that part of the game -- and such a frame is never
        // trusted.
        Frame covered;
        if (load_png(std::string(kLiveBuild) + "live\\shots\\r2_B_000.png", covered)) {
            const PartyHud h = k.reader.read(covered, party_frame_trusted(true, nullptr));
            check(h.state == PartyState::Unknown, "r2_B_000.png, the game covered: unknown -- " + said(h));
            // And by its pixels alone, read as though it had been trusted:
            // the terminal's plain ground down the panels' column is no
            // reading either (the round 2 tester's problem 2).
            const PartyHud by_pixels = k.reader.read(covered, true);
            check(by_pixels.state == PartyState::Unknown && by_pixels.why.find("plain") != std::string::npos,
                  "the same read as trusted: unknown by its pixels -- " + said(by_pixels));
            // The others read off the screen with a window over the game,
            // the HUD's plate still showing above it: the same.
            int plain = 0, plated = 0;
            for (const char* name : { "r1_before", "r1_pre", "r2_before", "r2_B_001", "r3_D_000", "r3_D_001",
                                      "r3_D_002", "r3_D_003", "r3_pre" }) {
                Frame f;
                if (!load_png(std::string(kLiveBuild) + "live\\shots\\" + name + ".png", f)) continue;
                ++plated;
                plain += k.reader.read(f, true).state == PartyState::Unknown ? 1 : 0;
            }
            check(plated > 0 && plain == plated,
                  std::to_string(plain) + " of " + std::to_string(plated) +
                      " more frames with a program's window over the game, read as trusted: unknown");
        } else {
            skip("no live\\shots\\r2_B_000.png here\n");
        }
        check(party_frame_trusted(false, nullptr) && !party_frame_trusted(true, nullptr),
              "a frame the game drew itself is trusted; one off the screen with no game window to look at "
              "is not");
    }
    // Saved frames that do show a party: read as what they show, and left out
    // of the count below. The first came on 2026-10-16, after the frames below
    // had been gathered all alone: the trash cleaner kept it in a party that
    // another player led. Each is read only as it was saved: GoBot keeps and
    // prunes its debug frames as it plays, and one of the player's own since
    // is no measure of the code.
    struct PartyFrame {
        const char* path;       ///< beside this test
        PartyState  state;
        int         size;       ///< as many as are seen
        bool        covered;    ///< the bag over some of the panels
    };
    // The first has two more panels under the bag, dragged to the screen's
    // left edge, only their left ends showing: "at least" two. The other
    // three, kept by the quick quest's look an hour later, are a party of two
    // with nothing over its panels. In all four another member leads: the
    // crown on that member's panel at 0,106, and the red frame round it.
    const PartyFrame party_frames[] = {
        { kPartyUnderBag, PartyState::Member, 2, true },
        { kPartyProbe, PartyState::Member, 2, false },
        { "quest_debug\\probe_20261016_234032_01.png", PartyState::Member, 2, false },
        { "quest_debug\\probe_20261016_234045_02.png", PartyState::Member, 2, false },
    };
    const auto shows_a_party = [&](const std::string& path) {
        for (const PartyFrame& p : party_frames)
            if (path == exe_relative(p.path)) return true;
        return false;
    };
    for (const PartyFrame& p : party_frames) {
        Frame f;
        if (!load_live_frame(exe_relative(p.path), f)) {
            skip("no %s here, or not as it was saved", p.path);
            continue;
        }
        const PartyHud h = k.reader.read(f);
        // One leader in a party, by the crown: here another member, on the
        // panel at 0,106 -- and the red frame agrees, round that panel and
        // not round the player's own.
        int leaders = h.own_leader ? 1 : 0;
        for (const PartyPanel& panel : h.panels) leaders += panel.leader ? 1 : 0;
        const PartyPanel* lead = panel_at(h, kLiveLeaderY);
        const bool agree = lead != nullptr && lead->leader && !lead->hidden && lead->crown_score >= 0.99f &&
                           lead->frame_score >= PartyHudReader::kFrameScore &&
                           h.own_crown < PartyHudReader::kCrownScore && h.own_frame < PartyHudReader::kFrameScore;
        check(h.state == p.state && h.size == p.size && !h.full && leaders == 1 && h.covered == p.covered && agree,
              std::string(p.path) + ", saved live in a party: " + party_state_name(p.state) + ", " +
                  (p.covered ? "at least " : "") + std::to_string(p.size) + " of " + std::to_string(kPartyMax) +
                  ", led by the crowned panel at 0,106, the red frame agreeing -- " + said(h));
    }

    // Every eighth frame; GOBOT_PARTY_ALL=1 for every one of them.
    char all[8] = {};
    const bool every = GetEnvironmentVariableA("GOBOT_PARTY_ALL", all, sizeof all) > 0 && all[0] == '1';
    // Each with whether it is held to its hash: the live rounds come from the
    // checkout's build, which GoBot never writes; the trash cleaner's frames,
    // frame.png and playerlist.png from beside this test, only as saved.
    std::vector<std::pair<std::string, bool>> frames;
    for (const char* dir : { "live\\shots\\", "frozen\\live\\shots\\" }) {
        std::vector<std::string> names = files_in(std::string(kLiveBuild) + dir, "*.png");
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) frames.emplace_back(std::string(kLiveBuild) + dir + n, false);
    }
    {
        std::vector<std::string> names = files_in(exe_relative("trash_debug\\"), "*.png");
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) frames.emplace_back(exe_relative("trash_debug\\") + n, true);
    }
    frames.emplace_back(exe_relative("frame.png"), true);
    frames.emplace_back(exe_relative("playerlist.png"), true);
    int looked = 0, alone = 0, unknown = 0, party = 0, crowned = 0;
    std::string first_party;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!every && i % 8 != 0 && i + 2 < frames.size()) continue;
        const auto& [path, held] = frames[i];
        if (shows_a_party(path)) continue;     // read above, as what it shows
        Frame f;
        if (!(held ? load_live_frame(path, f) : load_png(path, f)) || f.width < 640 || f.height < 480) continue;
        ++looked;
        const PartyHud h = k.reader.read(f);
        if (h.own_crown >= PartyHudReader::kCrownScore) ++crowned;
        if (h.state == PartyState::Alone) {
            ++alone;
        } else if (h.state == PartyState::Unknown) {
            ++unknown;
        } else {
            ++party;
            if (first_party.empty()) first_party = path + ": " + said(h);
        }
    }
    if (looked == 0) {
        skip("no saved frames of the live client that these checks were written against here\n");
        return;
    }
    // PH-A4 as the saved frames have it now: those that show a party are read
    // as one above; every other is never read as one -- not a member, not a
    // leader, never full -- and nowhere on them is a crown.
    check(party == 0 && crowned == 0 && alone > 0,
          std::to_string(looked) + " saved frames that show no party: " + std::to_string(alone) + " alone, " +
              std::to_string(unknown) + " unknown, " + std::to_string(party) + " read as a party, " +
              std::to_string(crowned) + " with the own crown" +
              (first_party.empty() ? std::string() : " -- first: " + first_party));
}

void test_gate() {
    section("Party HUD: an invite only when it can work (PH-A5); with the tick, the own crown or no party shown "
            "(the player's rule of 2026-10-21)");
    PartyHud alone, member, leader, in_party, unknown, full_lead, full_member, full_in_party, stale_alone,
        stale_lead;
    alone.state = PartyState::Alone;
    alone.size = 1;
    member.state = PartyState::Member;
    member.size = 3;
    leader.state = PartyState::Leader;
    leader.size = 3;
    leader.own_leader = true;
    in_party.state = PartyState::InParty;
    in_party.size = 3;
    unknown.why = "the HUD is not on screen";
    full_lead = leader;
    full_lead.size = 6;
    full_lead.full = true;
    full_member = member;
    full_member.size = 6;
    full_member.full = true;
    full_in_party = in_party;
    full_in_party.size = 6;
    full_in_party.full = true;
    // A frame grabbed before GoBot's last change to the party, however it
    // reads (read_party sets it).
    stale_alone = alone;
    stale_alone.stale = true;
    stale_alone.stale_en = "an invitation was accepted";
    stale_alone.stale_vi = party_text::kEventAccepted;
    stale_lead = leader;
    stale_lead.stale = true;
    stale_lead.stale_en = "an invite was sent";
    stale_lead.stale_vi = party_text::kEventInvited;
    struct Row {
        const char*     what;
        const PartyHud* hud;
        bool            off;            ///< invited with the tick off
        const char*     off_vi;         ///< and why not
        bool            on;             ///< with the tick on
        const char*     on_vi;
    };
    // Alone, ticked, is invited: the HUD shows no party, and the invite
    // starts one the player leads, as GoC's does (the player's rule of
    // 2026-10-21: alone is not being in a party, as the game shows it).
    const std::string stale_accepted = std::string(party_text::kStale) + " (" + party_text::kEventAccepted + ")";
    const std::string stale_invited = std::string(party_text::kStale) + " (" + party_text::kEventInvited + ")";
    const Row rows[] = {
        { "alone, on a frame from before an accept", &stale_alone, false, stale_accepted.c_str(), false,
          stale_accepted.c_str() },
        { "leading, on a frame from before an invite", &stale_lead, false, stale_invited.c_str(), false,
          stale_invited.c_str() },
        { "alone", &alone, true, "", true, "" },
        { "leading, the own crown seen", &leader, true, "", true, "" },
        { "a member, another's crown seen", &member, true, "", false, party_text::kNotLeader },
        { "in a party, no crown seen", &in_party, true, "", false, party_text::kNoCrown },
        { "the HUD not read", &unknown, true, "", false, party_text::kUnreadNoCrown },
        { "full, leading", &full_lead, false, party_text::kFull, false, party_text::kFull },
        { "full, a member", &full_member, false, party_text::kFull, false, party_text::kFull },
        { "full, no crown seen", &full_in_party, false, party_text::kFull, false, party_text::kFull },
    };
    for (const Row& r : rows)
        for (const bool tick : { false, true }) {
            const PartyGate g = party_invite_gate(*r.hud, tick);
            const bool want = tick ? r.on : r.off;
            const std::string want_vi = tick ? r.on_vi : r.off_vi;
            check(g.allowed == want && g.reason_vi == want_vi && g.reason_en.empty() == want,
                  std::string(r.what) + (tick ? ", ticked: " : ", not ticked: ") +
                      (g.allowed ? "invited" : "not invited, \"" + g.reason_vi + "\" / \"" + g.reason_en + "\""));
        }
    check(party_invite_gate(unknown, true).reason_en.find("not on screen") != std::string::npos,
          "the HUD not read, ticked: the log says why it was not read");
    check(std::string(party_text::kNotLeader) == "Đang trong tổ đội nhưng không phải chủ tổ đội" &&
              std::string(party_text::kNoCrown) ==
                  "Đang trong tổ đội nhưng không thấy vương miện chủ tổ đội ở ô nào" &&
              std::string(party_text::kUnreadNoCrown) ==
                  "Không đọc được khung tổ đội nên không thấy vương miện của bạn",
          "the tab's words for each");
    // The card says what the HUD reads, plainly: no "chưa chắc" any more.
    check(party_hud_line_vi(unknown) == "Tổ đội: không đọc được" && party_hud_line_vi(alone) == "Tổ đội: một mình" &&
              party_hud_line_vi(stale_alone) == "Tổ đội: một mình" &&
              party_hud_line_vi(alone).find("chưa chắc") == std::string::npos,
          "the state card: \"" + party_hud_line_vi(unknown) + "\", \"" + party_hud_line_vi(alone) + "\"");
}

void test_worker_step() {
    section("Party HUD: the worker clicks nothing when the gate says no (PH-A6)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    // A pretend party worker's step for one asker, as party_loop takes it:
    // the gate on the frame it already has, and only then the player list
    // and the invite -- which here only count.
    int opened = 0, invited = 0;
    const auto step = [&](const Frame& f, bool always_leader, bool trusted = true) {
        const PartyGate g = party_invite_gate(k.reader.read(f, trusted), always_leader);
        if (!g.allowed) return g.reason_en;
        ++opened;           // the player list's ensure_open
        ++invited;          // its invite
        return std::string();
    };
    Frame full = k.frame;
    mark_own(full, Mark::Whole);
    for (int i = 0; i < 5; ++i) draw_member(full, 0, 112 + i * 46);
    const std::string why = step(full, false);
    check(opened == 0 && invited == 0 && !why.empty(),
          "a full party: the list never opened, nobody invited -- " + why);
    Frame member = k.frame;
    draw_member(member, 0, 112, Mark::Whole);
    step(member, true);
    check(opened == 0 && invited == 0, "a member with the tick on: still nothing");
    Frame no_crown = k.frame;
    draw_member(no_crown, 0, 112);
    step(no_crown, true);
    check(opened == 0 && invited == 0, "in a party with no crown seen, the tick on: still nothing");
    step(k.frame, true, false);
    check(opened == 0 && invited == 0, "the HUD not read, the tick on: still nothing");
    step(member, false);
    check(opened == 1 && invited == 1, "a member with the tick off: the list opened and the invite sent, as today");
    step(k.frame, false);
    check(opened == 2 && invited == 2, "alone, the tick off: invited, as today");
    Frame leading = k.frame;
    mark_own(leading, Mark::Whole);
    draw_member(leading, 0, 112);
    step(leading, true);
    check(opened == 3 && invited == 3, "leading, the own crown seen, the tick on: invited");
    step(k.frame, false, false);
    check(opened == 4 && invited == 4, "the HUD not read, the tick off: invited, as before");
    // Alone -- frame.png as it is, no party shown -- with the tick: invited,
    // to start a party, as GoC does.
    const std::string alone_why = step(k.frame, true);
    check(opened == 5 && invited == 5 && alone_why.empty(),
          "alone, the tick on: the list opened and the invite sent, to start a party");
    // And alone with a window of the game open somewhere: the game shows no
    // party all the same, and the tick invites (the player's rule).
    Frame window_open = k.frame;
    if (k.close_x.valid()) blit(window_open, k.close_x, 900, 200);
    const std::string window_why = step(window_open, true);
    check(k.close_x.valid() && opened == 6 && invited == 6 && window_why.empty(),
          "alone, a window of the game open somewhere (its X at 900,200), the tick on: invited -- the game shows no "
          "party");
}

// ------------------------------------------------------------------ round 7b

/// A tooltip as the game draws one, at `x`,`y`: a dark box with the border
/// three pixels thick in its fixed colours, outside in, and rows of light
/// words in it. Where it is.
ScreenRect draw_tooltip(Frame& f, int x, int y, int w = 150, int h = 90) {
    fill(f, x, y, w, h, 24, 24, 44);
    const std::uint32_t rings[] = { 0x838383, 0x22223F, 0x353557 };
    for (int i = 0; i < 3; ++i) {
        const auto r = static_cast<std::uint8_t>((rings[i] >> 16) & 0xFF);
        const auto g = static_cast<std::uint8_t>((rings[i] >> 8) & 0xFF);
        const auto b = static_cast<std::uint8_t>(rings[i] & 0xFF);
        fill(f, x + i, y + i, w - 2 * i, 1, r, g, b);
        fill(f, x + i, y + h - 1 - i, w - 2 * i, 1, r, g, b);
        fill(f, x + i, y + i, 1, h - 2 * i, r, g, b);
        fill(f, x + w - 1 - i, y + i, 1, h - 2 * i, r, g, b);
    }
    for (int row = 0; row < 4; ++row)
        for (int i = 0; i < 12; ++i) fill(f, x + 10 + i * 10, y + 12 + row * 18, 6, 8, 230, 230, 230);
    return ScreenRect{ x, y, w, h };
}

/// A sight of a frame that is always all in view, on a clock that each grab
/// winds on 30 ms: the game drawing its own picture, or read off the screen
/// with nothing over it.
PartySight still_sight(const Frame& f, std::uint64_t& t, bool from_screen) {
    PartySight s;
    s.grab = [&f, &t, from_screen](Frame& out, bool& fs) {
        out = f;
        fs = from_screen;
        t += 30;
        return true;
    };
    s.seen = [](const ScreenRect&, std::string* why) {
        if (why) why->clear();
        return true;
    };
    s.now = [&t] { return t; };
    return s;
}

/// A pretend game for the party worker, on a clock of the test's own: what
/// it shows at each moment -- the player alone, B's invitation, which a click
/// answers, and B's party, which the game draws `draw_ms` after it is joined.
/// Nothing real is looked at or touched: the worker's "clicks" change this.
struct PretendPartyGame {
    std::uint64_t t = 50'000'000;
    Frame         alone;            ///< the game with the player in no party
    Frame         member;           ///< in B's party, B leading: B's panel down the left, the crown on it
    Frame         invitation;       ///< B's invitation's buttons, as the game draws them
    bool          invited = false;  ///< B's invitation is up
    std::uint64_t joined_ms = 0;    ///< when the player joined B's party; 0: not
    std::uint64_t draw_ms = 900;    ///< how long the game takes to draw it
    bool          takes = true;     ///< an accept joins B's party: not when B's is full, or B gone

    Frame shows() const {
        Frame f = joined_ms != 0 && t >= joined_ms + draw_ms ? member : alone;
        if (invited) blit(f, invitation, 586, 600);
        return f;
    }
    /// B's invitation answered with Xác Nhận, by GoBot or by hand.
    void accept() {
        invited = false;
        if (takes) joined_ms = t;
    }
    /// The game drawing its own picture, all of it seen; a grab takes 30 ms.
    PartySight sight() {
        PartySight s;
        s.grab = [this](Frame& out, bool& from_screen) {
            out = shows();
            from_screen = false;
            t += 30;
            return true;
        };
        s.seen = [](const ScreenRect&, std::string* why) {
            if (why) why->clear();
            return true;
        };
        s.now = [this] { return t; };
        return s;
    }
};

/// The party worker's pass on a pretend game, as party_loop takes it -- the
/// library's own steps, in the loop's own order: one look (take_party_look);
/// with auto-accept, an invitation on it answered, the answer noted as a
/// change of GoBot's; the chat lines read; then each asker by the party on a
/// look grabbed after the last change -- this pass's, or a new one when a
/// change came after it (read_party, party_invite_gate) -- an invite noted as
/// a change. A pass ends 150 ms on, as the loop's does ([Chat] vote_gap_ms).
/// The loop reads the inviter's name with the game's font before it answers;
/// no part of this.
struct PretendWorker {
    const PartyHudReader& reader;
    const TemplateStore&  store;
    PartyEvents           events;
    PartyInvite           answering;
    bool                  auto_accept = true;
    bool                  tick = true;          ///< "Chỉ mời khi đang làm chủ tổ đội"
    int                   accepted = 0, invited = 0, fresh_looks = 0;
    std::vector<PartyGate> refusals;

    PretendWorker(const PartyHudReader& r, const TemplateStore& s) : reader(r), store(s) {}

    void pass(PretendPartyGame& g, int askers) {
        PartyLook look;
        if (take_party_look(g.sight(), look)) {
            PartyInvite::Seen seen;
            if (auto_accept) seen = answering.look(look.frame, store, nullptr);
            if (auto_accept && seen.up && !bag::bin_asking(look.frame, store)) {
                g.t += 650;     // the focus, and the settle before the click
                g.accept();
                ++accepted;
                events.changed(g.t, "an invitation was accepted", party_text::kEventAccepted);
            }
            g.t += 20;          // the chat lines read with the game's font
            for (int i = 0; i < askers; ++i) {
                if (events.last_change().at_ms >= look.grabbed_ms) {
                    g.t += 5;   // the loop's own steps before it: a grab begins after the change
                    PartyLook fresh;
                    if (!take_party_look(g.sight(), fresh)) continue;
                    look = std::move(fresh);
                    ++fresh_looks;
                }
                const PartyGate gate = party_invite_gate(read_party(reader, look, events), tick);
                if (!gate.allowed) {
                    refusals.push_back(gate);
                    continue;
                }
                ++invited;
                g.t += 1500;    // the list opened, the row, Req Party
                events.changed(g.t, "an invite was sent", party_text::kEventInvited);
            }
        }
        g.t += 150;
    }
};

bool starts_with(const std::string& s, const std::string& head) { return s.rfind(head, 0) == 0; }

/// A picture by the name the template store gives it: one of the reader's,
/// else from the installed templates (TemplateStore::load_one). Not valid
/// when neither has it.
Frame installed(const char* name) {
    if (const Frame* f = kit().pictures.get(name)) return *f;
    Frame out;
    TemplateStore::load_one(name, exe_relative("configs/templates.ini"), out);
    return out;
}

void test_invite_mid_pass() {
    section("Party HUD: an invitation accepted in the same pass -- the look from before it is stale, never used; "
            "the asker is read on a new look (R7-3, a pretend game)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    const Frame buttons = installed("grabbed/party_invite_buttons");
    if (!buttons.valid()) {
        skip("no picture of a party invitation's buttons here\n");
        return;
    }
    // The worker's store: the reader's pictures and the invitation's, as the
    // party worker loads them all.
    TemplateStore store = k.pictures;
    store.put("grabbed/party_invite_buttons", buttons);
    const auto game = [&](std::uint64_t draw_ms) {
        PretendPartyGame g;
        g.alone = k.frame;
        g.member = k.frame;
        draw_member(g.member, 0, 112, Mark::Whole);
        g.invitation = buttons;
        g.invited = true;
        g.draw_ms = draw_ms;
        return g;
    };
    {
        // As round 7's final check found it: B's invitation up, the player
        // alone, auto-accept on, the tick on, and C asking in the same pass.
        // The pass's own look is from before the accept: C is read on a new
        // look, taken after it -- here with B's party drawn by then, so a
        // member under B's crown, and not invited.
        PretendPartyGame g = game(0);
        PretendWorker w(k.reader, store);
        const Frame before = g.shows();
        w.pass(g, 1);
        const std::string vi = w.refusals.empty() ? std::string() : w.refusals[0].reason_vi;
        check(w.accepted == 1 && w.invited == 0 && w.fresh_looks == 1 && w.refusals.size() == 1 &&
                  vi == party_text::kNotLeader,
              "B's invitation accepted, then C asks in the same pass: read on a new look, a member now, not invited "
              "-- \"" + vi + "\"");
        // The pass's own look, from before the accept, is no reading for it
        // at all, whatever it shows: GoBot's own race, ruled out.
        PartyLook pass_look;
        pass_look.frame = before;
        pass_look.trusted = true;
        pass_look.grabbed_ms = w.events.last_change().at_ms - 680;
        const PartyHud stale = read_party(k.reader, pass_look, w.events);
        const PartyGate on_pass_look = party_invite_gate(stale, false);
        check(stale.stale && !on_pass_look.allowed &&
                  on_pass_look.reason_vi == std::string(party_text::kStale) + " (" + party_text::kEventAccepted + ")" &&
                  !alone_for_chat(stale),
              "that pass's own look, grabbed before the accept: stale -- no invite on it even with the tick off, and "
              "no Tự chat line -- \"" + on_pass_look.reason_vi + "\"");
    }
    {
        // The tick off: invited as before the HUD was read -- on a look taken
        // after the accept, never on the pass's own from before it.
        PretendPartyGame g = game(0);
        PretendWorker w(k.reader, store);
        w.tick = false;
        w.pass(g, 1);
        check(w.accepted == 1 && w.invited == 1 && w.fresh_looks == 1 && w.refusals.empty(),
              "the tick off: C, asking in the pass that accepted, invited as before -- on a new look taken after "
              "the accept");
    }
    {
        // The game slow to draw B's party: what the new look shows is what
        // goes -- the player's rule has no wait for it.
        PretendPartyGame g = game(900);
        PretendWorker w(k.reader, store);
        w.pass(g, 1);
        w.pass(g, 1);
        check(w.accepted == 1 && w.fresh_looks == 1 && w.invited == 1,
              "B's party drawn 0.9 s after the accept: C, on the new look, sees no party yet and is invited -- no "
              "settle time, by the player's rule");
    }
}

void test_plain_alone_invites() {
    section("Party HUD: alone is what the game shows -- ticked, alone invites, with a window, a dialog or a tooltip "
            "anywhere on the game (the player's rule of 2026-10-21, a pretend game)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    PretendPartyGame g;
    g.alone = k.frame;
    g.member = k.frame;
    PretendWorker w(k.reader, k.pictures);
    w.pass(g, 1);
    check(w.invited == 1 && w.refusals.empty(), "nobody in a party, the tick on: C invited, to start a party");
    // D asks in the same pass's wake: on a look after C's invite -- the
    // pass's own is stale -- that shows no party: invited too. No settle
    // time, no second look.
    w.pass(g, 2);
    check(w.invited == 3 && w.refusals.empty() && w.fresh_looks >= 1,
          fmt("two more asking in the next pass: each on a look taken after the invite before it, each invited "
              "(%d invited, %zu refused, %d new looks)",
              w.invited, w.refusals.size(), w.fresh_looks) +
              (w.refusals.empty() ? std::string() : " -- " + w.refusals[0].reason_en));
    {
        // Read off the screen, the party's part of the game in view before
        // and after the grab: as the game's own picture.
        std::uint64_t t = 1'000'000;
        PartyLook look;
        const bool took = take_party_look(still_sight(k.frame, t, true), look);
        const PartyEvents none;
        const PartyHud h = read_party(k.reader, look, none);
        check(took && look.from_screen && look.trusted && h.state == PartyState::Alone &&
                  party_invite_gate(h, true).allowed && alone_for_chat(h),
              "frame.png read off the screen, the party's part in view at the grab: alone, invited -- " + said(h));
    }
    Frame small;
    if (load_live_frame(exe_relative("playerlist.png"), small) && small.width == 1024) {
        std::uint64_t t = 1'000'000;
        PartyLook look;
        take_party_look(still_sight(small, t, false), look);
        const PartyEvents none;
        const PartyHud h = read_party(k.reader, look, none);
        check(h.state == PartyState::Alone && party_invite_gate(h, true).allowed,
              "playerlist.png, 1024x768, as the game drew it: alone, invited -- " + said(h));
    } else {
        skip("no playerlist.png the checks were written against here");
    }
    // A window of the game, a dialog, a tooltip, anywhere on the game: the HUD
    // shows no party all the same, and alone invites. None is looked for.
    std::vector<std::pair<std::string, Frame>> over;
    if (k.close_x.valid()) {
        Frame f = k.frame;
        blit(f, k.close_x, 1100, 600);
        over.emplace_back("a window's X at 1100,600", f);
    } else {
        skip("no picture of the client's close button here\n");
    }
    {
        Frame f = k.frame;
        draw_tooltip(f, 800, 300);
        over.emplace_back("a tooltip at 800,300", f);
    }
    const Frame question = installed(bag::kRegisterDialog);
    if (question.valid()) {
        Frame f = k.frame;
        blit(f, question, 400, 250);
        over.emplace_back("a question in the middle of the game", f);
    }
    for (const auto& [what, f] : over) {
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Alone && party_invite_gate(h, true).allowed && alone_for_chat(h),
              what + ": alone, invited with the tick, and Tự chat's ticked line goes -- " + said(h));
    }
}

void test_look_at_grab() {
    section("Party HUD: trusted as it was when the frame was grabbed, not when the gate runs (R7-5, a pretend game)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    Frame live;
    if (!load_live_frame(exe_relative(kPartyProbe), live) || live.width != 1280) {
        skip("no saved frame of the live party here\n");
        return;
    }
    // The live party of two, another program's window over the game's left
    // side at the grab -- over the panels -- as the frame read off the screen
    // has it. The program goes 600 ms after the grab began: the party worker
    // answers an invitation or reads the HUD first, and only then asks.
    Frame f = live;
    other_program(f, ScreenRect{ 0, 60, 420, 600 });
    std::uint64_t t = 70'000'000, gone = 0;
    const ScreenRect& area = PartyHudReader::kArea;
    PartySight s;
    s.grab = [&](Frame& out, bool& from_screen) {
        out = f;
        from_screen = true;
        gone = t + 600;
        t += 30;
        return true;
    };
    s.seen = [&](const ScreenRect&, std::string* why) {
        const bool over = gone == 0 || t < gone;
        if (why) *why = over ? "Notepad.exe" : "";
        return !over;
    };
    s.now = [&t] { return t; };
    PartyLook look;
    const bool took = take_party_look(s, look);
    t += 700;
    // What asking at the gate says -- the window gone -- and the frame read
    // by that: the program's own pixels taken for the game.
    const bool trusted_now = s.seen(area, nullptr);
    const PartyHud late = k.reader.read(look.frame, trusted_now);
    // The look as it was grabbed: not trusted, so not read.
    const PartyEvents none;
    const PartyHud h = read_party(k.reader, look, none);
    const PartyGate g = party_invite_gate(h, true);
    check(took && !look.trusted && look.in_the_way == "Notepad.exe" && trusted_now && h.state == PartyState::Unknown &&
              !g.allowed && !alone_for_chat(h),
          "another program over the panels at the grab, gone by the gate: judged at the grab, not read, not invited "
          "-- \"" + g.reason_vi + "\" (judged at the gate it would read " + party_state_name(late.state) + ")");
    // A window that came or went during the grab: trusted neither way.
    for (const bool came : { true, false }) {
        int calls = 0;
        PartySight m;
        m.grab = [&](Frame& out, bool& from_screen) {
            out = k.frame;
            from_screen = true;
            return true;
        };
        m.seen = [&](const ScreenRect&, std::string* why) {
            const bool after = calls++ >= 1;
            const bool over = came ? after : !after;
            if (why) *why = over ? "Notepad.exe" : "";
            return !over;
        };
        m.now = [&t] { return t; };
        PartyLook l;
        take_party_look(m, l);
        const PartyHud r = read_party(k.reader, l, none);
        check(!l.trusted && r.state == PartyState::Unknown,
              std::string("another program's window ") + (came ? "come" : "gone") +
                  " during the grab, over the panels: not trusted, not read -- " + said(r));
    }
    // The game's own picture of itself, whatever is over its window: trusted.
    {
        PartySight own = s;
        own.grab = [&](Frame& out, bool& from_screen) {
            out = k.frame;
            from_screen = false;
            return true;
        };
        own.seen = [](const ScreenRect&, std::string* why) {
            if (why) *why = "Notepad.exe";
            return false;
        };
        PartyLook l;
        take_party_look(own, l);
        check(!l.from_screen && l.trusted && read_party(k.reader, l, none).state == PartyState::Alone,
              "a frame the game drew itself, another program over its window: trusted, alone");
    }
}

void test_read_party() {
    section("Party HUD: a look held up against GoBot's own changes to the party (PartyEvents), and the game's own "
            "word (PartyMembership, P0-3's seam)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    std::uint64_t t = 90'000'000;
    PartyLook look;
    take_party_look(still_sight(k.frame, t, false), look);
    const std::pair<const char*, const char*> changes[] = {
        { "an invitation was accepted", party_text::kEventAccepted },
        { "an invitation was declined", party_text::kEventDeclined },
        { "an invite was sent", party_text::kEventInvited },
    };
    for (const auto& [en, vi] : changes) {
        // A look grabbed before GoBot's own accept, decline or invite: stale,
        // and nothing goes on it, ticked or not -- Tự chat's line neither.
        PartyEvents e;
        const PartyHud before = read_party(k.reader, look, e);
        e.changed(look.grabbed_ms + 100, en, vi);
        const PartyHud h = read_party(k.reader, look, e);
        const PartyGate on = party_invite_gate(h, true), off = party_invite_gate(h, false);
        check(before.state == PartyState::Alone && !before.stale && alone_for_chat(before) && h.stale &&
                  h.state == PartyState::Alone && !on.allowed && !off.allowed && !alone_for_chat(h) &&
                  on.reason_vi == std::string(party_text::kStale) + " (" + vi + ")" && off.reason_vi == on.reason_vi &&
                  on.reason_en.find(en) != std::string::npos,
              std::string("a look grabbed before GoBot's own change (") + en +
                  "): stale, no invite on it ticked or not, no Tự chat line -- \"" + on.reason_vi + "\"");
        // A look grabbed after it is no stale one, and what it shows decides
        // at once: no settle time.
        PartyLook after = look;
        after.grabbed_ms = look.grabbed_ms + 101;
        const PartyHud a = read_party(k.reader, after, e);
        check(!a.stale && a.state == PartyState::Alone && party_invite_gate(a, true).allowed && alone_for_chat(a),
              std::string("and one grabbed a millisecond after it: alone, invited, Tự chat's line goes (") + en + ")");
    }
    {
        // The latest change is kept, one told late included.
        PartyEvents e;
        e.changed(look.grabbed_ms + 200, "an invite was sent", party_text::kEventInvited);
        e.changed(look.grabbed_ms + 10, "an invitation was accepted", party_text::kEventAccepted);
        check(e.last_change().vi == party_text::kEventInvited && e.last_change().at_ms == look.grabbed_ms + 200,
              "a change told late does not stand for a later one");
        const PartyEvents none;
        check(none.last_change().at_ms == 0 && !read_party(k.reader, look, none).stale,
              "nothing changed yet: no look is stale");
    }
    {
        // The game's own word, once P0-3 gives it: in a party, the screen's
        // alone is a party whose panels are not seen -- no invite with the
        // tick, no Tự chat line. Its word of no party, or none yet: the
        // screen decides.
        const PartyEvents none;
        PartyMembership game;
        game.known = true;
        game.in_party = true;
        const PartyHud h = read_party(k.reader, look, none, game);
        PartyMembership alone_word;
        alone_word.known = true;
        const PartyHud a = read_party(k.reader, look, none, alone_word);
        check(h.state == PartyState::InParty && !party_invite_gate(h, true).allowed && !alone_for_chat(h) &&
                  a.state == PartyState::Alone && read_party(k.reader, look, none).state == PartyState::Alone,
              "the game's word that the player is in a party: in a party, not invited, no Tự chat line -- " +
                  said(h) + "; its word of no party, or none yet: alone, by the screen");
    }
}

void test_chat_alone_rule() {
    section("Tự chat's \"Chỉ khi chưa có tổ đội\" (P2-5) by the same rule: the line goes when the HUD reads alone");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    // [A ticked, B] and [A ticked, B ticked]: what becomes of A's turn.
    const std::vector<bool> one = { true, false }, both = { true, true }, typeable = { true, true };
    const auto turn = [&](const PartyHud& h, const std::vector<bool>& ticks) {
        return alone_turn(ticks, typeable, 0, alone_for_chat(h));
    };
    const PartyEvents none;
    std::uint64_t t = 30'000'000;
    PartyLook plain;
    take_party_look(still_sight(k.frame, t, false), plain);
    const PartyHud alone = read_party(k.reader, plain, none);
    check(alone_for_chat(alone) && turn(alone, one) == AloneTurn::Send && turn(alone, both) == AloneTurn::Send,
          "the HUD reads alone: A goes in its turn -- one look, no second");
    // Whatever else is on the game: a tooltip, a window, a dialog -- alone all
    // the same, and the line goes.
    std::vector<std::pair<std::string, Frame>> cases;
    {
        Frame f = k.frame;
        draw_tooltip(f, 800, 300);
        cases.emplace_back("a tooltip on the game", f);
    }
    if (k.close_x.valid()) {
        Frame f = k.frame;
        blit(f, k.close_x, 900, 200);
        cases.emplace_back("a window of the game open", f);
    } else {
        skip("no picture of the client's close button here\n");
    }
    for (const auto& [what, f] : cases) {
        PartyLook look;
        take_party_look(still_sight(f, t, false), look);
        const PartyHud h = read_party(k.reader, look, none);
        check(h.state == PartyState::Alone && alone_for_chat(h) && turn(h, one) == AloneTurn::Send,
              what + ": alone, A goes -- " + said(h));
    }
    // A party seen, the HUD not read, or a frame from before GoBot's last
    // change to the party: A waits -- passed over for B, or with every line
    // ticked put off a minute.
    Frame member = k.frame;
    draw_member(member, 0, 112, Mark::Whole);
    Frame leading = k.frame;
    mark_own(leading, Mark::Whole);
    draw_member(leading, 0, 112);
    Frame no_crown = k.frame;
    draw_member(no_crown, 0, 112);
    PartyEvents changed;
    changed.changed(plain.grabbed_ms + 50, "an invite was sent", party_text::kEventInvited);
    const std::vector<std::pair<std::string, PartyHud>> waits = {
        { "a member", k.reader.read(member) },
        { "leading a party", k.reader.read(leading) },
        { "in a party, no crown seen", k.reader.read(no_crown) },
        { "the HUD not read", k.reader.read(k.frame, false) },
        { "alone on a frame from before GoBot's invite", read_party(k.reader, plain, changed) },
    };
    for (const auto& [what, h] : waits)
        check(!alone_for_chat(h) && turn(h, one) == AloneTurn::Skip && turn(h, both) == AloneTurn::Wait,
              what + ": A passed over for B, or with every line ticked put off a minute -- " + said(h));
}

void test_panel_picture() {
    section("Party HUD: the member panel's frame, as it is matched");
    if (skip_without_kit()) return;
    const Frame& p = kit().reader.panel_picture();
    int solid = 0, bars = 0, under_frame = 0;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x) {
            const bool on = p.pixels[(static_cast<std::size_t>(y) * p.width + x) * 4 + 3] >= TemplateMatcher::kOpaque;
            if (!on) continue;
            ++solid;
            if (y >= 26 && y < 37 && x >= 6 && x < 89) ++bars;
            if (y < 3 || x < 3) ++under_frame;
        }
    check(p.width == 130 && p.height == 42 && solid > 600 && bars == 0 && under_frame == 0,
          "130x42, " + std::to_string(solid) + " pixels of frame; the bars and the leader's band left out");
}

/// Bug 3 of 2026-11-14 (the owner: "Khi thực hiện auto pt, nếu chẳng may pt
/// trúng người đã vào pt thì sẽ bị hiện dialog mà không tự động tắt => chỉ cần
/// ấn enter hoặc click xác nhận trong dialog"): the game's one-button message
/// after an invite closed by its own Xác Nhận -- clicked, never Enter, which
/// opens the chat line -- and that player held off for InviteReply::kHoldMs
/// (30 s since 2026.11.15b, ten minutes before).
struct ReplyGame {
    Frame base, box;
    int bx = 541, by = 320;
    int looks = 0;
    int from_look = 1;          ///< drawn from this look on; 0 never
    int only_looks = -1;        ///< drawn for this many looks, then gone by itself (-1: till clicked)
    std::vector<int> shift;     ///< its x moved by this much on look n (from 1), when given
    bool sticks = false;        ///< a click on its Xác Nhận does nothing
    /// A click on its Xác Nhận closes it this many ms of naps later -- a slow
    /// game's (2026-11-15); 0: at once.
    int  close_after_ms = 0;
    int  closes_at = -1;        ///< the naps' time it goes at, once clicked
    int  stop_at = -1;          ///< the naps' time from which a stop is asked; -1 never
    bool lit_after = false;     ///< after a click its label drawn lit, the box still there
    bool closed = false, clicked_once = false;
    Frame footer;               ///< an invitation's footer drawn too, when valid
    Frame words;                ///< its words, drawn on it where the game writes them, when valid
    int fx = 140, fy = 560;
    int t = 0;                  ///< the naps, in ms
    /// A clock handed over as Hands::now_ms (with `clocked`): the naps and
    /// `look_ms` for every look -- a slow guest's grab and search.
    bool clocked = false;
    int look_ms = 0;
    std::uint64_t clock = 0;
    bool stop = false;
    std::vector<std::pair<int, int>> clicks, moves;
    std::vector<int> click_t;
    ScreenRect ok;              ///< its Xác Nhận's label, as lone_ok finds it on a look with only it
    int dx_now() const {
        return looks >= 1 && looks <= static_cast<int>(shift.size()) ? shift[static_cast<std::size_t>(looks - 1)] : 0;
    }
    bool stopped() const { return stop || (stop_at >= 0 && t >= stop_at); }
    Frame look() {
        ++looks;
        clock += static_cast<std::uint64_t>(look_ms);
        if (closes_at >= 0 && t >= closes_at) closed = true;
        Frame f = base;
        if (footer.valid()) blit(f, footer, fx, fy);
        const bool on = from_look > 0 && looks >= from_look && !closed &&
                        (only_looks < 0 || looks < from_look + only_looks);
        if (on) {
            blit(f, box, bx + dx_now(), by);
            if (words.valid()) blit(f, words, bx + dx_now() + 24, by + 14);
            if (lit_after && clicked_once)
                for (int y = ok.y - 2; y < ok.bottom() + 2; ++y)
                    for (int x = ok.x - 2; x < ok.right() + 2; ++x) {
                        std::uint8_t* d = &f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4];
                        for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(std::min(255, d[c] + 70));
                    }
        }
        return f;
    }
    InviteReply::Hands hands() {
        InviteReply::Hands h;
        h.grab = [this](Frame& f) {
            f = look();
            return true;
        };
        h.click = [this](int x, int y) {
            clicks.emplace_back(x, y);
            click_t.push_back(t);
            const ScreenRect button{ ok.x - 4, ok.y - 4, ok.w + 8, ok.h + 8 };
            if (button.contains(x, y)) {
                clicked_once = true;
                if (!sticks && !lit_after) {
                    if (close_after_ms <= 0) closed = true;
                    else if (closes_at < 0) closes_at = t + close_after_ms;
                }
            }
            return true;
        };
        h.move = [this](int x, int y) { moves.emplace_back(x, y); };
        h.nap = [this](int ms) {
            t += ms;
            clock += static_cast<std::uint64_t>(ms);
            return !stopped();
        };
        h.stop = [this] { return stopped(); };
        if (clocked) h.now_ms = [this] { return clock; };
        return h;
    }
};

void test_invite_reply() {
    section("Tổ đội (2026.11.14b): the game's message after an invite -- \"already in a party\", refused -- closed "
            "by its own Xác Nhận, never a key, and that player not invited again for 30 s (10 min until 2026.11.15b)");
    static TemplateStore store;
    static const bool loaded = store.load_best(exe_relative("configs/templates.ini"));
    const Frame* fail = store.get("grabbed/invite_fail2");
    const Frame* ok_box = store.get("grabbed/dialog_ok");
    if (!loaded || fail == nullptr || ok_box == nullptr) {
        skip("the game's message pictures (grabbed/invite_fail2, grabbed/dialog_ok) are not installed here\n");
    } else {
        const auto game_with = [&](const Frame& box) {
            ReplyGame g;
            g.base = plain_frame(1280, 720);
            g.box = box;
            Frame probe = g.base;
            blit(probe, box, g.bx, g.by);
            bag::lone_ok(probe, store, ScreenRect{ 0, 0, probe.width, probe.height }, &g.ok);
            return g;
        };
        // 1. lone_ok and the party's own message: its label where it is.
        {
            const ReplyGame g = game_with(*fail);
            check(g.ok.valid() && std::abs(g.ok.x - 689) <= 1 && std::abs(g.ok.y - 378) <= 1,
                  fmt("grabbed/invite_fail2 at 541,320: its Xác Nhận found by lone_ok at %d,%d (689,378 wanted)", g.ok.x,
                      g.ok.y));
        }
        // 2. Never an invitation's, the bin's or the revive question's.
        {
            bool none = true;
            std::string which;
            for (const char* name : { "grabbed/party_invite_buttons", "grabbed/del_item_confirm_1",
                                      "grabbed/hoisinh_confirm" }) {
                const Frame* pic = store.get(name);
                if (pic == nullptr) continue;
                Frame f = plain_frame(1280, 720);
                blit(f, *pic, 140, 300);
                if (bag::lone_ok(f, store, ScreenRect{ 0, 0, f.width, f.height })) {
                    none = false;
                    which += std::string(" ") + name;
                }
            }
            check(none, "lone_ok takes no two-button dialog for the message -- an invitation, the bin's question, the "
                        "revive question" + which);
            const Frame* footer = store.get("grabbed/party_invite_buttons");
            if (footer != nullptr) {
                ReplyGame g = game_with(*fail);
                g.footer = *footer;
                const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs);
                const ScreenRect button{ g.ok.x - 4, g.ok.y - 4, g.ok.w + 8, g.ok.h + 8 };
                bool safe = true;
                for (const auto& [x, y] : g.clicks)
                    safe = safe && button.contains(x, y) &&
                           !(std::abs(x - (g.fx + 31)) <= 6 && std::abs(y - (g.fy + 23)) <= 6);
                check(safe && r.how == InviteReply::How::None && g.clicks.empty(),
                      std::string("an invitation and the message both up: nothing clicked -- lone_ok wants no dialog "
                                  "of two buttons anywhere -- never the invitation's Xác Nhận -- ") +
                          InviteReply::how_name(r.how));
            }
        }
        // 3. Closed: one click in the middle of its label, the pointer moved
        //    50 px under it, nothing else.
        {
            ReplyGame g = game_with(*fail);
            g.from_look = 2;
            const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs);
            check(r.how == InviteReply::How::Closed && r.clicks == 1 && g.clicks.size() == 1 &&
                      g.clicks[0].first == g.ok.cx() && g.clicks[0].second == g.ok.cy() && g.moves.size() == 1 &&
                      g.moves[0].first == g.ok.cx() && g.moves[0].second == g.ok.bottom() + InviteReply::kParkPx &&
                      r.seen_after_ms == InviteReply::kLookMs,
                  fmt("the message up: closed with one click at %d,%d, the pointer moved under it -- %s, %d click(s)",
                      g.clicks.empty() ? -1 : g.clicks[0].first, g.clicks.empty() ? -1 : g.clicks[0].second,
                      InviteReply::how_name(r.how), r.clicks));
        }
        // 4. One that will not go: two clicks, the second a pixel aside and
        //    later, then given up -- no third.
        {
            ReplyGame g = game_with(*fail);
            g.sticks = true;
            const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs);
            check(r.how == InviteReply::How::NotClosed && g.clicks.size() == 2 &&
                      g.clicks[1].first == g.clicks[0].first + 1 && g.clicks[1].second == g.clicks[0].second &&
                      g.click_t[1] - g.click_t[0] >= 300,
                  fmt("a message that will not close: two clicks, %d ms apart, then given up -- %s",
                      g.click_t.size() == 2 ? g.click_t[1] - g.click_t[0] : -1, InviteReply::how_name(r.how)));
        }
        // 5. None: nothing clicked or moved, the watch its length.
        {
            ReplyGame g = game_with(*fail);
            g.from_look = 0;
            const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs);
            check(r.how == InviteReply::How::None && g.clicks.empty() && g.moves.empty() &&
                      g.t >= InviteReply::kWatchMs && g.t < InviteReply::kWatchMs + InviteReply::kLookMs,
                  fmt("no message: nothing clicked or moved, watched %d ms", g.t));
        }
        // 6. A flash, and a box that moves: not clicked until seen twice at
        //    one place.
        {
            ReplyGame g = game_with(*fail);
            g.only_looks = 1;
            const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs);
            ReplyGame m = game_with(*fail);
            m.shift = { 0, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 };
            m.ok.x += 6;
            const InviteReply::Result rm = InviteReply::watch(store, nullptr, m.hands(), InviteReply::kWatchMs);
            check(r.how == InviteReply::How::None && g.clicks.empty() && rm.how == InviteReply::How::Closed &&
                      m.clicks.size() == 1 && m.looks >= 3 && m.clicks[0].first == m.ok.cx(),
                  "a message for one look only: not clicked; one moved 6 px after its first look: clicked only where "
                  "two looks found it");
        }
        // 7. Lit after the click, its box still there: not taken for closed.
        {
            ReplyGame g = game_with(*ok_box);
            g.lit_after = true;
            const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs);
            check(r.how != InviteReply::How::Closed && !g.clicks.empty(),
                  std::string("its button lit after the click, the box still up: not called closed -- ") +
                      InviteReply::how_name(r.how));
        }
        // 8. A stop first: nothing clicked.
        {
            ReplyGame g = game_with(*fail);
            g.stop = true;
            const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs);
            check(r.how == InviteReply::How::Stopped && g.clicks.empty(), "a stop: nothing clicked");
        }
        // 8b. On a slow guest, by the clock too (the review of 2026.11.14b): a
        //     look of 600 ms ends a watch with nothing in it well before its
        //     naps add up to 2.5 s; and a short watch (two looks) with a look
        //     of 700 ms still takes a message seen, its one look more, and
        //     sees it gone after the click.
        {
            ReplyGame g = game_with(*fail);
            g.from_look = 0;
            g.clocked = true;
            g.look_ms = 600;
            const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs);
            ReplyGame s = game_with(*fail);
            s.clocked = true;
            s.look_ms = 700;
            const InviteReply::Result rs =
                InviteReply::watch(store, nullptr, s.hands(), 2 * InviteReply::kLookMs + 50);
            check(r.how == InviteReply::How::None && g.clicks.empty() && g.looks <= 4 &&
                      g.t < InviteReply::kWatchMs && rs.how == InviteReply::How::Closed && rs.clicks == 1,
                  fmt("slow looks: an empty watch over by the clock after %d look(s) (%d ms of naps); a short one "
                      "still closes the message seen -- %s, %d click(s)",
                      g.looks, g.t, InviteReply::how_name(rs.how), rs.clicks));
        }
        // 8c. A slow game (2026-11-15, the owner: "sẽ có thêm 30 giây chờ phản
        //     ứng"): the first click on its Xác Nhận watched up to
        //     kFirstGoneMs (kReactMs) before the second. Gone 12 s after it:
        //     one click, and how long said. Never gone: the second click
        //     kFirstGoneMs after the first, watched kGoneMs, given up -- not
        //     much later. A stop 7 s into the wait: over at once.
        {
            ReplyGame g = game_with(*fail);
            g.close_after_ms = 12'000;
            const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs);
            check(InviteReply::kFirstGoneMs == kReactMs && InviteReply::kGoneMs == 2500 &&
                      r.how == InviteReply::How::Closed && r.clicks == 1 && g.clicks.size() == 1 &&
                      r.gone_after_ms >= 12'000 && r.gone_after_ms <= 12'000 + InviteReply::kLookMs,
                  fmt("the message gone 12 s after the click: one click, closed, seen gone %d ms after it -- %s",
                      r.gone_after_ms, InviteReply::how_name(r.how)));
            ReplyGame s = game_with(*fail);
            s.sticks = true;
            const InviteReply::Result rs = InviteReply::watch(store, nullptr, s.hands(), InviteReply::kWatchMs);
            const int apart = s.click_t.size() == 2 ? s.click_t[1] - s.click_t[0] : -1;
            const int after_first = s.click_t.empty() ? -1 : s.t - s.click_t[0];
            check(rs.how == InviteReply::How::NotClosed && s.clicks.size() == 2 && apart >= InviteReply::kFirstGoneMs &&
                      apart <= InviteReply::kFirstGoneMs + 2 * InviteReply::kLookMs &&
                      after_first <= InviteReply::kFirstGoneMs + InviteReply::kGoneMs + 3 * InviteReply::kLookMs &&
                      rs.gone_after_ms == -1,
                  fmt("a message that never goes on a slow game: the second click %d ms after the first, given up "
                      "%d ms after the first -- %s",
                      apart, after_first, InviteReply::how_name(rs.how)));
            ReplyGame p = game_with(*fail);
            p.sticks = true;
            p.stop_at = InviteReply::kLookMs + 7'000;   // the first click goes at the second look, kLookMs in
            const InviteReply::Result rp = InviteReply::watch(store, nullptr, p.hands(), InviteReply::kWatchMs);
            check(rp.how == InviteReply::How::Stopped && p.clicks.size() == 1 && p.t >= p.stop_at &&
                      p.t <= p.stop_at + InviteReply::kLookMs,
                  fmt("a stop 7 s into the wait for the message to go: stopped at %d ms of naps, one click", p.t));
        }
        // 9. Its words read, for the log.
        {
            GlyphFont font;
            Frame words;
            if (!font.load(GlyphFont::default_dir()) || !font.render_line("Doi phuong da co nhom", words)) {
                skip("no glyphs here to draw the message's words with\n");
            } else {
                ReplyGame g = game_with(*ok_box);
                g.words = words;
                const InviteReply::Result r = InviteReply::watch(store, &font, g.hands(), InviteReply::kWatchMs);
                check(r.how == InviteReply::How::Closed && r.words.find("Doi phuong da co nhom") != std::string::npos,
                      "the message's words read with the game's glyphs: \"" + r.words + "\"");
            }
        }
    }
    // 10. That player held off (PartyChat::hold_off): not returned again for
    //     the hold, a name read alike held too, and said by cooling() -- with
    //     a hold of ten minutes, as 2026.11.14b had it.
    {
        const std::uint64_t ten_min = 600'000;
        PartyChat chat;
        PartyChat::Settings s;
        s.votes = 1;
        s.cooldown_s = 30;
        chat.set_settings(s);
        std::uint64_t now = 100000;
        const auto first = chat.saw({ "Someone: pt" }, now);
        chat.hold_off("Someone", now, ten_min);
        const bool held_31 = chat.saw({ "Someone: pt" }, now + 31'000).empty();
        const bool held_599 = chat.saw({ "Someone: pt" }, now + 599'000).empty();
        const bool alike = chat.saw({ "Somone: pt" }, now + 400'000).empty();
        const auto waiting = chat.cooling(now + 100'000);
        const bool again = chat.saw({ "Someone: pt" }, now + 601'000).size() == 1;
        const bool other = chat.saw({ "Other: pt" }, now + 602'000).size() == 1;
        PartyChat reset = chat;
        reset.hold_off("Someone", now + 601'000, ten_min);
        reset.reset();
        check(first.size() == 1 && held_31 && held_599 && alike && again && other &&
                  waiting.count("Someone") == 1 && waiting.at("Someone") > 400 && reset.cooling(now + 602'000).empty(),
              "hold_off: held at +31 s and +599 s, a name read alike held too, another player not; asked again at "
              "+601 s; cooling() says so, reset() forgets it");
    }
    // 10b. The hold the party worker gives since 2026.11.17b (the owner chose
    //      A: a minute, ten after two in a row -- 2026.11.15b's 30 s before):
    //      InviteTries' first hold handed to PartyChat -- held at +59 s past a
    //      5-s cooldown, asked again at +61 s.
    {
        PartyChat chat;
        PartyChat::Settings s;
        s.votes = 1;
        s.cooldown_s = 5;
        chat.set_settings(s);
        const std::uint64_t now = 100000;
        const auto first = chat.saw({ "Someone: pt" }, now);
        InviteTries tries;
        const InviteTries::Outcome o = tries.failed("Someone", now, "the game answered it with a message");
        chat.hold_off("Someone", now, o.hold_ms);
        const bool held_59 = chat.saw({ "Someone: pt" }, now + 59'000).empty();
        const bool again = chat.saw({ "Someone: pt" }, now + 61'000).size() == 1;
        check(o.hold_ms == 60'000 && first.size() == 1 && held_59 && again,
              "InviteTries' first hold handed to PartyChat: the player whose invite the game answered is held 60 s "
              "(30 s until 2026.11.17b): held at +59 s past a 5-s cooldown, asked again at +61 s");
    }
#ifdef GOBOT_SOURCE_DIR
    // 11. Where the party worker looks for it, and what it never presses.
    {
        const std::string gui = read_all(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp");
        const std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/party_invite.cpp");
        const std::size_t npos = std::string::npos;
        const auto body = [](const std::string& text, const std::string& head) {
            const std::size_t a = text.find(head);
            const std::size_t b = a == std::string::npos ? std::string::npos : text.find("\n}", a);
            return a == std::string::npos || b == std::string::npos ? std::string() : text.substr(a, b + 2 - a);
        };
        const std::string party = body(gui, "void GuiApp::party_loop() {");
        const std::string watch = body(cpp, "InviteReply::Result InviteReply::watch(");
        // With the stop that also ends on the character down (2026-11-15).
        const std::size_t invite = party.find("list.invite(input_, capture, store, *row, &why, mouse_stopped)");
        const std::size_t answered = invite == npos ? npos : party.find("InviteReply::watch(", invite);
        const std::size_t invited = party.find("say_party(\"invited \" + who)");
        check(!party.empty() && !watch.empty() && invite != npos && answered != npos && invited != npos &&
                  invite < answered && answered < invited && party.find("chat.hold_off(") != npos &&
                  party.find("VK_RETURN") == npos && party.find("VK_ESCAPE") == npos &&
                  party.find("key_press(") == npos && watch.find("key_press") == npos && watch.find("VK_") == npos &&
                  watch.find("send_key") == npos && party.find("click_at(") == npos,
              "gui_app.cpp: after an invite the game's message is looked for and closed by its Xác Nhận, before "
              "\"invited\" is said, the player held off; no Enter, ESC or other key anywhere in it");
        // The other two places it is looked for, and the pause (the review of
        // 2026.11.14b: the source check above reached neither): the late
        // watch after the HUD's tick, that player held off; a message up
        // before an invite closed on the list's own look, before its rows
        // are read; the chat's askers not counted while one is stuck. Three
        // watches in all, and the clock handed to them.
        const auto in_order = [&](const std::vector<std::string>& parts) {
            std::size_t at = 0;
            for (const std::string& p : parts) {
                at = party.find(p, at);
                if (at == npos) return false;
                at += p.size();
            }
            return true;
        };
        int watches = 0;
        for (std::size_t at = party.find("InviteReply::watch("); at != npos;
             at = party.find("InviteReply::watch(", at + 1))
            ++watches;
        check(in_order({ "party_hud_tick(look, store);", "InviteReply::kLateMs",
                         "const bool late_due = rules.auto_invite && !last_invited.empty() && reply_clicks == 0;",
                         "InviteReply::watch(",
                         "invite_failed(last_invited" }) &&
                  in_order({ "Frame open_now;", "if (bag::lone_ok(open_now, store,", "InviteReply::watch(",
                             "const auto rows = list.read(open_now, font);" }) &&
                  in_order({ "const bool reply_stuck = now_ms() < reply_stuck_until;",
                             "if (reply_stuck || reply_clicks > 0) lines.clear();",
                             "for (const std::string& who : chat.saw(lines, now))" }) &&
                  watches == 3 && gui.find("reply_hands.now_ms = steady_ms;") != npos &&
                  watch.find("clock_ms() >= watch_ms && !have_first") != npos,
              "gui_app.cpp: the late watch after the HUD's tick, that invite counted as come to nothing; a message "
              "up before an invite closed before the list is read; no asker counted while one is stuck or waits to "
              "be clicked again; three watches, on the clock as well as the naps");

        // 12. 2026.11.17b (the owner chose A, B and C): every watch of the
        //     worker's one click with kCloseHoldMs -- never kReactMs with the
        //     mouse held; a held-off player turned away before the list is
        //     opened; each invite judged on a later pass's HUD and that player
        //     held off in the chat as well; the list shut by close() -- its
        //     button -- after every turn, and never a key.
        int short_watches = 0;
        for (std::size_t at = party.find("InviteReply::kCloseHoldMs, 1)"); at != npos;
             at = party.find("InviteReply::kCloseHoldMs, 1)", at + 1))
            ++short_watches;
        const std::size_t open_at = party.find("list.ensure_open(input_, capture, store, &why, mouse_stopped)");
        int shuts = 0;
        for (std::size_t at = party.find("shut_list();", open_at == npos ? 0 : open_at); at != npos;
             at = party.find("shut_list();", at + 1))
            ++shuts;
        check(short_watches == 3 && open_at != npos &&
                  in_order({ "for (const std::string& who : chat.saw(lines, now))",
                             "if (tries.held(who, now_ms(), &left)) {", "continue;",
                             "list.ensure_open(input_, capture, store, &why, mouse_stopped)" }) &&
                  in_order({ "if (tries.due(now_ms())) {", "const PartyHud hud = party_read(look, store);",
                             "tries.judge(seen, now_ms())", "chat.hold_off(o.who, steady_ms(), o.hold_ms);" }) &&
                  in_order({ "const InviteTries::Outcome o = tries.failed(who, now_ms(), why_en);",
                             "chat.hold_off(who, steady_ms(), o.hold_ms);" }) &&
                  in_order({ "list.invite(input_, capture, store, *row, &why, mouse_stopped)",
                             "tries.sent(who, now_ms(), size_before);", "InviteReply::watch(",
                             "invite_failed(who, \"the game answered it with a message\"", "shut_list();",
                             "say_party(\"invited \" + who)" }) &&
                  in_order({ "if (list.opened_it()) list_left_open = true;",
                             "list.close(input_, capture, store, &why_open, mouse_stopped)" }) &&
                  shuts >= 6 && party.find("InviteReply::kHoldMs") == npos && party.find("VK_ESCAPE") == npos,
              fmt("gui_app.cpp (2026.11.17b): %d of the worker's watches one click held %d ms at most; a held-off "
                  "player turned away before ensure_open; each invite judged on the HUD a pass later and held off in "
                  "the chat too; the list shut by its button at %d places after it is opened, never ESC",
                  short_watches, InviteReply::kCloseHoldMs, shuts));
        // 13. The review of 2026.11.17b: a message left up, or the invites
        //     stopped, by one asker's turn ends the pass's askers before the
        //     next is held, the list opened or the message clicked again; an
        //     asker not on the list held in the chat a short while, never
        //     counted as a miss; a message up before an invite answers nobody,
        //     no English stand-in on the tab.
        const std::size_t not_listed = party.find("asked, but is not among the ");
        const std::size_t not_listed_end = party.find("shut_list();", not_listed == npos ? 0 : not_listed);
        const std::string not_listed_part =
            not_listed == npos || not_listed_end == npos ? std::string()
                                                         : party.substr(not_listed, not_listed_end - not_listed);
        check(in_order({ "for (const std::string& who : chat.saw(lines, now))",
                         "if (reply_clicks > 0 || now_ms() < reply_stuck_until) break;",
                         "if (tries.held(who, now_ms(), &left)) {",
                         "list.ensure_open(input_, capture, store, &why, mouse_stopped)" }) &&
                  !not_listed_part.empty() &&
                  not_listed_part.find("chat.hold_off(who, steady_ms(), InviteTries::kNotListedHoldMs);") != npos &&
                  not_listed_part.find("invite_failed(") == npos && InviteTries::kNotListedHoldMs == 60'000 &&
                  party.find("reply_after(\"\", before);") != npos &&
                  party.find("reply_after(\"an earlier invitee\"") == npos &&
                  party.find("\"Thông báo có sẵn\"") != npos,
              "gui_app.cpp (review of 2026.11.17b): a message waiting or the invites stopped ends the pass's askers "
              "before the next turn; not on the list -- a 60 s chat hold, never a counted miss; a message up before "
              "an invite names no player on the tab");
    }
#endif
}

/// 2026.11.17b, A (the owner: "khi pt người đã có pt thì hiển thị thông báo
/// sau đó bot không phản ứng gì nữa và cứ mở khung pt"; chose "ok làm A B C
/// đi"): a player whose invite did not lead to a join is held off -- a minute,
/// ten after two in a row -- whatever the screen showed; a join starts the
/// count again. On a pretend clock, nothing on a screen.
void test_invite_tries() {
    section("Tổ đội (2026.11.17b, A): an invite that did not lead to a join holds that player off -- 1 min, 10 min "
            "after two in a row -- judged on the party HUD 15 s on; a join starts the count again");
    using T = InviteTries;
    check(T::kJoinWaitMs == 15'000 && T::kFailHoldMs == 60'000 && T::kFailHoldLongMs == 600'000 &&
              T::kFailsForLong == 2 && T::kStaleWaitMs == 10'000,
          "the owner's numbers: judged 15 s after the invite, held 1 min, 10 min from the 2nd in a row");
    const std::uint64_t t0 = 1'000'000;
    // 1. No join: not judged before 15 s, then held a minute.
    {
        T t;
        t.sent("Someone", t0, 1);
        const bool early = !t.due(t0 + 14'999) && t.waiting("Someone");
        const bool due = t.due(t0 + 15'000);
        const auto out = t.judge(T::Hud{ 1, false }, t0 + 15'000);
        std::uint64_t left = 0;
        const bool held = t.held("Someone", t0 + 15'000 + 59'000, &left);
        const bool free = !t.held("Someone", t0 + 15'000 + 60'001);
        check(early && due && out.size() == 1 && !out[0].joined && out[0].fails == 1 && out[0].hold_ms == 60'000 &&
                  held && left == 1'000 && free && !t.waiting("Someone") && !t.due(t0 + 100'000),
              "alone before and after: not judged before 15 s, then no join -- held 60 s, free after: " +
                  (out.empty() ? std::string("nothing") : out[0].why));
        // 2. The second in a row: ten minutes.
        t.sent("Someone", t0 + 80'000, 1);
        const auto second = t.judge(T::Hud{ 1, false }, t0 + 95'000);
        check(second.size() == 1 && second[0].fails == 2 && second[0].hold_ms == 600'000 &&
                  t.held("Someone", t0 + 95'000 + 599'000) && !t.held("Someone", t0 + 95'000 + 600'001),
              fmt("the second in a row: held %d s", second.empty() ? -1 : static_cast<int>(second[0].hold_ms / 1000)));
        // 3. A join starts it again: no hold, the next miss a minute.
        t.sent("Someone", t0 + 700'000, 2);
        const auto joined = t.judge(T::Hud{ 3, false }, t0 + 715'000);
        t.sent("Someone", t0 + 800'000, 3);
        const auto after = t.failed("Someone", t0 + 801'000, "the game answered it with a message");
        check(joined.size() == 1 && joined[0].joined && joined[0].hold_ms == 0 && after.fails == 1 &&
                  after.hold_ms == 60'000 && !t.waiting("Someone"),
              "a member's panel more than before: joined, the count started again -- the next miss a minute");
    }
    // 4. The game's message counts at once; the invite is not judged again.
    {
        T t;
        t.sent("Other", t0, 1);
        const auto o = t.failed("Other", t0 + 2'000, "the game answered it with a message");
        check(o.fails == 1 && o.hold_ms == 60'000 && !t.waiting("Other") && !t.due(t0 + 20'000) &&
                  t.judge(T::Hud{ 1, false }, t0 + 20'000).empty() && t.held("Other", t0 + 61'000) &&
                  !t.held("Other", t0 + 62'001),
              "a message after the invite: held at once, from then -- no second count 15 s on");
    }
    // 5. A HUD that cannot tell: unread now or before -- no join.
    {
        T a, b;
        a.sent("Someone", t0, 1);
        b.sent("Someone", t0, 0);
        const auto ua = a.judge(T::Hud{ 0, false }, t0 + 15'000);
        const auto ub = b.judge(T::Hud{ 2, false }, t0 + 15'000);
        check(ua.size() == 1 && !ua[0].joined && ua[0].why.find("could not be read") != std::string::npos &&
                  ub.size() == 1 && !ub[0].joined && ub[0].why.find("not read before") != std::string::npos,
              "the HUD unread after the invite, or before it: counted as no join -- " +
                  (ua.empty() ? std::string() : ua[0].why) + "; " + (ub.empty() ? std::string() : ub[0].why));
    }
    // 6. A look from before GoBot's last change: waited past, up to 10 s more.
    {
        T t;
        t.sent("Someone", t0, 1);
        const auto stale = t.judge(T::Hud{ 2, true }, t0 + 15'000);
        const bool still = t.waiting("Someone");
        const auto fresh = t.judge(T::Hud{ 2, false }, t0 + 16'000);
        T u;
        u.sent("Someone", t0, 1);
        const auto late = u.judge(T::Hud{ 1, true }, t0 + 25'000);
        check(stale.empty() && still && fresh.size() == 1 && fresh[0].joined && late.size() == 1 && !late[0].joined,
              "a stale look: not judged on, the next one is; still stale 25 s on, taken as it is");
    }
    // 7. Two out at once share one more member: the first invited gets it.
    {
        T t;
        t.sent("First", t0, 1);
        t.sent("Second", t0 + 3'000, 1);
        const auto out = t.judge(T::Hud{ 2, false }, t0 + 20'000);
        const bool first_joined = out.size() == 2 && out[0].who == "First" && out[0].joined;
        const bool second_not = out.size() == 2 && out[1].who == "Second" && !out[1].joined && out[1].fails == 1;
        check(first_joined && second_not, "two invites, one member more: one join, the other held off");
    }
    // 8. A name read a letter differently is the same player.
    {
        T t;
        t.failed("Someone", t0, "x");
        const auto o = t.failed("Somone", t0 + 70'000, "x");
        check(t.held("Somone", t0 + 1'000) && o.fails == 2 && o.hold_ms == 600'000 && !t.held("Other", t0 + 1'000),
              "\"Somone\" for \"Someone\": the same count and hold; another player not held");
    }
    // 9. (Review of 2026.11.17b.) An older invitee who joined before a later
    //    invite's HUD was read is in that invite's size_before already: not
    //    taken off it again. A sent at 1, A joins, B sent at 2, A judged, B
    //    joins: both joined. B not joining: a miss all the same.
    {
        T t, u;
        for (T* x : { &t, &u }) {
            x->sent("Alpha", t0, 1);
            x->sent("Bravo", t0 + 5'000, 2);
        }
        const auto a = t.judge(T::Hud{ 2, false }, t0 + 15'000);
        const auto b = t.judge(T::Hud{ 3, false }, t0 + 20'000);
        const auto ua = u.judge(T::Hud{ 2, false }, t0 + 15'000);
        const auto ub = u.judge(T::Hud{ 2, false }, t0 + 20'000);
        check(a.size() == 1 && a[0].who == "Alpha" && a[0].joined && b.size() == 1 && b[0].who == "Bravo" &&
                  b[0].joined && !t.held("Bravo", t0 + 21'000) && t.fails("Bravo") == 0 && ua.size() == 1 &&
                  ua[0].joined && ub.size() == 1 && !ub[0].joined && ub[0].fails == 1,
              "A sent at 1 and joined, B sent at 2, A judged, B joined: both joins -- " +
                  (b.empty() ? std::string("nothing") : b[0].why) + "; B not joining (HUD 2): a miss");
    }
}

/// 2026.11.17b, B: the party worker's watch clicks once and holds the game's
/// mouse kCloseHoldMs at most after the click -- not kReactMs -- and a message
/// still up is left for a later pass.
void test_reply_short_hold() {
    section("Tổ đội (2026.11.17b, B): the worker's watch -- one click on the message's Xác Nhận, the mouse held "
            "2.5 s at most after it, never a key");
    static TemplateStore store;
    static const bool loaded = store.load_best(exe_relative("configs/templates.ini"));
    const Frame* fail = store.get("grabbed/invite_fail2");
    if (!loaded || fail == nullptr) {
        skip("the game's message picture (grabbed/invite_fail2) is not installed here\n");
        return;
    }
    const auto game = [&] {
        ReplyGame g;
        g.base = plain_frame(1280, 720);
        g.box = *fail;
        Frame probe = g.base;
        blit(probe, *fail, g.bx, g.by);
        bag::lone_ok(probe, store, ScreenRect{ 0, 0, probe.width, probe.height }, &g.ok);
        return g;
    };
    check(InviteReply::kCloseHoldMs == 2500 && InviteReply::kCloseTries == 3 && InviteReply::kRetryGapMs == 1000 &&
              InviteReply::kCloseHoldMs < kReactMs,
          "kCloseHoldMs 2.5 s (not kReactMs), three clicks in all, a second apart at least");
    // A message that will not go: one click, given up kCloseHoldMs after it.
    {
        ReplyGame g = game();
        g.sticks = true;
        const InviteReply::Result r =
            InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs, InviteReply::kCloseHoldMs, 1);
        const int after = g.click_t.empty() ? -1 : g.t - g.click_t[0];
        check(r.how == InviteReply::How::NotClosed && g.clicks.size() == 1 && r.clicks == 1 && after >= 0 &&
                  after <= InviteReply::kCloseHoldMs + 2 * InviteReply::kLookMs,
              fmt("a message that stays: one click, given up %d ms after it (%d at most) -- %s", after,
                  InviteReply::kCloseHoldMs + 2 * InviteReply::kLookMs, InviteReply::how_name(r.how)));
    }
    // On a slow guest, by the clock: looks of 700 ms do not stretch it.
    {
        ReplyGame g = game();
        g.sticks = true;
        g.clocked = true;
        g.look_ms = 700;
        const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), 2 * InviteReply::kLookMs + 50,
                                                         InviteReply::kCloseHoldMs, 1);
        // Two looks and a nap to take it, then the hold and one look past it.
        const int most = 2 * (700 + InviteReply::kLookMs) + InviteReply::kCloseHoldMs + 700 + InviteReply::kLookMs;
        check(r.how == InviteReply::How::NotClosed && g.clicks.size() == 1 &&
                  g.clock <= static_cast<std::uint64_t>(most),
              fmt("700-ms looks: one click, over %llu ms of the clock after it began",
                  static_cast<unsigned long long>(g.clock)));
    }
    // Gone a second after the click: closed with that one click.
    {
        ReplyGame g = game();
        g.close_after_ms = 1000;
        const InviteReply::Result r =
            InviteReply::watch(store, nullptr, g.hands(), InviteReply::kWatchMs, InviteReply::kCloseHoldMs, 1);
        check(r.how == InviteReply::How::Closed && g.clicks.size() == 1 && r.gone_after_ms >= 1000 &&
                  r.gone_after_ms <= 1000 + InviteReply::kLookMs,
              fmt("gone 1 s after the click: closed, seen gone %d ms after it", r.gone_after_ms));
    }
    // Three watches in a row on one that stays -- what three passes do --
    // click it three times, no more, each a watch of its own.
    {
        ReplyGame g = game();
        g.sticks = true;
        int clicks = 0;
        for (int pass = 0; pass < InviteReply::kCloseTries; ++pass) {
            const InviteReply::Result r = InviteReply::watch(store, nullptr, g.hands(), 2 * InviteReply::kLookMs + 50,
                                                             InviteReply::kCloseHoldMs, 1);
            clicks += r.clicks;
        }
        check(clicks == InviteReply::kCloseTries && static_cast<int>(g.clicks.size()) == clicks,
              fmt("three passes on a message that stays: %d clicks, one each", clicks));
    }
}

/// 2026.11.17b, C: the player list shut after an asker's turn by the button
/// that opens it, found by its picture at 1024x768, 1280x720 and 1366x768;
/// never pressed when it is shut already or shuts by itself; one press only.
void test_list_close() {
    section("Tổ đội (2026.11.17b, C): the player list shut by its own button after an invite, at 1024x768, "
            "1280x720 and 1366x768 -- one press, none when it is shut already, never ESC");
    Frame icon = plain_frame(20, 20), grid = plain_frame(60, 30);
    std::uint32_t seed = 1117;
    for (Frame* p : { &icon, &grid })
        for (std::size_t i = 0; i < p->pixels.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                seed = seed * 1664525u + 1013904223u;
                p->pixels[i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(seed >> 24);
            }
            p->pixels[i + 3] = 255;
        }
    TemplateStore pics;
    pics.put(PlayerList::Settings{}.open_picture, icon);
    pics.put(PlayerList::Settings{}.grid_picture, grid);

    struct Game {
        int w = 1280, h = 720, bx = 1212, by = 139;
        bool up = true;
        int shuts_after = 0;            ///< ms after the press it goes; -1 never
        int shuts_alone_at = -1;        ///< ms from the start it goes by itself; -1 never
        std::uint64_t t = 10'000, start = 10'000, pressed_at = 0;
        std::uint64_t stop_at = ~0ull;
        std::vector<std::pair<int, int>> presses;
    };
    const auto run = [&](Game& g, std::string& why) {
        PlayerList list;
        list.set_click([&](int x, int y) {
            g.presses.emplace_back(x, y);
            if (g.presses.size() == 1) g.pressed_at = g.t;
            return true;
        });
        list.set_clock([&g] { return g.t; }, [&g](int ms) { g.t += static_cast<std::uint64_t>(ms); });
        list.set_grab([&](Frame& f) {
            f = plain_frame(g.w, g.h);
            blit(f, icon, g.bx, g.by);
            bool showing = g.up;
            if (g.shuts_alone_at >= 0 && g.t >= g.start + static_cast<std::uint64_t>(g.shuts_alone_at))
                showing = false;
            // The button flips it: shut so long after the first press.
            if (!g.presses.empty() && g.shuts_after >= 0 &&
                g.t >= g.pressed_at + static_cast<std::uint64_t>(g.shuts_after))
                showing = false;
            if (showing) blit(f, grid, g.bx - 200, g.by + 60);
            return true;
        });
        InputManager none;
        ScreenCapture capture;
        return list.close(none, capture, pics, &why, [&g] { return g.t >= g.stop_at; });
    };
    check(PlayerList::kCloseWaitMs == 3000 && PlayerList::kCloseSettleMs == 300,
          "close() waits 3 s at most after its press, and looks twice 300 ms apart first");
    // At each size: the button where that client has it (1366x768 by the
    // right edge, as the other two -- found by its picture wherever it is).
    const struct { int w, h, bx, by; } sizes[] = { { 1024, 768, 956, 139 }, { 1280, 720, 1212, 139 },
                                                   { 1366, 768, 1298, 139 } };
    for (const auto& s : sizes) {
        Game g;
        g.w = s.w;
        g.h = s.h;
        g.bx = s.bx;
        g.by = s.by;
        g.shuts_after = 400;
        std::string why;
        const bool ok = run(g, why);
        check(ok && g.presses.size() == 1 && g.presses[0].first == s.bx + 10 && g.presses[0].second == s.by + 10 &&
                  g.t - g.pressed_at <= 400 + 250,
              fmt("%dx%d: one press on its button's middle at %d,%d, shut %llu ms after it", s.w, s.h,
                  g.presses.empty() ? -1 : g.presses[0].first, g.presses.empty() ? -1 : g.presses[0].second,
                  static_cast<unsigned long long>(g.t - g.pressed_at)) +
                  (why.empty() ? "" : " -- " + why));
    }
    // Shut already: nothing pressed.
    {
        Game g;
        g.up = false;
        std::string why;
        check(run(g, why) && g.presses.empty(), "a list shut already: true, nothing pressed");
    }
    // Shut by itself a moment later -- a good invite does that: nothing
    // pressed, or the press would bring it back up.
    {
        Game g;
        g.shuts_alone_at = 100;
        std::string why;
        check(run(g, why) && g.presses.empty(), "a list that shuts by itself between the two looks: nothing pressed");
    }
    // Never shuts: one press -- never a second -- given up after 3 s.
    {
        Game g;
        g.shuts_after = -1;
        std::string why;
        const bool ok = run(g, why);
        check(!ok && g.presses.size() == 1 && g.t - g.pressed_at >= 3000 && g.t - g.pressed_at <= 3000 + 250 &&
                  why.find("still showed") != std::string::npos,
              fmt("a list that stays: one press, given up %llu ms after it -- ",
                  static_cast<unsigned long long>(g.t - g.pressed_at)) + why);
    }
    // A stop first: nothing pressed.
    {
        Game g;
        g.stop_at = 0;
        std::string why;
        check(!run(g, why) && g.presses.empty() && why == "stopped", "a stop: nothing pressed");
    }
#ifdef GOBOT_SOURCE_DIR
    {
        const std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/player_list.cpp");
        const std::size_t a = cpp.find("bool PlayerList::close(");
        const std::size_t b = a == std::string::npos ? a : cpp.find("\n}", a);
        const std::string body = a == std::string::npos || b == std::string::npos ? std::string() : cpp.substr(a, b - a);
        check(!body.empty() && body.find("VK_") == std::string::npos && body.find("key_press") == std::string::npos &&
                  body.find("click(input, bx, by, stop)") != std::string::npos,
              "player_list.cpp: close() presses the list's own button through click(), and no key");
    }
#endif
}

} // namespace

void test_party_hud(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_invite_reply();
    test_invite_tries();
    test_reply_short_hold();
    test_list_close();
    test_panel_picture();
    test_crown_pictures();
    test_reading();
    test_crowns();
    test_nonsense();
    test_leader_unread();
    test_bag_over();
    test_party_under_window();
    test_own_crown_under_slot();
    test_trust();
    test_gate();
    test_worker_step();
    test_read_party();
    test_look_at_grab();
    test_invite_mid_pass();
    test_plain_alone_invites();
    test_chat_alone_rule();
    test_real_frames();
}
