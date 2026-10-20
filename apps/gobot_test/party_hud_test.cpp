// gobot_test -- the party HUD read off the game's screen, and whether an
// invite can work by it.
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
#include "gobot/auto_login.hpp"
#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
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
    int looked = 0, alone = 0, sure = 0, by_window = 0, by_tooltip = 0, by_plain = 0, unknown = 0, party = 0,
        crowned = 0;
    std::string first_party;
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
            by_tooltip += h.alone_doubt_vi == party_text::kDoubtTooltip ? 1 : 0;
            by_plain += h.alone_doubt_vi == party_text::kDoubtPlain ? 1 : 0;
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
    check(sure + by_window + by_tooltip + by_plain == alone,
          fmt("of the %d alone: %d surely; not surely, %d with a window open, %d with a tooltip over the panels, "
              "%d with something plain over the first member's place",
              alone, sure, by_window, by_tooltip, by_plain));
}

void test_gate() {
    section("Party HUD: an invite only when it can work (PH-A5), with the tick by the crown, or surely alone");
    PartyHud alone, sure_alone, window_alone, hidden_alone, member, leader, in_party, unknown, full_lead, full_member,
        full_in_party;
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
    const Row rows[] = {
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
    check(party_hud_line_vi(unknown) == "Tổ đội: không đọc được" && party_hud_line_vi(alone) == "Tổ đội: một mình",
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
            const bool edge = PartyHudReader::tooltip_edge(f, PartyHudReader::kTooltipArea, &at);
            check(h.state == PartyState::Alone && !h.sure_alone && h.alone_doubt_vi == party_text::kDoubtTooltip &&
                      edge,
                  fmt("the live tooltip at %d,66: alone, not surely -- its edge at %d,%d -- ", x, at.x, at.y) +
                      said(h));
        }
        // And nowhere near the panels: no doubt from it.
        Frame f = k.frame;
        blit(f, tip, 700, 300);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Alone && h.sure_alone, "the same tooltip at 700,300, nowhere near the panels: "
                                                            "surely alone -- " + said(h));
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
    test_own_crown_under_slot();
    test_trust();
    test_gate();
    test_worker_step();
    test_real_frames();
}
