#include "gobot/party_hud.hpp"
#include "gobot/auto_login.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <unordered_map>

#include <windows.h>

namespace gobot {

namespace {

/// The game's pictures, by the names the template store gives them: skin 1,
/// which this client draws, then skin 0, whose copies are the same pixels.
const char* const kPanelNames[] = { "inven/1/Dialog_PartyInfo.spr/0", "inven/0/Dialog_PartyInfo.spr/0",
                                    "dialog_party_info" };
const char* const kOtherNames[] = { "inven/1/Party_King_Other.spr/0", "inven/0/Party_King_Other.spr/0" };
const char* const kMineNames[]  = { "inven/1/Party_King_My.spr/0", "inven/0/Party_King_My.spr/0" };
const char* const kPlateNames[] = { "hpmp_indicator2", "grabbed/hpmp_indicator2" };
/// The bag's tab strip, by the names bag::find looks it up by.
const char* const kStripNames[] = { "grabbed/inventory", "inventory" };

/// The sizes those pictures are; anything else is some other picture.
constexpr int kPanelW = 130, kPanelH = 42;
constexpr int kOwnW = 290, kOwnH = 44;

/// The parts of a member panel that change, in the panel's own pixels: the
/// two bars, the name box's inside and the portrait box's inside. Measured on
/// GoC's picture; the live helper's picture of a panel differs from it in the
/// bars alone.
constexpr ScreenRect kBars{ 6, 26, 83, 11 };
constexpr ScreenRect kName{ 8, 7, 78, 14 };
constexpr ScreenRect kFace{ 90, 5, 34, 32 };

/// How far from where it is expected a leader's frame may be found: the
/// player's own panel sits at the screen's top-left corner on both client
/// sizes, a member's red frame over that member's panel.
constexpr int kSlack = 3;

const Frame* first_of(const TemplateStore& store, const char* const* names, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (const Frame* f = store.get(names[i])) return f;
    return nullptr;
}

std::uint8_t& alpha(Frame& f, int x, int y) {
    return f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4 + 3];
}
std::uint8_t alpha_of(const Frame& f, int x, int y) {
    return f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4 + 3];
}

ScreenRect clamp_to(const Frame& f, const ScreenRect& r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.right()), y1 = std::min(f.height, r.bottom());
    return ScreenRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

/// How much of `area` of `f` is its one commonest colour, exactly, 0 to 1.
float plain_share(const Frame& f, const ScreenRect& area) {
    std::unordered_map<std::uint32_t, int> seen;
    int all = 0, most = 0;
    for (int y = area.y; y < area.bottom(); ++y)
        for (int x = area.x; x < area.right(); ++x) {
            const std::uint8_t* p = &f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4];
            const std::uint32_t c = (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
            most = std::max(most, ++seen[c]);
            ++all;
        }
    return all > 0 ? static_cast<float>(most) / static_cast<float>(all) : 0.0f;
}

/// The best place for `tpl` within kSlack of `at`, in colour, at `least` or
/// better: the score, or -1.
float frame_at(const Frame& f, const ScreenRect& at, const Frame& tpl, float least) {
    const ScreenRect around = clamp_to(f, ScreenRect{ at.x - kSlack, at.y - kSlack, tpl.width + 2 * kSlack,
                                                      tpl.height + 2 * kSlack });
    if (around.w < tpl.width || around.h < tpl.height) return -1.0f;
    const auto m = TemplateMatcher::find(f, around, tpl, least, TemplateMatcher::Tone::Colour);
    return m.found ? m.score : -1.0f;
}

} // namespace

const char* party_state_name(PartyState s) {
    switch (s) {
        case PartyState::Alone:   return "alone";
        case PartyState::Member:  return "member";
        case PartyState::Leader:  return "leader";
        case PartyState::InParty: return "in a party";
        case PartyState::Unknown: break;
    }
    return "unknown";
}

