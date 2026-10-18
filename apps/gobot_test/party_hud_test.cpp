// gobot_test -- the party HUD read off the game's screen, and whether an
// invite can work by it.
//
// No saved frame of the live client has a party on it, so the parties here
// are drawn: GoC's own pictures of the game's member panel and of its two
// leader frames, onto frame.png from the live client, with the bars, a name
// and a face in the panels as a live party would have them -- and the other
// helper's one live picture of a leader's panel. The saved frames of the live
// rounds are what must never read as a party. Nothing is pressed anywhere.

#include "gobot/party_hud.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

const char* const kLiveBuild = "C:\\Users\\player\\source\\GoBot\\build\\";

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

struct Kit {
    TemplateStore pictures;
    PartyHudReader reader;
    Frame panel, other, mine, live_leader;
    Frame frame;                ///< frame.png, 1280x720, from the live client
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
        k.panel = *k.pictures.get("inven/1/Dialog_PartyInfo.spr/0");
        k.other = *k.pictures.get("inven/1/Party_King_Other.spr/0");
        k.mine = *k.pictures.get("inven/1/Party_King_My.spr/0");
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

/// A member's panel as a live party shows it: the game's panel, its HP and MP
/// bars part full, a name in white, a face -- and the leader's red frame over
/// it when `leads`.
void draw_member(Frame& f, int x, int y, bool leads, int hp = 60, int mp = 80) {
    const Kit& k = kit();
    blit(f, k.panel, x, y);
    fill(f, x + 7, y + 27, hp * 80 / 100, 3, 220, 40, 40);
    fill(f, x + 7, y + 33, mp * 80 / 100, 3, 40, 120, 220);
    for (int i = 0; i < 9; ++i) fill(f, x + 14 + i * 7, y + 10, 4, 8, 250, 250, 250);   // a name
    fill(f, x + 93, y + 8, 28, 26, 200, 150, 90);                                     // a face
    if (leads) blit(f, k.other, x, y);
}

std::string said(const PartyHud& h) { return party_hud_text(h); }

void test_reading() {
    section("Party HUD: alone, a member, the leader, full (PH-A1, PH-A2)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        const PartyHud h = k.reader.read(k.frame);
        check(h.state == PartyState::Alone && h.size == 1 && !h.full && h.panels.empty(),
              "frame.png as it is: alone -- " + said(h));
    }
    {
        // A member of two: the other member's panel down the left, wearing
        // the leader's frame.
        Frame f = k.frame;
        draw_member(f, 0, 112, true);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Member && h.size == 2 && !h.full && h.panels.size() == 1 &&
                  h.panels[0].leader && std::abs(h.panels[0].at.x) <= 1 && std::abs(h.panels[0].at.y - 112) <= 1,
              "another member's panel with the leader's frame: member 2/6 -- " + said(h));
        check(party_hud_line_vi(h) == "Tổ đội: thành viên (2/6)", "said as " + party_hud_line_vi(h));
    }
    {
        // Leading three: the player's own panel in red, two members.
        Frame f = k.frame;
        blit(f, k.mine, 0, 0);
        draw_member(f, 0, 112, false);
        draw_member(f, 0, 160, false, 30, 10);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Leader && h.own_leader && h.size == 3 && !h.full,
              "the own panel in red and two members: leader 3/6 -- " + said(h));
        check(party_hud_line_vi(h) == "Tổ đội: chủ tổ đội (3/6)", "said as " + party_hud_line_vi(h));
    }
    {
        Frame f = k.frame;
        blit(f, k.mine, 0, 0);
        for (int i = 0; i < 5; ++i) draw_member(f, 0, 112 + i * 46, false, 20 * i, 100 - 15 * i);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Leader && h.size == 6 && h.full,
              "the own panel in red and five members: leader 6/6, full -- " + said(h));
    }
    {
        // A full party the player does not lead.
        Frame f = k.frame;
        for (int i = 0; i < 5; ++i) draw_member(f, 0, 112 + i * 46, i == 2);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Member && h.size == 6 && h.full,
              "five members, one leading: member 6/6, full -- " + said(h));
    }
    if (k.live_leader.valid()) {
        // The other helper's own picture of a leader's panel, cut off the
        // live client a pixel above the panel: found, and found leading.
        Frame f = k.frame;
        blit(f, k.live_leader, 0, 120);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Member && h.panels.size() == 1 && h.panels[0].leader,
              "the helper's live picture of a leader's panel: a member, led by that panel -- " + said(h));
    } else {
        std::printf("  [skip] no grabbed/party_leader_key2 here\n");
    }
    {
        // At 1024x768, where the HUD is the same distance from the top-left.
        Frame shot;
        if (load_png(std::string(kLiveBuild) + "bin\\Release\\playerlist.png", shot) && shot.width == 1024) {
            check(k.reader.read(shot).state == PartyState::Alone, "playerlist.png (1024x768) as it is: alone");
            draw_member(shot, 0, 112, false);
            draw_member(shot, 0, 158, true);
            const PartyHud h = k.reader.read(shot);
            check(h.state == PartyState::Member && h.size == 3,
                  "and two members drawn on it: member 3/6 -- " + said(h));
        } else {
            std::printf("  [skip] no playerlist.png from the live client here\n");
        }
    }
}

