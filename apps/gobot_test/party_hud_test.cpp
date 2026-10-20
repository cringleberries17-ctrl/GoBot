// gobot_test -- the party HUD read off the game's screen, and whether an
// invite can work by it -- and, since round 7b, whether alone is shown
// rather than merely not contradicted: the look judged as it was grabbed, a
// look from before a change to the party never used, no dialog or tooltip
// anywhere, all of it on pretend games where a game is needed.
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
#include "gobot/party_invite.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

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

const char* const kLiveBuild = "C:\\Users\\player\\source\\GoBot\\build\\";

/// The saved frames of the live party, under kLiveBuild. Another member
/// leads: that member's panel at 0,106, the crown on its corner. The first is
/// the quick quest's look, nothing over the panels; the second the trash
/// cleaner's, the bag dragged over two more panels below it.
const char* const kPartyProbe = "bin\\Release\\quest_debug\\probe_20261016_234019_00.png";
const char* const kPartyUnderBag = "bin\\Release\\trash_debug\\20261016_223524_0_not_in_bin.png";
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
        for (const std::string& p : { exe_relative("frame.png"), std::string(kLiveBuild) + "bin\\Release\\frame.png" })
            if (!k.frame.valid() && load_png(p, k.frame) && (k.frame.width != 1280 || k.frame.height != 720))
                k.frame = Frame{};
        if (!k.frame.valid()) {
            k.why = "no frame.png from the live client here";
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
    std::printf("  [skip] %s\n", kit().why.c_str());
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
        std::printf("  [skip] no grabbed/party_leader_key2 here\n");
    }
    {
        // At 1024x768, where the HUD is the same distance from the top-left.
        Frame shot;
        if (load_png(std::string(kLiveBuild) + "bin\\Release\\playerlist.png", shot) && shot.width == 1024) {
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
            std::printf("  [skip] no playerlist.png from the live client here\n");
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
    if (!load_png(std::string(kLiveBuild) + kPartyProbe, live) || live.width != 1280) {
        std::printf("  [skip] no saved frame of the live party here\n");
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
            std::printf("  [skip] no %s.png staged beside this test\n", c.ours);
            continue;
        }
        if (!TemplateStore::load_one(c.gocs, ini, gocs)) {
            std::printf("  [skip] no %s.png here to compare %s with\n", c.gocs, c.ours);
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
        std::printf("  [skip] the Level plate or the bag's strip is not among the loaded pictures\n");
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
        !load_png(std::string(kLiveBuild) + kPartyUnderBag, dragged) || kept.width != 1280 ||
        dragged.width != 1280) {
        std::printf("  [skip] no r3_B_000.png or the saved party frame here\n");
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

/// Where the cuts of the live frames are (round 7: the HUD's tooltip, the ESC
/// menu): beside this test, else the copy handed over with them.
std::string live_cuts_dir() {
    for (const std::string& d : { exe_relative("testdata\\round7"),
                                  std::string(kLiveBuild) + "handoff\\round7\\coder\\testdata" })
        if (!files_in(d, "*.png").empty()) return d;
    return {};
}

/// The cut `what` of a live frame (<what>_<x>_<y>.png), as a picture.
Frame live_cut(const std::string& what) {
    Frame f;
    const std::string dir = live_cuts_dir();
    if (dir.empty()) return f;
    for (const std::string& name : files_in(dir, what + "_*.png"))
        if (load_png(dir + "\\" + name, f)) break;
    return f;
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
    section("Party HUD: a party under a window reads as alone, but never surely, so it never invites with the "
            "tick (R5-1)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    Frame live;
    if (!load_png(std::string(kLiveBuild) + kPartyProbe, live) || live.width != 1280) {
        std::printf("  [skip] no saved frame of the live party here\n");
        return;
    }
    {
        // The live party of two, another member leading, with something
        // plain over the leader's panel from the screen's left edge, crown
        // and all. Nothing of the party shows, and no tab strip says a bag
        // hides it: it reads as a player alone does -- but not surely.
        Frame f = live;
        fill(f, 0, 100, 150, 55, 200, 200, 210);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Alone && h.panels.empty() && !h.own_leader && !h.sure_alone &&
                  h.alone_doubt_vi == party_text::kDoubtPlain,
              "the live party, something plain over the leader's panel and its crown: alone, not surely -- " +
                  said(h));
        // So with the tick nobody is invited, and the tab and the log say
        // why; without it, invited as before.
        const PartyGate ticked = party_invite_gate(h, true), plain = party_invite_gate(h, false);
        check(!ticked.allowed && ticked.reason_vi.rfind(party_text::kAloneUnsure, 0) == 0 &&
                  ticked.reason_en.find("not surely") != std::string::npos && plain.allowed,
              "and with the tick, not invited: \"" + ticked.reason_vi + "\" (" + ticked.reason_en +
                  "); without it, invited");
    }
    // The game's own tooltip (the live client's, cut from a saved frame) and
    // one of its windows (the live ESC menu, its X with it) laid over the
    // leader's panel from the left edge, as R5-1 found them: alone, never
    // surely, never invited with the tick.
    const Frame tip = live_cut("tooltip"), menu = live_cut("esc_menu");
    if (tip.valid() && menu.valid()) {
        // The tooltip's own box starts 4 pixels into its cut.
        for (const int dx : { 0, 5, 10 }) {
            Frame f = live;
            blit(f, tip, dx - 4, 66);
            const PartyHud h = k.reader.read(f);
            const PartyGate g = party_invite_gate(h, true);
            check(!g.allowed && !h.sure_alone &&
                      (h.state != PartyState::Alone || h.alone_doubt_vi == party_text::kDoubtTooltip),
                  fmt("the live tooltip from x=%d over the leader's panel: ", dx) + said(h) + " -- not invited");
        }
        Frame f = live;
        blit(f, menu, 0, 92);
        const PartyHud h = k.reader.read(f);
        check(!party_invite_gate(h, true).allowed && !h.sure_alone &&
                  (h.state != PartyState::Alone || h.alone_doubt_vi == party_text::kDoubtWindow),
              "the live ESC menu over the leader's panel: " + said(h) + " -- not invited");
    } else {
        std::printf("  [skip] no cuts of the live tooltip and ESC menu here (testdata\\round7)\n");
    }
    if (const Frame* x = k.pictures.get(bag::kCloseButtons[0])) {
        // A window of the game as wide as the quest window over the leader's
        // panel, its X at its top right -- far right of the part the party is
        // read from (kArea) -- and another program's window over the right of
        // the game, the X under it, as the round 7 review found it: the frame
        // read off the screen has no X on it, and the panels' part of the
        // game is all in view, so the frame is trusted.
        Frame f = live;
        const ScreenRect at = game_window(f, ScreenRect{ 0, 90, 700, 420 }, *x);
        const PartyHud seen = k.reader.read(f);
        check(at.x >= PartyHudReader::kArea.right() && seen.state == PartyState::Alone && !seen.sure_alone &&
                  seen.alone_doubt_vi == party_text::kDoubtWindow && !party_invite_gate(seen, true).allowed,
              fmt("the live party under a window of the game, its X at %d,%d: alone, not surely, not invited -- ",
                  at.x, at.y) + said(seen));
        other_program(f, ScreenRect{ 400, 0, f.width - 400, f.height });
        // By its pixels alone it now looks like a player alone with nothing
        // open: only the capture can tell that part of it is not the game.
        const PartyHud by_pixels = k.reader.read(f, true, true);
        check(by_pixels.state == PartyState::Alone && by_pixels.sure_alone,
              "and another program over x>=400, the X under it: by its pixels alone, surely alone -- " +
                  said(by_pixels));
        const PartyHud h = k.reader.read(f, true, false);
        const PartyGate ticked = party_invite_gate(h, true), plain = party_invite_gate(h, false);
        check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtHidden &&
                  !ticked.allowed &&
                  ticked.reason_vi == std::string(party_text::kAloneUnsure) + " (" + party_text::kDoubtHidden + ")" &&
                  plain.allowed,
              "so, read as the capture has it, part of the game not seen: alone, not surely, and with the tick "
              "not invited: \"" + ticked.reason_vi + "\" (" + ticked.reason_en + "); without it, invited");
    } else {
        std::printf("  [skip] no picture of the client's close button here\n");
    }
    // A window wearing the close box over the panels, the pointer resting on
    // its X, or the button held down on it: the X drawn in that state, which
    // the drawing at rest misses, is a window open all the same.
    for (const char* name : { "inven/1/btn_CloseBox.spr/1", "inven/1/btn_CloseBox.spr/2" }) {
        const Frame* x = k.pictures.get(name);
        if (x == nullptr) {
            check(false, std::string(name) + " is among the pictures the reader loads");
            continue;
        }
        Frame f = live;
        const ScreenRect at = game_window(f, ScreenRect{ 0, 90, 300, 310 }, *x);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtWindow &&
                  !party_invite_gate(h, true).allowed,
              std::string("the live party under a window wearing ") + name +
                  fmt(" at %d,%d: alone, not surely, not invited -- ", at.x, at.y) + said(h));
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
    load_png(std::string(kLiveBuild) + "bin\\Release\\playerlist.png", small);
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
        std::printf("  [skip] no seal icons here, staged or in GoC's folder\n");
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
        std::printf("  [skip] could not put up a window of this test's own here\n");
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
    // All of it, for telling alone for sure: the same answers, and the frame
    // the game drew itself is whole whatever lies over the window.
    const Frame f = plain_frame(400, 500);
    std::string hides;
    const bool whole = party_frame_whole(true, w, f, &hides);
    check(!whole && hides == "off the screen" && party_frame_whole(false, w, f) &&
              !party_frame_whole(true, nullptr, f) && party_frame_whole(false, nullptr, f),
          "the whole of a frame of it off the screen: not the game's own (" + hides +
              "); one it drew itself: the game's own");
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
            std::printf("  [skip] no live\\shots\\r2_B_000.png here\n");
        }
        check(party_frame_trusted(false, nullptr) && !party_frame_trusted(true, nullptr),
              "a frame the game drew itself is trusted; one off the screen with no game window to look at "
              "is not");
    }
    // Saved frames that do show a party: read as what they show, and left out
    // of the count below. The first came on 2026-10-16, after the frames below
    // had been gathered all alone: the trash cleaner kept it in a party that
    // another player led. A frame saved since that reads as a party is
    // to be looked at by eye -- named here if it shows one, a wrong reading to
    // fix if it does not.
    struct PartyFrame {
        const char* path;       ///< under kLiveBuild
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
        { "bin\\Release\\quest_debug\\probe_20261016_234032_01.png", PartyState::Member, 2, false },
        { "bin\\Release\\quest_debug\\probe_20261016_234045_02.png", PartyState::Member, 2, false },
    };
    const auto shows_a_party = [&](const std::string& path) {
        for (const PartyFrame& p : party_frames)
            if (path == std::string(kLiveBuild) + p.path) return true;
        return false;
    };
    for (const PartyFrame& p : party_frames) {
        Frame f;
        if (!load_png(std::string(kLiveBuild) + p.path, f)) {
            std::printf("  [skip] no %s here\n", p.path);
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
    std::vector<std::string> frames;
    for (const char* dir : { "live\\shots\\", "frozen\\live\\shots\\", "bin\\Release\\trash_debug\\" }) {
        std::vector<std::string> names = files_in(std::string(kLiveBuild) + dir, "*.png");
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) frames.push_back(std::string(kLiveBuild) + dir + n);
    }
    frames.push_back(std::string(kLiveBuild) + "bin\\Release\\frame.png");
    frames.push_back(std::string(kLiveBuild) + "bin\\Release\\playerlist.png");
    int looked = 0, alone = 0, sure = 0, by_window = 0, by_dialog = 0, by_tooltip = 0, by_plain = 0, unknown = 0,
        party = 0, crowned = 0, sure_with_sign = 0;
    std::string first_party, first_sign;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!every && i % 8 != 0 && i + 2 < frames.size()) continue;
        if (shows_a_party(frames[i])) continue;     // read above, as what it shows
        Frame f;
        if (!load_png(frames[i], f) || f.width < 640 || f.height < 480) continue;
        ++looked;
        const PartyHud h = k.reader.read(f);
        if (h.own_crown >= PartyHudReader::kCrownScore) ++crowned;
        if (h.state == PartyState::Alone) {
            ++alone;
            sure += h.sure_alone ? 1 : 0;
            by_window += h.alone_doubt_vi == party_text::kDoubtWindow ? 1 : 0;
            by_dialog += h.alone_doubt_vi == party_text::kDoubtDialog ? 1 : 0;
            by_tooltip += h.alone_doubt_vi == party_text::kDoubtTooltip ? 1 : 0;
            by_plain += h.alone_doubt_vi == party_text::kDoubtPlain ? 1 : 0;
            // Sure only with no dialog and no tooltip anywhere on it, asked
            // again on its own.
            if (h.sure_alone &&
                (k.reader.dialog_up(f) || PartyHudReader::tooltip_edge(f, ScreenRect{ 0, 0, f.width, f.height }))) {
                ++sure_with_sign;
                if (first_sign.empty()) first_sign = frames[i];
            }
        } else if (h.state == PartyState::Unknown) {
            ++unknown;
        } else {
            ++party;
            if (first_party.empty()) first_party = frames[i] + ": " + said(h);
        }
    }
    if (looked == 0) {
        std::printf("  [skip] no saved frames of the live client here\n");
        return;
    }
    // PH-A4 as the saved frames have it now: those that show a party are read
    // as one above; every other is never read as one -- not a member, not a
    // leader, never full -- and nowhere on them is a crown.
    check(party == 0 && crowned == 0,
          std::to_string(looked) + " saved frames that show no party: " + std::to_string(alone) + " alone, " +
              std::to_string(unknown) + " unknown, " + std::to_string(party) + " read as a party, " +
              std::to_string(crowned) + " with the own crown" +
              (first_party.empty() ? std::string() : " -- first: " + first_party));
    // Of those alone, how many surely: the rest each with what may be over
    // the panels. Every one is one or the other.
    check(sure + by_window + by_dialog + by_tooltip + by_plain == alone,
          fmt("of the %d alone: %d surely; not surely, %d with a window open, %d with a dialog up, %d with a "
              "tooltip, %d with something plain over the first member's place",
              alone, sure, by_window, by_dialog, by_tooltip, by_plain));
    check(sure_with_sign == 0, fmt("and of the %d surely alone, %d with a dialog or a tooltip on it", sure,
                                   sure_with_sign) +
                                   (first_sign.empty() ? std::string() : " -- first: " + first_sign));
}