bool PartyHudReader::load(const TemplateStore& store, std::string* missing) {
    ready_ = false;
    const Frame* panel = first_of(store, kPanelNames, std::size(kPanelNames));
    const Frame* other = first_of(store, kOtherNames, std::size(kOtherNames));
    const Frame* mine = first_of(store, kMineNames, std::size(kMineNames));
    const Frame* plate = first_of(store, kPlateNames, std::size(kPlateNames));
    const Frame* strip = first_of(store, kStripNames, std::size(kStripNames));
    const auto lacks = [&](const char* what) {
        if (missing) *missing = what;
        return false;
    };
    if (panel == nullptr || panel->width != kPanelW || panel->height != kPanelH)
        return lacks("inven/1/Dialog_PartyInfo.spr/0");
    if (other == nullptr || other->width != kPanelW || other->height != kPanelH)
        return lacks("inven/1/Party_King_Other.spr/0");
    if (mine == nullptr || mine->width != kOwnW || mine->height != kOwnH)
        return lacks("inven/1/Party_King_My.spr/0");
    if (plate == nullptr || !plate->valid()) return lacks("hpmp_indicator2");
    if (strip == nullptr || !strip->valid()) return lacks("grabbed/inventory");

    // The member panel by its frame: what the game draws the same for every
    // member. Not the bars, the name or the face, nor the band the leader's
    // red frame covers, so that a leader's panel is found as a member's is;
    // nor its last columns, which the bag lies over where the player keeps it.
    panel_ = *panel;
    for (int y = 0; y < kPanelH; ++y)
        for (int x = 0; x < kPanelW; ++x) {
            const bool changes = kBars.contains(x, y) || kName.contains(x, y) || kFace.contains(x, y);
            const bool under_frame = alpha_of(*other, x, y) >= TemplateMatcher::kOpaque;
            const bool shown = alpha(panel_, x, y) >= TemplateMatcher::kOpaque && x < kPanelMatchW;
            alpha(panel_, x, y) = shown && !changes && !under_frame ? 255 : 0;
        }
    // The red frame over a member's panel the same way: those columns of it
    // under the bag would take a leader's frame for none.
    other_ = *other;
    for (int y = 0; y < kPanelH; ++y)
        for (int x = kPanelMatchW; x < kPanelW; ++x) alpha(other_, x, y) = 0;
    mine_ = *mine;
    landmark_ = *plate;
    strip_ = *strip;
    ready_ = true;
    return true;
}

bool PartyHudReader::load_pictures(TemplateStore& into, const std::string& templates_ini, std::string* missing) {
    const char* const wanted[] = { kPanelNames[0], kOtherNames[0], kMineNames[0], kPlateNames[0], kStripNames[0] };
    for (const char* name : wanted) {
        Frame f;
        if (!TemplateStore::load_one(name, templates_ini, f)) {
            if (missing) *missing = name;
            return false;
        }
        into.put(name, f);
    }
    return true;
}

bool party_frame_trusted(bool from_screen, void* window) {
    if (!from_screen) return true;
    // Only the part of the game the party is read from has to be in view:
    // with GoBot's own window beside the game, or in front of it but over
    // another part, the panels down the left are the game's own pixels.
    return window != nullptr && AutoLogin::part_seen(window, PartyHudReader::kArea);
}

bool PartyHudReader::bag_window(const Frame& f, ScreenRect& out) const {
    if (!ready_ || !f.valid() || !strip_.valid()) return false;
    // Only where a bag could reach over the panels: its strip no further
    // right than kBagLeft past the columns they are matched by.
    const ScreenRect where =
        clamp_to(f, ScreenRect{ 0, 0, kPanelMatchW + kBagLeft + strip_.width, f.height });
    if (where.w < strip_.width || where.h < strip_.height) return false;
    const auto m = TemplateMatcher::find(f, where, strip_, kBagStripScore);
    if (!m.found) return false;
    out = ScreenRect{ m.at.x - kBagLeft, m.at.y - kBagUp, kBagW, kBagH };
    return true;
}