void test_nonsense() {
    section("Party HUD: what no party looks like is not read as one");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        Frame f = k.frame;
        for (int i = 0; i < 6; ++i) draw_member(f, 0, 104 + i * 44, false);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Unknown && !h.full && h.size == 0,
              "six other panels, seven in all, more than a party holds: unknown, never full -- " + said(h));
    }
    {
        Frame f = k.frame;
        blit(f, k.mine, 0, 0);
        draw_member(f, 0, 112, true);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Unknown, "two leaders' frames: unknown -- " + said(h));
    }
    {
        // The HUD gone: a member's panel on its own says nothing.
        Frame f = plain_frame();
        draw_member(f, 0, 112, true);
        const PartyHud h = k.reader.read(f);
        check(h.state == PartyState::Unknown && h.why.find("HUD") != std::string::npos,
              "no HUD on screen: unknown -- " + said(h));
    }
    {
        // A panel half covered by a window is not counted, so a covered party
        // never reads as fuller than it is.
        Frame f = k.frame;
        blit(f, k.mine, 0, 0);
        for (int i = 0; i < 5; ++i) draw_member(f, 0, 112 + i * 46, false);
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

void test_real_frames() {
    section("Party HUD: the saved live frames, none with a party (PH-A3, PH-A4)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        // r2_B_000 was read off the screen with a terminal over most of the
        // game: its Level plate still shows, and the left of the screen, where
        // panels would be, is the terminal. No look at pixels can tell that
        // from a player alone; the capture can -- it came off the screen with
        // the game not in front -- and such a frame is never trusted.
        Frame covered;
        if (load_png(std::string(kLiveBuild) + "live\\shots\\r2_B_000.png", covered)) {
            const PartyHud h = k.reader.read(covered, party_frame_trusted(true, nullptr));
            check(h.state == PartyState::Unknown, "r2_B_000.png, the game covered: unknown -- " + said(h));
        } else {
            std::printf("  [skip] no live\\shots\\r2_B_000.png here\n");
        }
        check(party_frame_trusted(false, nullptr) && !party_frame_trusted(true, nullptr),
              "a frame the game drew itself is trusted; one off the screen of a game not in front is not");
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
    int looked = 0, alone = 0, unknown = 0, party = 0;
    std::string first_party;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!every && i % 8 != 0 && i + 2 < frames.size()) continue;
        Frame f;
        if (!load_png(frames[i], f) || f.width < 640 || f.height < 480) continue;
        ++looked;
        const PartyHud h = k.reader.read(f);
        if (h.state == PartyState::Alone) ++alone;
        else if (h.state == PartyState::Unknown) ++unknown;
        else {
            ++party;
            if (first_party.empty()) first_party = frames[i] + ": " + said(h);
        }
    }
    if (looked == 0) {
        std::printf("  [skip] no saved frames of the live client here\n");
        return;
    }
    check(party == 0, std::to_string(looked) + " saved frames: " + std::to_string(alone) + " alone, " +
                          std::to_string(unknown) + " unknown, " + std::to_string(party) +
                          " in a party: never a member, leader or full" +
                          (first_party.empty() ? std::string() : " -- first: " + first_party));
}

void test_gate() {
    section("Party HUD: an invite only when it can work (PH-A5)");
    PartyHud alone, member, leader, full_lead, full_member, unknown;
    alone.state = PartyState::Alone;
    alone.size = 1;
    member.state = PartyState::Member;
    member.size = 3;
    leader.state = PartyState::Leader;
    leader.size = 3;
    leader.own_leader = true;
    full_lead = leader;
    full_lead.size = 6;
    full_lead.full = true;
    full_member = member;
    full_member.size = 6;
    full_member.full = true;
    for (const bool tick : { false, true }) {
        const std::string with = tick ? " (ticked)" : " (not ticked)";
        check(party_invite_gate(alone, tick).allowed, "alone: yes" + with);
        check(party_invite_gate(leader, tick).allowed, "leading, not full: yes" + with);
        const PartyGate fl = party_invite_gate(full_lead, tick), fm = party_invite_gate(full_member, tick);
        check(!fl.allowed && fl.reason_vi == "Tổ đội đã đủ 6 người" && !fm.allowed &&
                  fm.reason_vi == "Tổ đội đã đủ 6 người",
              "full, leading or not: no, \"" + fl.reason_vi + "\"" + with);
        const PartyGate u = party_invite_gate(unknown, tick);
        check(u.allowed && u.reason_vi.empty(), "not read: yes, as before the HUD was read" + with);
    }
    const PartyGate m = party_invite_gate(member, true);
    check(!m.allowed && m.reason_vi == "Đang trong tổ đội nhưng không phải chủ tổ đội",
          "a member, ticked: no, \"" + m.reason_vi + "\"");
    check(party_invite_gate(member, false).allowed, "a member, not ticked: yes, as the plan says");
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
    const auto step = [&](const Frame& f, bool always_leader) {
        const PartyGate g = party_invite_gate(k.reader.read(f), always_leader);
        if (!g.allowed) return g.reason_en;
        ++opened;           // the player list's ensure_open
        ++invited;          // its invite
        return std::string();
    };
    Frame full = k.frame;
    blit(full, k.mine, 0, 0);
    for (int i = 0; i < 5; ++i) draw_member(full, 0, 112 + i * 46, false);
    const std::string why = step(full, false);
    check(opened == 0 && invited == 0 && !why.empty(),
          "a full party: the list never opened, nobody invited -- " + why);
    Frame member = k.frame;
    draw_member(member, 0, 112, true);
    step(member, true);
    check(opened == 0 && invited == 0, "a member with the tick on: still nothing");
    step(member, false);
    check(opened == 1 && invited == 1, "a member with the tick off: the list opened and the invite sent, as today");
    step(k.frame, true);
    check(opened == 2 && invited == 2, "alone: invited, as today");
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
    test_reading();
    test_nonsense();
    test_gate();
    test_worker_step();
    test_real_frames();
}