void test_gate() {
    section("Party HUD: an invite only when it can work (PH-A5), with the tick by the crown, or surely alone");
    PartyHud alone, sure_alone, window_alone, hidden_alone, member, leader, in_party, unknown, full_lead, full_member,
        full_in_party, stale_alone, stale_lead;
    alone.state = PartyState::Alone;
    alone.size = 1;
    sure_alone = alone;
    sure_alone.sure_alone = true;
    window_alone = alone;
    window_alone.alone_doubt = "a window of the game is open";
    window_alone.alone_doubt_vi = party_text::kDoubtWindow;
    hidden_alone = alone;
    hidden_alone.alone_doubt = "part of the game's picture is under another window";
    hidden_alone.alone_doubt_vi = party_text::kDoubtHidden;
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
    stale_alone = sure_alone;
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
    // Alone, ticked, is invited only when surely alone -- an invite then
    // starts a party, as GoC's does (the player's decision of 2026-10-19).
    // Alone and not surely is what a party under a window looks like too
    // (the round 5 review's R5-1): not invited.
    const std::string window_vi = std::string(party_text::kAloneUnsure) + " (" + party_text::kDoubtWindow + ")";
    const std::string hidden_vi = std::string(party_text::kAloneUnsure) + " (" + party_text::kDoubtHidden + ")";
    const std::string stale_accepted = std::string(party_text::kStale) + " (" + party_text::kEventAccepted + ")";
    const std::string stale_invited = std::string(party_text::kStale) + " (" + party_text::kEventInvited + ")";
    const Row rows[] = {
        { "alone surely, on a frame from before an accept", &stale_alone, false, stale_accepted.c_str(), false,
          stale_accepted.c_str() },
        { "leading, on a frame from before an invite", &stale_lead, false, stale_invited.c_str(), false,
          stale_invited.c_str() },
        { "alone, surely", &sure_alone, true, "", true, "" },
        { "alone, not surely", &alone, true, "", false, party_text::kAloneUnsure },
        { "alone, a window open", &window_alone, true, "", false, window_vi.c_str() },
        { "alone, part of the game not seen", &hidden_alone, true, "", false, hidden_vi.c_str() },
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
    const PartyGate alone_ticked = party_invite_gate(window_alone, true);
    check(alone_ticked.reason_en == "read as alone, but not surely: a window of the game is open",
          "alone with a window open, ticked: the log says \"" + alone_ticked.reason_en + "\"");
    check(std::string(party_text::kNotLeader) == "Đang trong tổ đội nhưng không phải chủ tổ đội" &&
              std::string(party_text::kNoCrown) ==
                  "Đang trong tổ đội nhưng không thấy vương miện chủ tổ đội ở ô nào" &&
              std::string(party_text::kUnreadNoCrown) ==
                  "Không đọc được khung tổ đội nên không thấy vương miện của bạn" &&
              std::string(party_text::kAloneUnsure) == "Có vẻ chưa có tổ đội, nhưng chưa chắc",
          "the tab's words for each");
    check(party_hud_line_vi(unknown) == "Tổ đội: không đọc được" && party_hud_line_vi(sure_alone) == "Tổ đội: một mình",
          "the state card: \"" + party_hud_line_vi(unknown) + "\", \"" + party_hud_line_vi(sure_alone) + "\"");
    // Alone and not surely so says so on the card itself, not only in "Thử
    // xem" and a refused invite (the round 7b tester's problem 5).
    check(party_hud_line_vi(alone) == "Tổ đội: một mình (chưa chắc)" &&
              party_hud_line_vi(window_alone) == "Tổ đội: một mình (chưa chắc)",
          "alone, not surely: \"" + party_hud_line_vi(window_alone) + "\"");
}

void test_worker_step() {
    section("Party HUD: the worker clicks nothing when the gate says no (PH-A6)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    // A pretend party worker's step for one asker, as party_loop takes it:
    // the gate on the frame it already has, and only then the player list
    // and the invite -- which here only count.
    int opened = 0, invited = 0;
    const auto step = [&](const Frame& f, bool always_leader, bool trusted = true, bool whole = true) {
        const PartyGate g = party_invite_gate(k.reader.read(f, trusted, whole), always_leader);
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
    // Alone with a window of the game open -- a close button anywhere: not
    // surely alone, so not with the tick.
    Frame window_open = k.frame;
    if (const Frame* x = k.pictures.get(bag::kCloseButtons[0])) blit(window_open, *x, 900, 200);
    const std::string alone_why = step(window_open, true);
    check(opened == 0 && invited == 0 && alone_why.find("not surely") != std::string::npos,
          "alone, a window open somewhere, the tick on: still nothing -- " + alone_why);
    // Alone with nothing open on the frame, but part of the game under
    // another program's window: not surely alone either.
    const std::string hidden_why = step(k.frame, true, true, false);
    check(opened == 0 && invited == 0 && hidden_why.find("not surely") != std::string::npos,
          "alone, part of the game not seen, the tick on: still nothing -- " + hidden_why);
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
    // Surely alone -- frame.png as it is, nothing open over the game -- with
    // the tick: invited, to start a party, as GoC does.
    const std::string sure_why = step(k.frame, true);
    check(opened == 5 && invited == 5 && sure_why.empty(),
          "surely alone, the tick on: the list opened and the invite sent, to start a party");
}

void test_sure_alone() {
    section("Party HUD: alone, and surely so -- no window, no tooltip, nothing plain over the panels "
            "(the player's decision of 2026-10-19)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        const PartyHud h = k.reader.read(k.frame);
        check(h.state == PartyState::Alone && h.sure_alone && party_invite_gate(h, true).allowed,
              "frame.png, nothing over the game: surely alone, invited with the tick -- " + said(h));
    }
    Frame small;
    if (load_png(std::string(kLiveBuild) + "bin\\Release\\playerlist.png", small) && small.width == 1024) {
        // A clear sky where the first member's panel would be: plain enough
        // to look twice at, not plain enough to be a window.
        const PartyHud h = k.reader.read(small);
        check(h.state == PartyState::Alone && h.sure_alone, "playerlist.png, 1024x768, a clear sky under the "
                                                            "HUD: surely alone -- " + said(h));
    }
    if (k.pictures.get(bag::kCloseButtons[0]) == nullptr) {
        std::printf("  [skip] no pictures of the game's close buttons here\n");
        return;
    }
    {
        // Part of the game not seen -- read off the screen with another
        // program's window over some of it, or past the screen's edge: what
        // is there could be the X of a window over the panels.
        const PartyHud h = k.reader.read(k.frame, true, false);
        const PartyGate g = party_invite_gate(h, true);
        check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtHidden &&
                  !g.allowed && g.reason_vi == std::string(party_text::kAloneUnsure) + " (" +
                                                   party_text::kDoubtHidden + ")" &&
                  party_invite_gate(h, false).allowed,
              "frame.png, part of the game not seen: alone, not surely -- \"" + g.reason_vi + "\" (" + g.reason_en +
                  "); the tick off: invited, as before");
    }
    {
        // A close button anywhere -- here the bag's own, from the client's
        // pictures, far from the panels: a window is open, and could be
        // over them.
        Frame f = k.frame;
        blit(f, *k.pictures.get(bag::kCloseButtons[0]), 1100, 600);
        const PartyHud h = k.reader.read(f);
        const PartyGate g = party_invite_gate(h, true);
        check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtWindow &&
                  !g.allowed && g.reason_vi == std::string(party_text::kAloneUnsure) + " (" +
                                                   party_text::kDoubtWindow + ")",
              "a close button at 1100,600: alone, not surely -- \"" + g.reason_vi + "\"");
    }
    {
        // The client's close buttons as it draws them under the pointer and
        // held down: a window over the panels with the pointer resting on
        // its X. The box's X then scores under kWindowXScore against its
        // drawing at rest, so each state is looked for by its own picture.
        for (const char* name : kCloseButtonStates) {
            const Frame* x = k.pictures.get(name);
            if (x == nullptr) {
                check(false, std::string(name) + " is among the pictures the reader loads");
                continue;
            }
            std::string rest_name = name;
            rest_name.back() = '0';
            const Frame* rest = k.pictures.get(rest_name);
            Frame f = k.frame;
            blit(f, *x, 900, 200);
            const float vs_rest =
                rest != nullptr
                    ? TemplateMatcher::find(f, ScreenRect{ 890, 190, x->width + 20, x->height + 20 }, *rest, -1.0f).score
                    : -1.0f;
            const PartyHud h = k.reader.read(f);
            check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtWindow &&
                      !party_invite_gate(h, true).allowed,
                  std::string(name) + fmt(" at 900,200 (its drawing at rest scores %.3f on it): alone, not surely -- ",
                                          static_cast<double>(vs_rest)) +
                      said(h));
        }
    }
    {
        // Something of one colour over the first member's place.
        Frame f = k.frame;
        const ScreenRect& slot = PartyHudReader::kFirstSlot;
        fill(f, slot.x, slot.y, slot.w, slot.h, 90, 90, 100);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtPlain,
              "one plain colour over the first member's place: alone, not surely -- " + said(h));
    }
    const Frame tip = live_cut("tooltip");
    if (tip.valid()) {
        // The game's tooltip over the panels' column, where the live client
        // drew it -- and the same nearer the left edge.
        for (const int x : { 16, 0 }) {
            Frame f = k.frame;
            blit(f, tip, x, 66);
            const PartyHud h = k.reader.read(f);
            ScreenRect at;
            const bool edge = PartyHudReader::tooltip_edge(f, ScreenRect{ 0, 0, f.width, f.height }, &at);
            check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtTooltip &&
                      edge,
                  fmt("the live tooltip at %d,66: alone, not surely -- its edge at %d,%d -- ", x, at.x, at.y) +
                      said(h));
        }
        // And nowhere near the panels: still a tooltip on the game's
        // picture, which could be over a window's X (round 7's R7-4).
        Frame f = k.frame;
        blit(f, tip, 700, 300);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtTooltip &&
                  !party_invite_gate(h, true).allowed,
              "the same tooltip at 700,300, nowhere near the panels: alone, not surely, not invited with the "
              "tick -- " + said(h));
    } else {
        std::printf("  [skip] no cut of the live tooltip here (testdata\\round7)\n");
    }
    {
        // Without the close buttons' pictures no window can be ruled out.
        TemplateStore pictures = k.pictures;
        TemplateStore without;
        for (const std::string& name : pictures.names()) {
            bool close = false;
            for (const char* x : bag::kCloseButtons) close = close || name == x;
            if (!close) without.put(name, *pictures.get(name));
        }
        PartyHudReader reader;
        const bool loaded = reader.load(without);
        const PartyHud h = reader.read(k.frame);
        check(loaded && h.state == PartyState::Alone && !h.sure_alone &&
                  h.alone_doubt_vi == party_text::kDoubtNoPictures && !party_invite_gate(h, true).allowed,
              "without the close buttons' pictures: alone, never surely -- " + said(h));
        // Nor without the book's tab, or any one of the dialogs' pictures: a
        // window or a dialog not looked for may be up.
        std::vector<DialogSign> signs(std::begin(kWindowSigns), std::end(kWindowSigns));
        signs.insert(signs.end(), std::begin(kDialogSigns), std::end(kDialogSigns));
        std::vector<std::string> done;
        for (const DialogSign& sign : signs) {
            if (std::find(done.begin(), done.end(), sign.name) != done.end()) continue;
            done.push_back(sign.name);
            TemplateStore short_sign;
            for (const std::string& name : pictures.names())
                if (name != sign.name) short_sign.put(name, *pictures.get(name));
            PartyHudReader r;
            const bool l = r.load(short_sign);
            const PartyHud s = r.read(k.frame);
            const bool dialog_sign = std::any_of(std::begin(kDialogSigns), std::end(kDialogSigns),
                                                 [&](const DialogSign& d) { return std::string(d.name) == sign.name; });
            check(l && r.can_see_dialogs() != dialog_sign && s.state == PartyState::Alone && !s.sure_alone &&
                      s.alone_doubt_vi == party_text::kDoubtNoPictures,
                  std::string("nor without ") + sign.name + ": alone, never surely -- " + said(s));
        }
        // Nor with all but one: the box's X under the pointer, not looked
        // for, is a window that may be open.
        TemplateStore all_but_one;
        for (const std::string& name : pictures.names())
            if (name != "inven/1/btn_CloseBox.spr/1") all_but_one.put(name, *pictures.get(name));
        PartyHudReader short_one;
        const bool loaded_short = short_one.load(all_but_one);
        const PartyHud s = short_one.read(k.frame);
        check(loaded_short && s.state == PartyState::Alone && !s.sure_alone &&
                  s.alone_doubt_vi == party_text::kDoubtNoPictures,
              "nor without the picture of the box's X under the pointer alone -- " + said(s));
    }
    // The four live party frames are never alone at all, and a member whose
    // leader's panel is under the bag is no reading: see test_party_under_window
    // and test_bag_over for the rest.
}