PartyHud PartyHudReader::read(const Frame& f, bool trusted) const {
    PartyHud hud;
    if (!ready_) {
        hud.why = "the party pictures are not loaded";
        return hud;
    }
    if (!f.valid()) {
        hud.why = "no picture of the game";
        return hud;
    }
    if (!trusted) {
        hud.why = "read off the screen with the game behind or under another window";
        return hud;
    }
    // The party is only on screen with the HUD: the player's own panel, its
    // Level plate where it always sits. Without it -- another window over the
    // game, a loading screen, the login -- nothing here says anything.
    if (!AutoLogin::plate_at(f, &landmark_)) {
        hud.why = "the HUD is not on screen";
        return hud;
    }
    // A frame read off the screen with another program's window over the
    // left of the game, its plate still showing above it: that window's plain
    // ground where the panels would be, and nobody seen there is no proof of
    // nobody there.
    const ScreenRect seen_column =
        clamp_to(f, ScreenRect{ 0, kSearchTop, kPanelMatchW, kSearchBottom - kSearchTop });
    if (plain_share(f, seen_column) >= kPlainShare) {
        hud.why = "the left of the screen is mostly one plain colour: another window over the game";
        return hud;
    }

    // The player's own panel, at the top-left corner: the red frame over it
    // says the player leads.
    hud.own_score = frame_at(f, ScreenRect{ 0, 0, kOwnW, kOwnH }, mine_, -1.0f);
    hud.own_leader = hud.own_score >= kLeaderScore;

    // The other members' panels, down the left of the screen.
    const ScreenRect area = clamp_to(f, ScreenRect{ 0, kSearchTop, kSearchRight, kSearchBottom - kSearchTop });
    if (area.w >= panel_.width && area.h >= panel_.height) {
        for (const auto& m : TemplateMatcher::find_all(f, area, panel_, kPanelScore, kPartyMax + 2)) {
            PartyPanel p;
            p.at = m.at;
            p.score = m.score;
            p.leader_score = frame_at(f, m.at, other_, -1.0f);
            p.leader = p.leader_score >= kLeaderScore;
            hud.panels.push_back(p);
        }
    }
    std::sort(hud.panels.begin(), hud.panels.end(),
              [](const PartyPanel& a, const PartyPanel& b) { return a.at.y < b.at.y; });

    int leaders = hud.own_leader ? 1 : 0;
    for (const PartyPanel& p : hud.panels) leaders += p.leader ? 1 : 0;
    // What no party looks like: more panels than one holds, or two leaders.
    // Something else on the screen is being taken for panels, and a reading
    // built on that would say "full" of a party that is not.
    if (static_cast<int>(hud.panels.size()) + 1 > kPartyMax) {
        hud.why = std::to_string(hud.panels.size()) + " panels, more than a party holds";
        hud.panels.clear();
        hud.own_leader = false;
        return hud;
    }
    if (leaders > 1) {
        hud.why = "two leaders' frames";
        hud.panels.clear();
        hud.own_leader = false;
        return hud;
    }

    // The bag over the panels' column hides whoever is under it: the party is
    // then at least what is seen, and nobody seen is no proof of nobody there.
    ScreenRect bag;
    const ScreenRect column{ 0, kSearchTop, kPanelMatchW, kSearchBottom - kSearchTop };
    hud.covered = bag_window(f, bag) && bag.overlaps(column);
    if (hud.covered && hud.panels.empty() && !hud.own_leader) {
        hud.why = "the bag is open over where the party's panels are drawn";
        return hud;
    }
    hud.size = static_cast<int>(hud.panels.size()) + 1;
    hud.full = hud.size >= kPartyMax;
    // A party has its one leader. Panels with no leader's frame on any of
    // them, nor on the player's own, are a party whose leader was not seen:
    // the player's own red frame not matched, or the leader's panel under a
    // window -- never proof that somebody else leads.
    const bool led_by_other = leaders == 1 && !hud.own_leader;
    hud.state = hud.own_leader      ? PartyState::Leader
              : hud.panels.empty()  ? PartyState::Alone
              : led_by_other        ? PartyState::Member
                                    : PartyState::InParty;
    return hud;
}

PartyGate party_invite_gate(const PartyHud& hud, bool always_leader) {
    PartyGate g;
    if (hud.state == PartyState::Unknown) return g;     // as before it was read
    if (hud.full) {
        g.allowed = false;
        g.reason_vi = party_text::kFull;
        g.reason_en = "the party is full (6/6)";
        return g;
    }
    if (hud.state == PartyState::Member && always_leader) {
        g.allowed = false;
        g.reason_vi = party_text::kNotLeader;
        g.reason_en = "in a party but not its leader";
    } else if (hud.state == PartyState::InParty && always_leader) {
        // Only a party seen to be led by somebody else is refused for the
        // tick: this may well be the player's own.
        g.note_vi = party_text::kLeaderUnread;
        g.note_en = "who leads the party could not be read";
    }
    return g;
}

std::string party_hud_line_vi(const PartyHud& hud) {
    const std::string of = " (" + std::string(hud.covered ? "ít nhất " : "") + std::to_string(hud.size) + "/" +
                           std::to_string(kPartyMax) + ")";
    switch (hud.state) {
        case PartyState::Alone:   return "Tổ đội: một mình";
        case PartyState::Member:  return "Tổ đội: thành viên" + of;
        case PartyState::Leader:  return "Tổ đội: chủ tổ đội" + of;
        case PartyState::InParty: return "Tổ đội: đang trong tổ đội" + of + ", không rõ ai làm chủ";
        case PartyState::Unknown: break;
    }
    return "Tổ đội: không đọc được";
}

std::string party_hud_text(const PartyHud& hud) {
    std::string s = party_state_name(hud.state);
    if (hud.state == PartyState::Unknown) return s + (hud.why.empty() ? std::string() : " (" + hud.why + ")");
    char buf[96];
    std::snprintf(buf, sizeof buf, " %s%d/%d%s; own frame %.2f", hud.covered ? "at least " : "", hud.size,
                  kPartyMax, hud.full ? ", full" : "", static_cast<double>(hud.own_score));
    s += buf;
    if (hud.covered) s += "; the bag over the panels";
    for (const PartyPanel& p : hud.panels) {
        std::snprintf(buf, sizeof buf, "; panel at %d,%d (%.2f%s)", p.at.x, p.at.y, static_cast<double>(p.score),
                      p.leader ? ", leads" : "");
        s += buf;
    }
    return s;
}

} // namespace gobot