// ------------------------------------------------------------------ round 7b

/// A tooltip as the game draws one, at `x`,`y`: a dark box with the border
/// three pixels thick in its fixed colours, outside in, and rows of light
/// words in it. Where it is.
ScreenRect draw_tooltip(Frame& f, int x, int y, int w = 150, int h = 90) {
    fill(f, x, y, w, h, 24, 24, 44);
    const std::uint32_t rings[] = { PartyHudReader::kTooltipOuter, PartyHudReader::kTooltipMiddle,
                                    PartyHudReader::kTooltipInner };
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
/// what it shows of an invitation noted (PartyEvents::invitation) and, with
/// auto-accept, the invitation answered, the answer noted -- a party joined;
/// the chat lines read; then each asker by the party on a look grabbed after
/// the last change -- this pass's, or a new one when a change came after it
/// (read_party, party_invite_gate) -- an invite noted as a change. A pass
/// ends 150 ms on, as the loop's does ([Chat] vote_gap_ms). The loop reads
/// the inviter's name with the game's font before it answers; no part of
/// this.
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
            // Looked for whenever the tick's invite could go from alone, as the
            // loop does: answered by hand, the party changes all the same.
            PartyInvite::Seen seen;
            if (auto_accept || tick) {
                seen = answering.look(look.frame, store, nullptr);
                events.invitation(look.grabbed_ms, seen.up && !bag::bin_asking(look.frame, store));
            }
            if (auto_accept && seen.up) {
                g.t += 650;     // the focus, and the settle before the click
                g.accept();
                ++accepted;
                events.answered(g.t);
                events.joined(g.t, "an invitation was accepted", party_text::kEventAccepted);
            }
            g.t += 20;          // the chat lines read with the game's font
            for (int i = 0; i < askers; ++i) {
                if (events.last_change().at_ms >= look.grabbed_ms) {
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
    section("Party HUD: an invitation accepted in the same pass -- the look from before it is no reading for an "
            "invite, and alone waits for the game to draw the party (R7-3, a pretend game)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    const Frame* buttons = k.pictures.get("grabbed/party_invite_buttons");
    if (buttons == nullptr) {
        check(false, "the invitation's buttons are among the pictures the reader loads");
        return;
    }
    const auto game = [&] {
        PretendPartyGame g;
        g.alone = k.frame;
        g.member = k.frame;
        draw_member(g.member, 0, 112, Mark::Whole);
        g.invitation = *buttons;
        g.invited = true;
        return g;
    };
    const std::string changed_vi = std::string(party_text::kAloneUnsure) + " (" + party_text::kDoubtJoined + ": " +
                                   party_text::kEventAccepted + ")";
    {
        // As round 7's final check found it: B's invitation up, the player
        // alone, auto-accept on, the tick on, and C asking in the same pass.
        // C is read on a new look, taken after the accept -- the pass's own
        // is from before it -- and the game has not drawn B's party yet.
        PretendPartyGame g = game();
        PretendWorker w(k.reader, k.pictures);
        const Frame before = g.shows();
        w.pass(g, 1);
        const std::string vi = w.refusals.empty() ? std::string() : w.refusals[0].reason_vi;
        check(w.accepted == 1 && w.invited == 0 && w.fresh_looks == 1 && w.refusals.size() == 1 && vi == changed_vi,
              "B's invitation accepted, then C asks in the same pass: read on a new look, not invited -- \"" + vi +
                  "\" / \"" + (w.refusals.empty() ? std::string() : w.refusals[0].reason_en) + "\"");
        // The pass's own look, from before the accept, is no reading for it
        // at all, whatever it shows.
        PartyLook pass_look;
        pass_look.frame = before;
        pass_look.trusted = pass_look.whole = true;
        pass_look.grabbed_ms = w.events.last_change().at_ms - 680;
        const PartyGate on_pass_look = party_invite_gate(read_party(k.reader, pass_look, w.events), false);
        check(!on_pass_look.allowed && starts_with(on_pass_look.reason_vi, party_text::kStale),
              "that pass's own look, grabbed before the accept: no invite on it even with the tick off -- \"" +
                  on_pass_look.reason_vi + "\"");
        // And that look by its frame alone: B's invitation on it is a dialog,
        // so not surely alone either.
        const PartyHud by_frame = k.reader.read(before);
        check(by_frame.state == PartyState::Alone && !by_frame.sure_alone &&
                  by_frame.alone_doubt_vi == party_text::kDoubtDialog,
              "the look itself, B's invitation on it: alone, not surely -- " + said(by_frame));
        // The passes after it, 150 ms apart, an asker in each, for five
        // seconds: the game draws B's party 0.9 s after the click. Never
        // invited -- not surely alone while the party is not drawn, then a
        // member under B's crown.
        int unsure = 0, member = 0, other = 0;
        std::string first_other;
        const std::uint64_t until = g.t + 5000;
        while (g.t < until) w.pass(g, 1);
        for (std::size_t i = 1; i < w.refusals.size(); ++i) {
            const std::string& r = w.refusals[i].reason_vi;
            if (r == changed_vi) ++unsure;
            else if (r == party_text::kNotLeader) ++member;
            else if (++other == 1) first_other = r;
        }
        check(w.invited == 0 && unsure > 0 && member > 0 && other == 0,
              fmt("the passes after it, over 5 s: invited %d; not surely alone, the party not drawn yet, %d times; "
                  "a member under B's crown %d times", w.invited, unsure, member) +
                  (first_other.empty() ? std::string() : " -- also: " + first_other));
        // The hole as it was: a pass 150 ms after the click, by the frame
        // alone, reads surely alone -- B's party not drawn yet -- and invites.
        PretendPartyGame h = game();
        h.accept();
        h.t += 150;
        const PartyHud old = k.reader.read(h.shows());
        PartyEvents events;
        events.joined(h.t - 150, "an invitation was accepted", party_text::kEventAccepted);
        PartyLook look;
        take_party_look(h.sight(), look);
        const PartyHud now = read_party(k.reader, look, events);
        check(old.state == PartyState::Alone && old.sure_alone && party_invite_gate(old, true).allowed &&
                  now.state == PartyState::Alone && !now.sure_alone &&
                  starts_with(now.alone_doubt_vi, party_text::kDoubtJoined) && !party_invite_gate(now, true).allowed,
              "150 ms after the click, B's party not drawn: by the frame alone surely alone and invited (as round "
              "7 had it); held up against the accept, not surely -- " + said(now));
    }
    {
        // The tick off: invited as before the HUD was read -- on a look taken
        // after the accept, never on the pass's own from before it.
        PretendPartyGame g = game();
        PretendWorker w(k.reader, k.pictures);
        w.tick = false;
        w.pass(g, 1);
        check(w.accepted == 1 && w.invited == 1 && w.fresh_looks == 1 && w.refusals.empty(),
              "the tick off: C, asking in the pass that accepted, invited as before -- on a new look taken after "
              "the accept");
    }
    {
        // Auto-accept off: the player answers B's invitation by hand, between
        // two passes. The invitation was seen, and then seen gone with no
        // answer from GoBot: a party perhaps joined, and alone waits for the
        // game -- on a new look, the pass's own being from the moment it was
        // seen gone.
        PretendPartyGame g = game();
        PretendWorker w(k.reader, k.pictures);
        w.auto_accept = false;
        w.pass(g, 1);
        g.accept();
        w.pass(g, 1);
        const std::string gone_vi = std::string(party_text::kAloneUnsure) + " (" + party_text::kDoubtJoined + ": " +
                                    party_text::kEventInvitationGone + ")";
        check(w.invited == 0 && w.refusals.size() == 2 && w.refusals[0].reason_vi.find(party_text::kDoubtDialog) !=
                                                               std::string::npos &&
                  w.refusals[1].reason_vi == gone_vi && w.fresh_looks == 1,
              "auto-accept off, B's invitation accepted by hand between two passes: the first asker refused for "
              "the invitation on screen, the next for a party perhaps joined and not drawn yet -- \"" +
                  (w.refusals.size() > 1 ? w.refusals[1].reason_vi : std::string()) + "\"");
    }
}

void test_party_drawn_late() {
    section("Party HUD: a party joined and drawn late -- alone stays unsure until it is drawn, or 20 s go by "
            "without it (R7b-3, a pretend game)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    const Frame* buttons = k.pictures.get("grabbed/party_invite_buttons");
    if (buttons == nullptr) {
        check(false, "the invitation's buttons are among the pictures the reader loads");
        return;
    }
    const auto game = [&](std::uint64_t draw_ms, bool takes) {
        PretendPartyGame g;
        g.alone = k.frame;
        g.member = k.frame;
        draw_member(g.member, 0, 112, Mark::Whole);
        g.invitation = *buttons;
        g.invited = true;
        g.draw_ms = draw_ms;
        g.takes = takes;
        return g;
    };
    const std::string joined_vi = std::string(party_text::kAloneUnsure) + " (" + party_text::kDoubtJoined + ": " +
                                  party_text::kEventAccepted + ")";
    for (const std::uint64_t draw : { std::uint64_t{ 3500 }, std::uint64_t{ 9000 } }) {
        // GoBot accepts B's invitation; the game draws B's party `draw` ms
        // later -- past the 3 s round 7b waited -- and C asks every 0.6 s.
        PretendPartyGame g = game(draw, true);
        PretendWorker w(k.reader, k.pictures);
        w.pass(g, 1);
        const std::uint64_t accepted = w.events.last_join().at_ms;
        int unsure = 0, member = 0, other = 0;
        std::uint64_t last_unsure = 0;
        for (const std::uint64_t at : { std::uint64_t{ 1000 }, std::uint64_t{ 2000 }, std::uint64_t{ 3200 },
                                        draw - 200, draw + 400, draw + 1500 }) {
            g.t = accepted + at;
            const std::size_t had = w.refusals.size();
            w.pass(g, 1);
            for (std::size_t i = had; i < w.refusals.size(); ++i) {
                if (w.refusals[i].reason_vi == joined_vi) {
                    ++unsure;
                    last_unsure = at;
                } else if (w.refusals[i].reason_vi == party_text::kNotLeader) {
                    ++member;
                } else {
                    ++other;
                }
            }
        }
        // The look round 7b went by, 3.1 s after the accept with the party not
        // drawn: by the frame and the 3 s settle alone, surely alone.
        PartyEvents settle_only;
        settle_only.changed(accepted, "an invitation was accepted", party_text::kEventAccepted);
        PartyLook at_3s;
        at_3s.frame = k.frame;
        at_3s.trusted = at_3s.whole = true;
        at_3s.grabbed_ms = accepted + 3100;
        const bool old_rule = read_party(k.reader, at_3s, settle_only).sure_alone;
        check(w.accepted == 1 && w.invited == 0 && unsure == 4 && member == 2 && other == 0 &&
                  last_unsure == draw - 200 && old_rule,
              fmt("B's party drawn %llu ms after GoBot's accept, C asking 1, 2, 3.2 s after it and on either side of "
                  "the drawing: invited %d; not surely alone %d times, the last %llu ms after the accept; then a "
                  "member %d times (round 7b's rule: surely alone 3.1 s after the accept with nothing drawn)",
                  static_cast<unsigned long long>(draw), w.invited, unsure,
                  static_cast<unsigned long long>(last_unsure), member));
    }
    {
        // An accept that never took -- B's party full, or B gone: no party is
        // ever drawn. Alone stays unsure for 20 s, and then is sure again.
        PretendPartyGame g = game(900, false);
        PretendWorker w(k.reader, k.pictures);
        w.pass(g, 1);
        const std::uint64_t accepted = w.events.last_join().at_ms;
        for (const std::uint64_t at : { std::uint64_t{ 5000 }, std::uint64_t{ 12'000 }, PartyEvents::kJoinWaitMs - 500 }) {
            g.t = accepted + at;
            w.pass(g, 1);
        }
        const int before = w.invited;
        g.t = accepted + PartyEvents::kJoinWaitMs + 100;
        w.pass(g, 1);
        check(w.accepted == 1 && before == 0 && w.invited == 1,
              fmt("an accept that never took: not invited 5, 12 and 19.5 s after it, nothing drawn; invited at 20 s "
                  "(%d)", w.invited));
    }
    {
        // The player answers by hand with the worker looking (auto-accept
        // off, the tick on): the invitation seen, then gone with no answer of
        // GoBot's -- B's party drawn 5 s later. Never invited meanwhile.
        PretendPartyGame g = game(5000, true);
        PretendWorker w(k.reader, k.pictures);
        w.auto_accept = false;
        w.pass(g, 1);
        g.accept();
        const std::uint64_t by_hand = g.t;
        while (g.t < by_hand + 7000) {
            g.t += 400;
            w.pass(g, 1);
        }
        int member = 0;
        for (const PartyGate& r : w.refusals) member += r.reason_vi == party_text::kNotLeader ? 1 : 0;
        check(w.invited == 0 && member > 0 && w.events.last_join().vi == party_text::kEventInvitationGone,
              fmt("B's invitation accepted by hand, the party drawn 5 s later: never invited -- a member %d times "
                  "once drawn", member));
    }
}

void test_plain_alone_invites() {
    section("Party HUD: the plain case -- surely alone, ticked, invites; then waits for the party it may have "
            "started (a pretend game)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    PretendPartyGame g;
    g.alone = k.frame;
    g.member = k.frame;
    PretendWorker w(k.reader, k.pictures);
    w.pass(g, 1);
    check(w.invited == 1 && w.refusals.empty(), "nobody in a party, nothing on the game, the tick on: C invited, to "
                                                "start a party");
    // D asks in the same pass's wake: the look is from before C's invite, and
    // the next ones too soon after it -- C may be joining.
    w.pass(g, 2);
    const bool waited = w.invited == 1 && w.refusals.size() == 2 &&
                        starts_with(w.refusals[1].reason_vi, std::string(party_text::kAloneUnsure));
    // Three seconds on, C never having answered: alone, surely, again.
    g.t += PartyEvents::kSettleMs;
    w.pass(g, 1);
    check(waited && w.invited == 2,
          "D, asking just after: not invited until the game could have drawn C's answer; three seconds on, invited");
    {
        // Read off the screen, all of the game in view before and after the
        // grab: as the game's own picture.
        std::uint64_t t = 1'000'000;
        PartyLook look;
        const bool took = take_party_look(still_sight(k.frame, t, true), look);
        const PartyEvents none;
        const PartyHud h = read_party(k.reader, look, none);
        check(took && look.from_screen && look.trusted && look.whole && h.sure_alone &&
                  party_invite_gate(h, true).allowed,
              "frame.png read off the screen, all of the game in view at the grab: surely alone, invited -- " +
                  said(h));
    }
    Frame small;
    if (load_png(std::string(kLiveBuild) + "bin\\Release\\playerlist.png", small) && small.width == 1024) {
        std::uint64_t t = 1'000'000;
        PartyLook look;
        take_party_look(still_sight(small, t, false), look);
        const PartyEvents none;
        const PartyHud h = read_party(k.reader, look, none);
        check(h.sure_alone && party_invite_gate(h, true).allowed,
              "playerlist.png, 1024x768, as the game drew it: surely alone, invited -- " + said(h));
    }
}

void test_look_at_grab() {
    section("Party HUD: trusted and whole as they were when the frame was grabbed, not when the gate runs "
            "(R7-5, a pretend game)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    const Frame* x = k.pictures.get(bag::kCloseButtons[0]);
    Frame live;
    if (x == nullptr || !load_png(std::string(kLiveBuild) + kPartyProbe, live) || live.width != 1280) {
        std::printf("  [skip] no saved frame of the live party, or no close button's picture, here\n");
        return;
    }
    // The live party of two under a window of the game whose X is far right
    // of the panels, and another program's window over the right of the game
    // and that X, as the frame read off the screen has it.
    Frame f = live;
    const ScreenRect at = game_window(f, ScreenRect{ 0, 90, 700, 420 }, *x);
    other_program(f, ScreenRect{ 400, 0, f.width - 400, f.height });
    // The other program goes 600 ms after the grab began: the party worker
    // answers an invitation or reads the HUD first, and only then asks.
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
    s.seen = [&](const ScreenRect& part, std::string* why) {
        const bool over = (gone == 0 || t < gone) && part.right() > 400;
        if (why) *why = over ? "Notepad.exe" : "";
        return !over || part.right() <= area.right();
    };
    s.now = [&t] { return t; };
    PartyLook look;
    const bool took = take_party_look(s, look);
    t += 700;
    // What asking at the gate says -- the window gone -- and the frame read
    // by that: surely alone, invited with the tick. The hole as it was.
    std::string now_why;
    const bool whole_now = s.seen(ScreenRect{ 0, 0, f.width, f.height }, &now_why);
    const PartyHud late = k.reader.read(look.frame, true, whole_now);
    // The look as it was grabbed: not whole, so not surely alone.
    const PartyEvents none;
    const PartyHud h = read_party(k.reader, look, none);
    const PartyGate g = party_invite_gate(h, true);
    check(took && look.trusted && !look.whole && look.in_the_way == "Notepad.exe" && whole_now && late.sure_alone &&
              party_invite_gate(late, true).allowed && h.state == PartyState::Alone && !h.sure_alone &&
              h.alone_doubt_vi == party_text::kDoubtHidden && !g.allowed,
          fmt("the X at %d,%d under another program at the grab, gone by the gate: judged at the gate, surely "
              "alone and invited; judged at the grab, \"", at.x, at.y) + g.reason_vi + "\"");
    // A window that came or went during the grab: whole neither way.
    for (const bool came : { true, false }) {
        int calls = 0;
        PartySight m;
        m.grab = [&](Frame& out, bool& from_screen) {
            out = k.frame;
            from_screen = true;
            return true;
        };
        m.seen = [&](const ScreenRect& part, std::string* why) {
            const bool after = calls++ >= 2;
            const bool over = part.right() > area.right() && (came ? after : !after);
            if (why) *why = over ? "Notepad.exe" : "";
            return !over;
        };
        m.now = [&t] { return t; };
        PartyLook l;
        take_party_look(m, l);
        const PartyHud r = read_party(k.reader, l, none);
        check(l.trusted && !l.whole && !r.sure_alone && r.alone_doubt_vi == party_text::kDoubtHidden,
              std::string("another program's window ") + (came ? "come" : "gone") +
                  " during the grab, frame.png under it: not whole, not surely alone -- " + said(r));
    }
    // The game's own picture of itself, whatever is over its window: whole.
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
        check(!l.from_screen && l.trusted && l.whole && read_party(k.reader, l, none).sure_alone,
              "a frame the game drew itself, another program over its window: trusted, whole, surely alone");
    }
}

void test_tooltip_over_x() {
    section("Party HUD: a window's X under a tooltip anywhere is no window ruled out (R7-4)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    const Frame* x = k.pictures.get(bag::kCloseButtons[0]);
    Frame live;
    if (x == nullptr || !load_png(std::string(kLiveBuild) + kPartyProbe, live) || live.width != 1280) {
        std::printf("  [skip] no saved frame of the live party, or no close button's picture, here\n");
        return;
    }
    // The live party under a window as wide as Kho Cash, its X at 237,68 --
    // right of the panels' column -- and a tooltip over that X, as the game
    // draws one over its windows.
    Frame f = live;
    const ScreenRect at = game_window(f, ScreenRect{ 0, 62, 261, 330 }, *x);
    const PartyHud with_x = k.reader.read(f);
    draw_tooltip(f, 200, 60);
    const PartyHud h = k.reader.read(f);
    ScreenRect edge;
    const bool tip = PartyHudReader::tooltip_edge(f, ScreenRect{ 0, 0, f.width, f.height }, &edge);
    // Round 7 looked for an edge only over the panels' column, left of
    // x=134, and found none there: no window, no tooltip, surely alone.
    const bool in_column = PartyHudReader::tooltip_edge(f, ScreenRect{ 0, 40, 134, 440 });
    check(with_x.state == PartyState::Alone && with_x.alone_doubt_vi == party_text::kDoubtWindow && tip &&
              !in_column && edge.x >= 134 && h.state == PartyState::Alone && !h.sure_alone &&
              h.alone_doubt_vi == party_text::kDoubtTooltip && !party_invite_gate(h, true).allowed,
          fmt("the live party under a window, its X at %d,%d under a tooltip whose edge is at %d,%d, right of the "
              "panels' column where round 7 looked: alone, not surely, not invited -- ", at.x, at.y, edge.x,
              edge.y) + said(h));
    const Frame cut = live_cut("tooltip");
    if (cut.valid()) {
        Frame g = live;
        game_window(g, ScreenRect{ 0, 62, 261, 330 }, *x);
        blit(g, cut, 150, 60);
        const PartyHud c = k.reader.read(g);
        check(c.state == PartyState::Alone && !c.sure_alone && c.alone_doubt_vi == party_text::kDoubtTooltip &&
                  !party_invite_gate(c, true).allowed,
              "the same with the live tooltip over the X: alone, not surely -- " + said(c));
    }
    // The same review's other case: the live monster book, its X held down --
    // which no drawing of a close button matches -- laid over the live
    // party, as book_A_154 shows it. Cut in memory, never saved.
    Frame book_frame;
    TemplateStore book_pictures;
    const Frame title = installed("grabbed/monster_book_title"), close = installed("grabbed/monster_book_close");
    if (title.valid() && close.valid() && load_png(std::string(kLiveBuild) + "live\\shots\\book_A_154.png", book_frame)) {
        book_pictures.put(bag::kBookTitle, title);
        book_pictures.put(bag::kBookClose, close);
        const bag::Book b = bag::find_book(book_frame, book_pictures);
        if (b.found && b.window.x >= 0 && b.window.y >= 0 && b.window.right() <= book_frame.width &&
            b.window.bottom() <= book_frame.height) {
            Frame g = live;
            blit(g, cut_of(book_frame, b.window), 0, 60);
            int xs = 0;
            for (const char* name : bag::kCloseButtons)
                if (const Frame* p = k.pictures.get(name))
                    xs += TemplateMatcher::find(g, ScreenRect{ 0, 0, g.width, g.height }, *p, PartyHudReader::kWindowXScore)
                              .found;
            for (const char* name : kCloseButtonStates)
                if (const Frame* p = k.pictures.get(name))
                    xs += TemplateMatcher::find(g, ScreenRect{ 0, 0, g.width, g.height }, *p, PartyHudReader::kWindowXScore)
                              .found;
            const PartyHud bk = k.reader.read(g);
            check(!b.has_x && xs == 0 && bk.state == PartyState::Alone && !bk.sure_alone &&
                      bk.alone_doubt_vi == party_text::kDoubtWindow &&
                      bk.alone_doubt.find("monster book") != std::string::npos && !party_invite_gate(bk, true).allowed,
                  "the live monster book, its X held down, over the live party at 0,60: no close button seen, the "
                  "book by its tab -- alone, not surely, not invited -- " + said(bk));
        } else {
            std::printf("  [skip] the monster book not found whole on book_A_154 here\n");
        }
    } else {
        std::printf("  [skip] no book_A_154.png or the book's pictures here\n");
    }
    // A tooltip anywhere on the game's picture, nothing else on it.
    for (const int tx : { 700, 1100 }) {
        Frame g = k.frame;
        draw_tooltip(g, tx, 400);
        const PartyHud t = k.reader.read(g);
        check(t.state == PartyState::Alone && !t.sure_alone && t.alone_doubt_vi == party_text::kDoubtTooltip &&
                  !party_invite_gate(t, true).allowed,
              fmt("frame.png, a tooltip at %d,400 and nothing else: alone, not surely -- ", tx) + said(t));
    }
}

void test_dialogs_over_panels() {
    section("Party HUD: any dialog of the game, anywhere, X or none -- a question, a notice, the client's dialog "
            "frame -- and alone is not sure (the round 7 tester's case)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    if (!k.reader.can_see_dialogs()) {
        check(false, "the dialogs' pictures are among those the reader loads");
        return;
    }
    Frame live;
    const bool have_live = load_png(std::string(kLiveBuild) + kPartyProbe, live) && live.width == 1280;
    struct Paste {
        const char* what;
        Frame       pic;
        int         dx = 0, dy = 0;     ///< where its dialog's top-left is in it
        bool        corner = true;      ///< the cut holds the frame's top-left corner
    };
    std::vector<Paste> pastes;
    const auto add = [&](const char* what, const Frame* pic, int dx = 0, int dy = 0, bool corner = true) {
        if (pic != nullptr && pic->valid()) pastes.push_back(Paste{ what, *pic, dx, dy, corner });
        else std::printf("  [skip] no picture of %s here\n", what);
    };
    const Frame question = installed(bag::kRegisterDialog);
    add("the registration question (grabbed/fragment_register)", question.valid() ? &question : nullptr);
    // A cut of the live client's notice that starts inside its frame.
    add("a notice with Xác Nhận alone (grabbed/dialog_ok)", k.pictures.get("grabbed/dialog_ok"), 0, 0, false);
    add("the client's dialog frame with its two buttons (inven/1/dialog)", k.pictures.get("inven/1/dialog"));
    add("the client's taller dialog frame (inven/1/dialog_2)", k.pictures.get("inven/1/dialog_2"));
    // The client's frames with nothing in them but their words' panel: no
    // button to be found by -- a dialog whose buttons are drawn otherwise.
    Frame bare, bare_tall;
    if (const Frame* d = k.pictures.get("inven/1/dialog")) {
        bare = *d;
        fill(bare, 100, 54, 96, 26, 214, 214, 222);   // the frame's own light grey
        add("the client's dialog frame, its buttons painted out", &bare);
    }
    if (const Frame* d = k.pictures.get("inven/1/dialog_2")) {
        bare_tall = *d;
        fill(bare_tall, 100, d->height - 32, 96, 26, 214, 214, 222);
        add("the client's taller dialog frame, its buttons painted out", &bare_tall);
    }
    // The live client's own, cut with 8 pixels round them: the question, the
    // same with Hủy Bỏ held down -- which the two buttons' pictures miss --
    // and the bin's quantity question, cut from inside its frame.
    const Frame q_rest = live_cut("question_rest"), q_held = live_cut("question_pressed"), bin = live_cut("bin_question");
    add("the live registration question", q_rest.valid() ? &q_rest : nullptr, 8, 8);
    add("the live question, Hủy Bỏ held down", q_held.valid() ? &q_held : nullptr, 8, 8);
    add("the live bin's quantity question", bin.valid() ? &bin : nullptr, 0, 0, false);
    // The client's dialog frames by their corners, the signs that need
    // neither buttons nor words: each paste that holds a frame's corner is
    // found by one of them on its own.
    std::vector<Frame> corners;
    for (const DialogSign& sign : kDialogSigns) {
        const std::string n = sign.name;
        const bool frame = n == "inven/1/dialog" || n == "inven/1/dialog_2";
        if (frame && sign.part.x == 0 && sign.part.y == 0 && k.pictures.get(sign.name) != nullptr)
            corners.push_back(cut_of(*k.pictures.get(sign.name), sign.part));
    }
    for (const Paste& p : pastes) {
        // On frame.png where the round 7 tester laid them, and over the live
        // party's leader's panel, crown and all.
        for (const int px : { 0, 20 }) {
            Frame f = k.frame;
            blit(f, p.pic, px - p.dx, 120 - p.dy);
            ScreenRect at;
            std::string what;
            const bool up = k.reader.dialog_up(f, &at, &what);
            const PartyHud h = k.reader.read(f);
            // The best of the frames' corners where this dialog's corner is.
            float corner = -1.0f;
            for (const Frame& c : corners) {
                const auto m =
                    TemplateMatcher::find(f, ScreenRect{ std::max(0, px - 1), 119, c.width + 2, c.height + 2 }, c, -1.0f);
                if (m.found) corner = std::max(corner, m.score);
            }
            check(up && h.state == PartyState::Alone && !h.sure_alone &&
                      h.alone_doubt_vi == party_text::kDoubtDialog && !party_invite_gate(h, true).allowed &&
                      (!p.corner || corner >= kDialogFrameScore),
                  std::string(p.what) + fmt(" on frame.png at %d,120: ", px) +
                      (up ? what + fmt(" at %d,%d", at.x, at.y) : std::string("no dialog seen")) +
                      (p.corner ? fmt("; a frame's corner there %.3f", static_cast<double>(corner)) : std::string()) +
                      " -- alone, not surely, not invited");
        }
        if (!have_live) continue;
        Frame f = live;
        blit(f, p.pic, -p.dx, 100 - p.dy);
        const PartyHud h = k.reader.read(f);
        check(h.state != PartyState::Member && !h.sure_alone && !party_invite_gate(h, true).allowed &&
                  (h.state != PartyState::Alone || h.alone_doubt_vi == party_text::kDoubtDialog),
              std::string(p.what) + " over the live leader's panel and crown: " + said(h) + " -- not invited");
    }
    // A dialog seen by its frame's top edge alone, the rest of it under a
    // window or past the screen's bottom: the corner is enough.
    if (const Frame* d = k.pictures.get("inven/1/dialog")) {
        Frame f = k.frame;
        blit(f, cut_of(*d, ScreenRect{ 0, 0, d->width, 14 }), 0, 120);
        ScreenRect at;
        std::string what;
        const bool up = k.reader.dialog_up(f, &at, &what);
        const PartyHud h = k.reader.read(f);
        check(up && what == "the client's dialog frame" && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtDialog,
              "only the top 14 rows of the client's dialog frame, at 0,120: " + what + fmt(" at %d,%d", at.x, at.y) +
                  " -- alone, not surely");
    }
    // Nothing of a dialog on frame.png or the live party as saved.
    check(!k.reader.dialog_up(k.frame) && (!have_live || !k.reader.dialog_up(live)),
          "frame.png and the live party as saved: no dialog");
    // A saved frame of the live client with the question up, as it was.
    Frame q;
    if (load_png(std::string(kLiveBuild) + "live\\shots\\fragq_A_085.png", q)) {
        ScreenRect at;
        std::string what;
        const bool up = k.reader.dialog_up(q, &at, &what);
        check(up, "the live question on fragq_A_085: a dialog -- " + what + fmt(" at %d,%d", at.x, at.y));
    }
}

void test_windows_without_x() {
    section("Party HUD: the game's windows and dialogs with no X of their own -- the party's menu and buttons, the "
            "NPC shop, the death panel, the bin's and the party's name dialogs, a notice, the ESC menu, the area's "
            "player list, a second character panel -- and alone is not sure (R7b-1, R7b-2, the round 7b tester)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    Frame live, small;
    const bool have_live = load_png(std::string(kLiveBuild) + kPartyProbe, live) && live.width == 1280;
    const bool have_small =
        load_png(std::string(kLiveBuild) + "bin\\Release\\playerlist.png", small) && small.width == 1024;
    // Each where the round 7b reviewer laid it over the live party's leader's
    // panel and crown -- a member, then, whose leader's panel is hidden --
    // and where the tester laid it on frame.png and playerlist.png.
    struct Pic {
        const char* what;
        const char* name;
        int         lx, ly;     ///< over the live leader's panel
    };
    const Pic pics[] = {
        { "the party menu", "inven/1/dialog_partymenu.spr/0", -10, 60 },
        { "the party's buttons", "grabbed/party_panel", 0, 106 },
        { "the NPC shop", "inven/1/Dialog_Shop.spr/0", -20, 40 },
        { "the death panel, the client's picture", "inven/1/dialog_Panic.spr/0", -20, 65 },
        { "the death panel, the live client's", "grabbed/chardeadpanel", 0, 90 },
        { "the bin's quantity dialog, cut inside its frame", "grabbed/del_item_confirm_2", 0, 100 },
        { "the party's name dialog, cut inside its frame", "grabbed/party_panel_confirm", 0, 100 },
        { "a notice, cut inside its frame", "grabbed/login_popup1", 0, 100 },
        { "the ESC menu", "inven/1/dialog_setting.spr/0", 0, 60 },
        { "the area's player list", "inven/1/dialog_marea.spr/0", -10, 65 },
        { "a second character panel", "inven/1/Dialog_CharInfo.spr/0", 0, 100 },
    };
    for (const Pic& p : pics) {
        const Frame pic = installed(p.name);
        if (!pic.valid()) {
            check(false, std::string(p.name) + " is installed here");
            continue;
        }
        bool all = true;
        int places = 0;
        std::string first_bad;
        const auto at = [&](const Frame& base, int x, int y, const char* on) {
            if (x + pic.width > base.width || y + pic.height > base.height) return;
            Frame f = base;
            blit(f, pic, x, y);
            const PartyHud h = k.reader.read(f);
            ++places;
            if (h.sure_alone || party_invite_gate(h, true).allowed) {
                if (all) first_bad = fmt(" -- on %s at %d,%d: ", on, x, y) + said(h);
                all = false;
            }
        };
        if (have_live) at(live, p.lx, p.ly, "the live party");
        at(k.frame, 0, 120, "frame.png");
        at(k.frame, 400, 200, "frame.png");
        if (have_small) at(small, 0, 60, "playerlist.png");
        check(all && places > 0, std::string(p.what) + " (" + p.name + ") at " + std::to_string(places) +
                                     " places: never surely alone, never invited with the tick" + first_bad);
    }
    // The party menu over the live party's leader's crown, where round 7b
    // read the member as surely alone: alone still, and not surely, by the
    // party's menu.
    if (have_live) {
        const Frame menu = installed("inven/1/dialog_partymenu.spr/0");
        if (menu.valid()) {
            Frame f = live;
            blit(f, menu, -10, 60);
            const PartyHud h = k.reader.read(f);
            check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtWindow &&
                      h.alone_doubt.find("party") != std::string::npos,
                  "the party menu over the live leader's crown: the member reads alone, and not surely -- the "
                  "party's menu seen -- " + said(h));
        }
    }
    // Nothing of these on the frames as saved.
    check(k.reader.read(k.frame).sure_alone && (!have_small || k.reader.read(small).sure_alone),
          "frame.png and playerlist.png as saved: surely alone, as before");
}

void test_read_party() {
    section("Party HUD: a look held up against what changed the party (PartyEvents), and the game's own word "
            "(PartyMembership, P0-3's seam)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    std::uint64_t t = 90'000'000;
    PartyLook look;
    take_party_look(still_sight(k.frame, t, false), look);
    {
        PartyEvents e;
        const PartyHud before = read_party(k.reader, look, e);
        e.joined(look.grabbed_ms + 100, "an invitation was accepted", party_text::kEventAccepted);
        const PartyHud h = read_party(k.reader, look, e);
        const PartyGate on = party_invite_gate(h, true), off = party_invite_gate(h, false);
        check(before.sure_alone && !before.stale && h.stale && !h.sure_alone && !on.allowed && !off.allowed &&
                  on.reason_vi == std::string(party_text::kStale) + " (" + party_text::kEventAccepted + ")" &&
                  off.reason_vi == on.reason_vi && on.reason_en.find("accepted") != std::string::npos,
              "a look grabbed before a change GoBot made: no invite on it, ticked or not -- \"" + on.reason_vi +
                  "\" / \"" + on.reason_en + "\"");
    }
    {
        // After the change, a look too soon to say alone by, then one late
        // enough.
        PartyEvents e;
        e.changed(t, "an invite was sent", party_text::kEventInvited);
        t += 100;
        PartyLook soon, late;
        take_party_look(still_sight(k.frame, t, false), soon);
        t += PartyEvents::kSettleMs;
        take_party_look(still_sight(k.frame, t, false), late);
        const PartyHud s = read_party(k.reader, soon, e), l = read_party(k.reader, late, e);
        check(!s.stale && s.state == PartyState::Alone && !s.sure_alone &&
                  s.alone_doubt_vi == std::string(party_text::kDoubtChanged) + ": " + party_text::kEventInvited &&
                  !l.stale && l.sure_alone,
              "grabbed 0.1 s after an invite: alone, not surely (" + s.alone_doubt + "); 3 s after: surely");
    }
    {
        // Something seen, not done: an invitation on screen is no change of
        // GoBot's -- a look from just before it is no stale one -- but alone
        // waits for it all the same.
        PartyEvents e;
        e.seen(look.grabbed_ms + 50, "an invitation was on screen", party_text::kEventInvitation);
        const PartyHud h = read_party(k.reader, look, e);
        check(!h.stale && !h.sure_alone && e.last_change().at_ms == 0 && e.last_any().vi == party_text::kEventInvitation,
              "an invitation seen: no change of GoBot's, but alone not sure -- " + said(h));
        // The latest of each is kept, an event told late included.
        e.changed(look.grabbed_ms + 200, "an invite was sent", party_text::kEventInvited);
        e.joined(look.grabbed_ms + 10, "an invitation was accepted", party_text::kEventAccepted);
        check(e.last_change().vi == party_text::kEventInvited && e.last_any().vi == party_text::kEventInvited &&
                  e.last_join().vi == party_text::kEventAccepted,
              "an event told late does not stand for a later one");
    }
    {
        // A party perhaps joined: not alone for sure until 20 s after it,
        // whatever came since; then the 3 s after the last event of any kind.
        PartyEvents e;
        const std::uint64_t at = 50'000'000;
        e.joined(at, "an invitation was accepted", party_text::kEventAccepted);
        const auto at_ms = [&](std::uint64_t ms) {
            PartyLook l = look;
            l.grabbed_ms = at + ms;
            return read_party(k.reader, l, e);
        };
        const PartyHud s3 = at_ms(3100), s19 = at_ms(PartyEvents::kJoinWaitMs - 100), s20 = at_ms(PartyEvents::kJoinWaitMs);
        e.changed(at + PartyEvents::kJoinWaitMs - 1000, "an invite was sent", party_text::kEventInvited);
        const PartyHud s21 = at_ms(PartyEvents::kJoinWaitMs + 1000), s23 = at_ms(PartyEvents::kJoinWaitMs + 2000);
        check(!s3.sure_alone && starts_with(s3.alone_doubt_vi, party_text::kDoubtJoined) && !s19.sure_alone &&
                  s20.sure_alone && !s21.sure_alone && starts_with(s21.alone_doubt_vi, party_text::kDoubtChanged) &&
                  s23.sure_alone && party_invite_gate(s3, true).reason_vi ==
                                        std::string(party_text::kAloneUnsure) + " (" + party_text::kDoubtJoined + ": " +
                                            party_text::kEventAccepted + ")",
              "a party perhaps joined: not surely alone 3.1 or 19.9 s after it (\"" + s3.alone_doubt_vi +
                  "\"), surely at 20 s; an invite sent at 19 s: unsure until 22 s, then sure");
    }
    {
        // What the looks see of an invitation (PartyEvents::invitation): up,
        // seen; gone with no answer of GoBot's, a party perhaps joined --
        // once; an older look says nothing new; gone after GoBot's own
        // answer, nothing.
        PartyEvents e;
        e.invitation(1000, false);
        const bool none = e.last_any().at_ms == 0;
        e.invitation(2000, true);
        const bool seen = e.last_any().at_ms == 2000 && e.last_any().vi == party_text::kEventInvitation &&
                          e.last_join().at_ms == 0 && e.last_change().at_ms == 0;
        e.invitation(1900, false);
        const bool older = e.last_join().at_ms == 0;
        e.invitation(2600, false);
        const bool gone = e.last_join().at_ms == 2600 && e.last_join().vi == party_text::kEventInvitationGone &&
                          e.last_change().at_ms == 2600;
        e.invitation(2900, false);
        const bool once = e.last_join().at_ms == 2600;
        PartyEvents a;
        a.invitation(2000, true);
        a.answered(2300);
        a.changed(2300, "an invitation was declined", party_text::kEventDeclined);
        a.invitation(2200, true);       // a look from before the answer
        a.invitation(2600, false);
        const bool answered = a.last_join().at_ms == 0 && a.last_change().vi == party_text::kEventDeclined;
        a.invitation(5000, true);       // a new one
        a.invitation(5400, false);
        const bool next = a.last_join().at_ms == 5400;
        check(none && seen && older && gone && once && answered && next,
              "an invitation up is seen; gone unanswered, a party perhaps joined, once; an older look, or one gone "
              "after GoBot's own answer, nothing more; the next one gone unanswered, again");
    }
    {
        // The game's own word, once P0-3 gives it: in a party is not alone,
        // whatever the screen shows.
        const PartyEvents none;
        PartyMembership game;
        game.known = true;
        game.in_party = true;
        const PartyHud h = read_party(k.reader, look, none, game);
        PartyMembership alone_word;
        alone_word.known = true;
        const PartyHud a = read_party(k.reader, look, none, alone_word);
        check(!h.sure_alone && h.alone_doubt_vi == party_text::kDoubtGameSays && !party_invite_gate(h, true).allowed &&
                  a.sure_alone && read_party(k.reader, look, none).sure_alone,
              "the game's word that the player is in a party: not surely alone, not invited; its word of alone, or "
              "none yet: the screen decides");
    }
}

void test_chat_alone_rule() {
    section("Tự chat's \"Chỉ khi chưa có tổ đội\" (P2-5) by the same surely alone: unsure, the line waits");
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
    const PartyHud sure = read_party(k.reader, plain, none);
    check(alone_for_chat(sure) && turn(sure, one) == AloneTurn::Send,
          "surely alone, nothing on the game: A goes in its turn");
    struct Case {
        const char* what;
        Frame       f;
    };
    std::vector<Case> cases;
    {
        Frame f = k.frame;
        draw_tooltip(f, 800, 300);
        cases.push_back({ "a tooltip on the game", f });
    }
    const Frame question = installed(bag::kRegisterDialog);
    if (question.valid()) {
        Frame f = k.frame;
        blit(f, question, 0, 120);
        cases.push_back({ "a question over the panels", f });
    } else {
        std::printf("  [skip] no picture of the registration question here\n");
    }
    if (const Frame* x = k.pictures.get(bag::kCloseButtons[0])) {
        Frame f = k.frame;
        blit(f, *x, 900, 200);
        cases.push_back({ "a window of the game open", f });
    }
    for (const Case& c : cases) {
        PartyLook look;
        take_party_look(still_sight(c.f, t, false), look);
        const PartyHud h = read_party(k.reader, look, none);
        check(h.state == PartyState::Alone && !alone_for_chat(h) && turn(h, one) == AloneTurn::Skip &&
                  turn(h, both) == AloneTurn::Wait,
              std::string(c.what) + ": alone, not surely -- A passed over for B, or with every line ticked put off "
                                    "a minute -- " + said(h));
    }
    {
        // Part of the game unseen at the grab, and a party change a moment
        // ago: the same.
        PartyLook hidden = plain;
        hidden.from_screen = true;
        hidden.whole = false;
        PartyEvents e;
        e.changed(plain.grabbed_ms - 200, "an invitation was accepted", party_text::kEventAccepted);
        check(turn(read_party(k.reader, hidden, none), one) == AloneTurn::Skip &&
                  turn(read_party(k.reader, plain, e), one) == AloneTurn::Skip,
              "part of the game under another window at the grab, or an invitation accepted a moment ago: A waits");
    }
    // A party seen, or the HUD unread: as before.
    Frame member = k.frame;
    draw_member(member, 0, 112, Mark::Whole);
    check(!alone_for_chat(k.reader.read(member)) && !alone_for_chat(PartyHud{}),
          "a member, or the HUD not read: not alone, as before");
}

/// A pretend game for Tự chat's two looks, on a clock of the test's own: the
/// player alone; B's invitation up from `invite_from` until `answer_at`, when
/// the player accepts it by hand; B's party drawn `draw_ms` after that. 0: no
/// invitation, no answer.
struct ScriptedGame {
    std::uint64_t t = 40'000'000;
    Frame         alone, member, invitation;
    std::uint64_t invite_from = 0, answer_at = 0, draw_ms = 2500;
    Frame shows() const {
        Frame f = answer_at != 0 && t >= answer_at + draw_ms ? member : alone;
        if (invite_from != 0 && t >= invite_from && (answer_at == 0 || t < answer_at)) blit(f, invitation, 586, 600);
        return f;
    }
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

void test_chat_watch() {
    section("Tự chat's \"Chỉ khi chưa có tổ đội\": two looks 3 s apart, the game watched for an invitation between "
            "them (R7b-4, a pretend game)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    const Frame* buttons = k.pictures.get("grabbed/party_invite_buttons");
    if (buttons == nullptr) {
        check(false, "the invitation's buttons are among the pictures the reader loads");
        return;
    }
    PartyInvite finder;
    const std::function<bool(const Frame&)> up = [&](const Frame& f) {
        return finder.look(f, k.pictures, nullptr).up && !bag::bin_asking(f, k.pictures);
    };
    // As chat_alone_put_off takes them, with nothing from the party worker:
    // a look, noted and read; when surely alone, the watch, then the second
    // look, noted and read. What the line goes by, and how many looks the
    // watch took.
    struct Took {
        PartyHud first, second;
        int      watched = 0;
        bool     watch_ok = false;
    };
    const auto take = [&](ScriptedGame& g, PartyEvents& e) {
        Took r;
        PartyLook look;
        take_party_look(g.sight(), look);
        e.invitation(look.grabbed_ms, up(look.frame));
        r.first = read_party(k.reader, look, e);
        if (!alone_for_chat(r.first)) return r;
        const std::function<bool(const Frame&)> counted = [&](const Frame& f) {
            ++r.watched;
            return up(f);
        };
        r.watch_ok = watch_invitations(
            g.sight(), counted, [&g](std::uint32_t ms) { g.t += ms; }, [] { return false; }, e,
            g.t + PartyEvents::kSettleMs, 250);
        PartyLook again;
        take_party_look(g.sight(), again);
        e.invitation(again.grabbed_ms, up(again.frame));
        r.second = read_party(k.reader, again, e);
        return r;
    };
    const auto game = [&] {
        ScriptedGame g;
        g.alone = k.frame;
        g.member = k.frame;
        draw_member(g.member, 0, 112, Mark::Whole);
        g.invitation = *buttons;
        return g;
    };
    {
        // Nothing happens: both looks surely alone, the line goes.
        ScriptedGame g = game();
        PartyEvents e;
        const Took r = take(g, e);
        check(alone_for_chat(r.first) && r.watch_ok && r.watched >= 10 && alone_for_chat(r.second),
              fmt("nothing on the game: surely alone on both looks, %d looks between them -- the line goes",
                  r.watched));
    }
    {
        // B's invitation comes up half a second into the watch, the player
        // accepts it by hand at 1.2 s, and the game draws B's party 2.5 s
        // after that -- not yet at the second look.
        ScriptedGame g = game();
        PartyEvents e;
        g.invite_from = g.t + 600;
        g.answer_at = g.t + 1300;
        const Took r = take(g, e);
        // That second look by itself, nobody having watched: surely alone.
        PartyLook blind;
        blind.frame = g.shows();
        blind.trusted = blind.whole = true;
        blind.grabbed_ms = g.t;
        const PartyEvents none;
        const bool unwatched = read_party(k.reader, blind, none).sure_alone;
        check(alone_for_chat(r.first) && !alone_for_chat(r.second) && r.second.state == PartyState::Alone &&
                  starts_with(r.second.alone_doubt_vi, party_text::kDoubtJoined) &&
                  r.second.alone_doubt_vi.find(party_text::kEventInvitationGone) != std::string::npos && unwatched,
              "an invitation up and accepted by hand between the two looks, the party not drawn by the second: the "
              "line waits -- \"" + r.second.alone_doubt_vi + "\" (that look unwatched: surely alone)");
    }
    {
        // Accepted by hand just before the first look -- which shows nothing
        // of it -- and drawn 1.5 s later: the second look shows the party.
        ScriptedGame g = game();
        PartyEvents e;
        g.invite_from = g.t - 4000;
        g.answer_at = g.t - 200;
        g.draw_ms = 1500;
        const Took r = take(g, e);
        check(alone_for_chat(r.first) && r.second.state == PartyState::Member && !alone_for_chat(r.second),
              "an invitation accepted by hand a moment before the first look, the party drawn 1.5 s on: the first "
              "look surely alone, the second a member -- the line waits -- " + said(r.second));
    }
    {
        // Still up at the second look: a dialog on it.
        ScriptedGame g = game();
        PartyEvents e;
        g.invite_from = g.t + 2000;
        const Took r = take(g, e);
        check(alone_for_chat(r.first) && !alone_for_chat(r.second) && r.second.state == PartyState::Alone,
              "an invitation come up between the looks and still up: the line waits -- " + said(r.second));
    }
    {
        // Cut short, or a grab that fails: no second look is to be trusted.
        ScriptedGame g = game();
        PartyEvents e;
        const bool stopped = watch_invitations(
            g.sight(), up, [&g](std::uint32_t ms) { g.t += ms; }, [] { return true; }, e, g.t + 3000, 250);
        PartySight broken = g.sight();
        broken.grab = [](Frame&, bool&) { return false; };
        const bool failed = watch_invitations(
            broken, up, [&g](std::uint32_t ms) { g.t += ms; }, [] { return false; }, e, g.t + 3000, 250);
        check(!stopped && !failed, "the watch cut short, or the game not grabbed: false, and no second look");
    }
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

} // namespace

void test_party_hud(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_panel_picture();
    test_crown_pictures();
    test_reading();
    test_crowns();
    test_nonsense();
    test_leader_unread();
    test_bag_over();
    test_party_under_window();
    test_sure_alone();
    test_tooltip_over_x();
    test_dialogs_over_panels();
    test_windows_without_x();
    test_own_crown_under_slot();
    test_trust();
    test_gate();
    test_worker_step();
    test_read_party();
    test_look_at_grab();
    test_invite_mid_pass();
    test_party_drawn_late();
    test_plain_alone_invites();
    test_chat_alone_rule();
    test_chat_watch();
    test_real_frames();
}
