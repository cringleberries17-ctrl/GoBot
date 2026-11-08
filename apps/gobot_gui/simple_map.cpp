// The "Bản đồ" tab (2026.11.04c): the map the character is on, drawn from the
// game's own files -- its platforms, the bottom ground, its gates -- with the
// character and the monsters where the game's packets put them, as GoC shows
// them -- between their frames, on the floors those put them on (mob_dot,
// you_dot, since 2026.11.05); and on it, with the mouse, that map's hunting
// zone ("vùng đánh" and its "chỗ về"), kept in configs\spots.ini and handed to
// the hunter whenever it hunts there with "Chỗ đánh quái" ticked. Since
// 2026.11.05b also the hunter's jump up as an arrow, what its jumps learned in
// grey, and the seek's way to the monster it goes for, those it cannot get to
// greyed (map_view.hpp's jump_arrow, solid_marks, seek_grey_of).
//
// What is drawn comes from the tab's own worker (MapViewJob, gobot/map_view.hpp):
// which map it is -- a map entry, or MapTrust naming it from standing points --
// its file, its floors as runs, its gates and where a drop is refused for one.
// The worker reads and writes spots.ini too, its saves one batch at a time
// (ZoneSaveQueue): the GUI thread reads no file. A zone is never put on another
// map's pixels: what the worker knows is gone by only while its map entry and
// the log ins counted here are still the live ones (usable_identity), and the
// hunter takes a zone only for the map entry it was sent for (Hunter::set_zone).
//
// Mouse only -- while a run is on, keys typed here would go to the game
// (FrontKeeper): a drag pans, the wheel and [-] [+] zoom, the zone's home, its
// edges, corners and the strip along its top edit it, a right click opens the
// menu. An edit is saved once, on the release, never a frame at a time, and
// never into hunt.ini. The rules all this goes by are pure and tested
// (map_view.hpp, hunt_zone.hpp, map_sketch.hpp); here is the drawing and the
// asking.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_sketch.hpp"
#include "gobot/map_table.hpp"
#include "gobot/map_view.hpp"
#include "gobot/packet_eyes.hpp"

#include "imgui.h"
#include "imgui_internal.h"     // IsMouseDragPastThreshold: a click told from a drag, either button

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gobot {
namespace {

using namespace simple_ui;

const char* const kMenu = "##mapmenu";
const char* const kDeleteTitle = "Xoá vùng?";
const char* const kHomeRefused = "Chỗ về phải nằm trên một bục, trong vùng.";
const char* const kNoFloorNear = "Không có bục nào gần đó để làm chỗ về.";
const char* const kNotSureHere = "Chưa chắc nhân vật đang ở bản đồ này — đi vài bước trên các bục rồi bấm lại.";

/// Much of the box red (ZoneCheck::red_bad), as the card says it in red.
std::string red_bad_vi(const ZoneCheck& c) {
    const int pct = c.floor_cols > 0 ? c.red_cols * 100 / c.floor_cols : 0;
    return "Phần lớn bục trong vùng (" + std::to_string(pct) +
           "%) bot không tự đi về được (tô đỏ): rời bục chỗ về là bot ở lại dưới đó. Kéo ghim cam xuống bục thấp nhất "
           "có quái, rồi kéo vùng lên trên.";
}

/// The red note at a commit that leaves much of the box red, at every edit:
/// a new zone round the character keeps its home where it stands (the
/// owner's Q5, kZoneAroundHomeLowest false), and says so. "" otherwise.
std::string red_note_vi(const ZoneCheck& c, bool made_new) {
    if (!c.red_bad) return {};
    return (made_new && !kZoneAroundHomeLowest ? "Vùng mới đặt chỗ về ở chỗ nhân vật đứng. " : "") + red_bad_vi(c);
}

/// The worker's job runs this often while the tab is shown or the hunter goes
/// by a zone -- at once after a map entry -- and, once the tab has been made,
/// this often otherwise: within MapViewJob::kGapMs, so a map named from
/// standing points is not let go of while nobody looks.
constexpr std::uint64_t kJobEveryMs = 400;
constexpr std::uint64_t kKeepAliveMs = 1500;
static_assert(kKeepAliveMs < MapViewJob::kGapMs, "the keep-alive must come before the job's gap rule");
/// The tab counts as shown this long after its last frame.
constexpr std::uint64_t kShownMs = 1000;
/// The hand-off looks at the hunter's status at most this often -- at once
/// after the worker or an edit changed something.
constexpr std::uint64_t kZoneTickMs = 100;
/// A save that failed is tried again after this.
constexpr std::uint64_t kSaveRetryMs = 5000;
/// A drop's reason is said while it is this fresh (as simple_drop_hints).
constexpr std::uint64_t kDropFreshMs = 1500;
constexpr std::uint32_t kNoteMs = 6000;
/// The canvas's height: what is left above BẮT ĐẦU, within these.
constexpr float kCanvasMinH = 300.0f;
constexpr float kCanvasMaxH = 460.0f;
/// A monster, a gate or a no-drop piece is hovered within this, on the screen.
constexpr float kHoverPx = 8.0f;
/// With no map file: a grid this fine, round you and the monsters with this
/// much to spare, never smaller than kGridMinW x kGridMinH.
constexpr double kGridPx = 100.0;
constexpr double kGridPad = 200.0;
constexpr double kGridMinW = 1600.0;
constexpr double kGridMinH = 900.0;

/// Every colour of the canvas, in one place. It keeps its own dark ground
/// whatever the window's theme. GoC's, where it has one.
struct MapColors {
    static constexpr ImU32 kGround    = IM_COL32(20, 20, 20, 255);
    static constexpr ImU32 kGrid      = IM_COL32(40, 40, 46, 255);
    static constexpr ImU32 kPlatform  = IM_COL32(70, 220, 90, 255);
    static constexpr ImU32 kBottom    = IM_COL32(80, 100, 255, 255);
    static constexpr ImU32 kWall      = IM_COL32(0, 220, 220, 255);
    static constexpr ImU32 kBlock     = IM_COL32(220, 0, 220, 255);
    static constexpr ImU32 kOther     = IM_COL32(110, 110, 110, 255);
    static constexpr ImU32 kNoDrop    = IM_COL32(255, 80, 80, 70);
    static constexpr ImU32 kRed       = IM_COL32(235, 90, 90, 180);
    static constexpr ImU32 kAmber     = IM_COL32(240, 180, 60, 140);
    static constexpr ImU32 kAmberLine = IM_COL32(240, 180, 60, 230);
    static constexpr ImU32 kGateMark  = IM_COL32(255, 180, 180, 255);
    static constexpr ImU32 kGateFill  = IM_COL32(220, 40, 40, 80);
    static constexpr ImU32 kGateLine  = IM_COL32(255, 80, 80, 220);
    static constexpr ImU32 kZoneFill  = IM_COL32(0, 100, 200, 50);
    static constexpr ImU32 kZoneLine  = IM_COL32(255, 255, 255, 200);
    static constexpr ImU32 kZoneHot   = IM_COL32(255, 255, 0, 255);
    static constexpr ImU32 kHotFill   = IM_COL32(0, 150, 255, 80);
    static constexpr ImU32 kStrip     = IM_COL32(255, 255, 255, 40);
    static constexpr ImU32 kSpotFill  = IM_COL32(0, 100, 200, 25);
    static constexpr ImU32 kSpotLine  = IM_COL32(255, 255, 255, 140);
    static constexpr ImU32 kHome      = IM_COL32(255, 190, 40, 255);
    static constexpr ImU32 kGrey      = IM_COL32(140, 140, 140, 150);
    static constexpr ImU32 kMob       = IM_COL32(255, 60, 60, 255);
    static constexpr ImU32 kMobRing   = IM_COL32(255, 255, 255, 160);
    static constexpr ImU32 kTarget    = IM_COL32(255, 255, 0, 255);
    static constexpr ImU32 kHpBack    = IM_COL32(70, 20, 20, 220);
    static constexpr ImU32 kHp        = IM_COL32(255, 70, 70, 255);
    static constexpr ImU32 kYou       = IM_COL32(0, 100, 255, 255);
    static constexpr ImU32 kWhite     = IM_COL32(255, 255, 255, 255);
    static constexpr ImU32 kLost      = IM_COL32(150, 150, 150, 255);
    static constexpr ImU32 kExtra     = IM_COL32(170, 190, 255, 170);
    static constexpr ImU32 kLabel     = IM_COL32(235, 235, 235, 255);
    static constexpr ImU32 kInk       = IM_COL32(15, 15, 15, 255);
    static constexpr ImU32 kJump      = IM_COL32(120, 230, 255, 240);
    static constexpr ImU32 kLearned   = IM_COL32(150, 150, 150, 200);
};

/// `c` with its alpha times `f`: a doubtful file's floors, a learned gate, a
/// monster on the move.
ImU32 alpha_of(ImU32 c, float f) {
    const ImU32 a = (c >> IM_COL32_A_SHIFT) & 0xFFu;
    const ImU32 na = static_cast<ImU32>(std::lround(static_cast<float>(a) * std::clamp(f, 0.0f, 1.0f)));
    return (c & ~IM_COL32_A_MASK) | (na << IM_COL32_A_SHIFT);
}

/// World pixels as whole ones.
std::string px(double v) { return std::to_string(std::lround(v)); }
int round_px(double v) { return static_cast<int>(std::lround(v)); }

/// Where a dot is drawn, "1100, 714" -- and where the packets put it when
/// that is elsewhere: "1100, 714 (gói tin 1100, 650)".
std::string place_vi(const MapDot& d, double x, double y) {
    std::string s = px(d.x) + ", " + px(d.y);
    if (round_px(d.x) != round_px(x) || round_px(d.y) != round_px(y))
        s += " (gói tin " + px(x) + ", " + px(y) + ")";
    return s;
}

/// 1234567 as "1.234.567", as the game writes it.
std::string thousands(std::uint32_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ".");
    return s;
}

/// The local time to the minute, as spots.ini keeps it: "2026-11-04 21:36".
std::string local_minute() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return {};
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min);
    return buf;
}

/// "ngang 1521–2121 (600 px) · dọc 200–520 (320 px) · chỗ về 1821, 359".
std::string zone_vi(const HuntZone& z) {
    return "ngang " + std::to_string(z.left) + "–" + std::to_string(z.right) + " (" + std::to_string(z.width()) +
           " px) · dọc " + std::to_string(z.top) + "–" + std::to_string(z.bottom) + " (" +
           std::to_string(z.height()) + " px) · chỗ về " + std::to_string(z.home_x) + ", " +
           std::to_string(z.home_y);
}

/// Why a zone may not be edited now, for the tooltips.
const char* edit_refusal_vi(MapEdit e) {
    switch (e) {
        case MapEdit::Ok:          return "";
        case MapEdit::BookBad:     return "Không đọc được configs\\spots.ini";
        case MapEdit::BookLoading: return "Đang đọc vùng đã lưu…";
        case MapEdit::Changing:    return "Đang đổi bản đồ…";
        case MapEdit::MapUnknown:  return "Chờ GoBot nhận ra bản đồ";
        case MapEdit::NoGame:      return "Cần file bản đồ của game để đặt vùng.";
    }
    return "";
}

/// The drop's reason as a short label over the character.
std::string drop_label(const Hunter::Status& hs, std::uint64_t now) {
    if (hs.drop_wait_ms == 0 || now < hs.drop_wait_ms || now - hs.drop_wait_ms > kDropFreshMs) return {};
    using Why = Hunter::DropWhy;
    switch (hs.drop_wait) {
        case Why::Gate:        return "không nhảy: cửa " + std::to_string(hs.drop_wait_px) + " px";
        case Why::PastMonster: return "không nhảy: rơi quá bậc của quái";
        case Why::Bottom:      return "không nhảy: nền dưới cùng";
        case Why::NoDropHere:  return "không nhảy: chỗ này không rơi";
        case Why::OutsideArea: return "không nhảy: ra khỏi vùng";
        case Why::MapUnknown:  return "không nhảy: chưa rõ bản đồ";
        case Why::Veto:        return "không nhảy: đang chờ";
        default:               return {};
    }
}

ImGuiMouseCursor grip_cursor(ZoneGrip g) {
    switch (g) {
        case ZoneGrip::Home:  return ImGuiMouseCursor_Hand;
        case ZoneGrip::Strip: return ImGuiMouseCursor_ResizeAll;
        case ZoneGrip::L:
        case ZoneGrip::R:     return ImGuiMouseCursor_ResizeEW;
        case ZoneGrip::T:
        case ZoneGrip::B:     return ImGuiMouseCursor_ResizeNS;
        case ZoneGrip::TL:
        case ZoneGrip::BR:    return ImGuiMouseCursor_ResizeNWSE;
        case ZoneGrip::TR:
        case ZoneGrip::BL:    return ImGuiMouseCursor_ResizeNESW;
        case ZoneGrip::None:  break;
    }
    return ImGuiMouseCursor_Arrow;
}

void floor_style(std::uint8_t attr, ImU32& col, float& thick) {
    switch (attr) {
        case 1:  col = MapColors::kPlatform; thick = 2.0f; return;
        case 2:  col = MapColors::kBottom;   thick = 3.0f; return;
        case 4:  col = MapColors::kWall;     thick = 1.0f; return;
        case 5:
        case 6:  col = MapColors::kBlock;    thick = 2.0f; return;
        default: col = MapColors::kOther;    thick = 2.0f; return;
    }
}

/// The world the canvas shows: the map, or with no file a box round you and
/// the monsters.
struct Bounds {
    double x0 = 0.0, y0 = 0.0, x1 = kGridMinW, y1 = kGridMinH;
    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
};

/// World pixels to the canvas's and back, through the camera as it is this
/// frame: MapCamera::to_screen's sums, kept for the thousands of points.
struct ToScreen {
    double ox = 0.0, oy = 0.0, cx = 0.0, cy = 0.0, z = 1.0;
    double vx0 = 0.0, vy0 = 0.0, vx1 = 0.0, vy1 = 0.0;     ///< the world in view

    ToScreen(const MapCamera& cam, const ImVec2& o, const ImVec2& s)
        : ox(o.x + s.x / 2.0), oy(o.y + s.y / 2.0), cx(cam.cx), cy(cam.cy),
          z(cam.zoom > 0.0 ? cam.zoom : MapCamera::kFollowZoom) {
        vx0 = cx - s.x / 2.0 / z;
        vx1 = cx + s.x / 2.0 / z;
        vy0 = cy - s.y / 2.0 / z;
        vy1 = cy + s.y / 2.0 / z;
    }
    ImVec2 at(double wx, double wy) const {
        return ImVec2(static_cast<float>(ox + (wx - cx) * z), static_cast<float>(oy + (wy - cy) * z));
    }
    /// Whether a world box meets the view, with `pad_px` screen pixels to spare.
    bool sees(double x0, double y0, double x1, double y1, double pad_px) const {
        const double pad = pad_px / z;
        return x1 >= vx0 - pad && x0 <= vx1 + pad && y1 >= vy0 - pad && y0 <= vy1 + pad;
    }
};

MapCamera::P cam_p(const ImVec2& v) { return { v.x, v.y }; }

/// The camera fitted to `b`, the whole of it in view.
void fit_to(MapCamera& cam, const Bounds& b, const ImVec2& size) {
    cam.zoom = MapCamera::fit_zoom(round_px(b.w()), round_px(b.h()), size.x, size.y);
    cam.cx = (b.x0 + b.x1) / 2.0;
    cam.cy = (b.y0 + b.y1) / 2.0;
}

/// The middle kept on `b`: never the world panned out of the canvas.
void clamp_to(MapCamera& cam, const Bounds& b) {
    cam.cx = std::clamp(cam.cx, b.x0, b.x1);
    cam.cy = std::clamp(cam.cy, b.y0, b.y1);
}

/// A run of points, a column a point or simplified, as one polyline; a
/// one-column run as a dot.
void stroke(ImDrawList* dl, const std::vector<MapPoint>& pts, const ToScreen& ts, std::vector<ImVec2>& buf, ImU32 col,
            float thick) {
    if (pts.empty()) return;
    if (pts.size() == 1) {
        const ImVec2 p = ts.at(pts[0].x, pts[0].y);
        dl->AddRectFilled(ImVec2(p.x - 1.0f, p.y - 1.0f), ImVec2(p.x + 1.0f, p.y + 1.0f), col);
        return;
    }
    buf.resize(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) buf[i] = ts.at(pts[i].x, pts[i].y);
    dl->AddPolyline(buf.data(), static_cast<int>(buf.size()), col, thick, ImDrawFlags_None);
}

/// The part of a..b inside the box lo..hi (Liang-Barsky); false for none.
bool clip_line(ImVec2& a, ImVec2& b, const ImVec2& lo, const ImVec2& hi) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    float t0 = 0.0f, t1 = 1.0f;
    const float p[4] = { -dx, dx, -dy, dy };
    const float q[4] = { a.x - lo.x, hi.x - a.x, a.y - lo.y, hi.y - a.y };
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return false;
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0.0f) t0 = std::max(t0, t);
        else             t1 = std::min(t1, t);
        if (t0 > t1) return false;
    }
    const ImVec2 from = a;
    a = ImVec2(from.x + t0 * dx, from.y + t0 * dy);
    b = ImVec2(from.x + t1 * dx, from.y + t1 * dy);
    return true;
}

/// A dashed line, the part on the canvas only: an edge far out of view is
/// never thousands of dashes.
void dashed_line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick, const ImVec2& lo, const ImVec2& hi) {
    if (!clip_line(a, b, ImVec2(lo.x - 4.0f, lo.y - 4.0f), ImVec2(hi.x + 4.0f, hi.y + 4.0f))) return;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.5f) return;
    constexpr float kDash = 6.0f;
    for (float d = 0.0f; d < len; d += kDash * 2.0f) {
        const float e = std::min(d + kDash, len);
        dl->AddLine(ImVec2(a.x + dx * d / len, a.y + dy * d / len), ImVec2(a.x + dx * e / len, a.y + dy * e / len),
                    col, thick);
    }
}

void dashed_ring(ImDrawList* dl, const ImVec2& c, float r, ImU32 col, float thick) {
    constexpr int kArcs = 8;
    constexpr float kTau = 6.2831853f;
    for (int i = 0; i < kArcs; ++i) {
        const float a0 = kTau * static_cast<float>(i) / kArcs;
        dl->PathArcTo(c, r, a0, a0 + kTau / (2.0f * kArcs), 4);
        dl->PathStroke(col, thick, ImDrawFlags_None);
    }
}

/// GoC's pink "+".
void plus_mark(ImDrawList* dl, const ImVec2& c, float r, ImU32 col) {
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, 1.5f);
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, 1.5f);
}

/// The home's pin: a drop shape kZonePinPx tall, its point on the home.
void home_pin(ImDrawList* dl, const ImVec2& home, ImU32 col) {
    const ImVec2 head(home.x, home.y - static_cast<float>(kZonePinPx));
    dl->AddTriangleFilled(ImVec2(head.x - 3.6f, head.y + 2.0f), ImVec2(head.x + 3.6f, head.y + 2.0f), home, col);
    dl->AddCircleFilled(head, 4.5f, col);
    dl->AddCircle(head, 4.5f, MapColors::kInk, 0, 1.0f);
    dl->AddCircleFilled(head, 1.6f, MapColors::kInk);
}

/// An arrow from a to b, its head at b: solid, or dashed -- a jump up that
/// would have gone.
void arrow(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 col, bool dashed, const ImVec2& lo,
           const ImVec2& hi) {
    if (dashed) dashed_line(dl, a, b, col, 2.0f, lo, hi);
    else        dl->AddLine(a, b, col, 2.0f);
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    const float ux = dx / len, uy = dy / len;
    constexpr float kHead = 8.0f, kWing = 4.5f;
    dl->AddTriangleFilled(b, ImVec2(b.x - ux * kHead - uy * kWing, b.y - uy * kHead + ux * kWing),
                          ImVec2(b.x - ux * kHead + uy * kWing, b.y - uy * kHead - ux * kWing), col);
}

/// Why the seek greys a monster, for the hover (Status::SeekGrey).
std::string seek_grey_vi(const Hunter::Status::SeekGrey& g) {
    switch (g.why) {
        case SeekNo::NeedsJumpOff:  return "Không tới được: bục trên, chưa nhảy lên";
        case SeekNo::NoAttackPoint: return "Không tới được: trong vùng không có chỗ đứng nào đánh tới nó";
        case SeekNo::NoFloor:       return "Không đứng trên bục nào (có thể đang bị đánh văng)";
        case SeekNo::Passed:
            return "Tạm bỏ qua, còn " + std::to_string((g.left_ms + 999) / 1000) + " s";
        case SeekNo::Others:        return "Người khác vừa đánh con này";
        case SeekNo::JumpsUsed:     return "Không tới được: bục trên, đã nhảy đủ số lần cho con này";
        case SeekNo::Outside:
        case SeekNo::None:          break;
    }
    return {};
}

/// How far `p` is from the segment a..b, on the screen.
float to_segment(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

/// Text at kSmall, measured and drawn in the screen's face.
ImVec2 small_size(const char* text) {
    ImGui::PushFont(nullptr, kSmall);
    const ImVec2 s = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return s;
}

void small_text(ImDrawList* dl, const ImVec2& at, ImU32 col, const char* text) {
    dl->AddText(nullptr, kSmall, at, col, text);
}

/// With no map file: the box round you and the living monsters drawn --
/// `dots`, v.mobs' (mob_dots): none while nothing places them, never the
/// world's stale last -- kGridPad to spare, kGridMinW x kGridMinH at least,
/// on the grid's lines.
Bounds grid_bounds(const PacketWorld::View& v, const std::vector<MapDot>& dots, bool you, double yx, double yy) {
    bool any = false;
    Bounds b;
    const auto add = [&](double x, double y) {
        if (!any) {
            b.x0 = b.x1 = x;
            b.y0 = b.y1 = y;
            any = true;
            return;
        }
        b.x0 = std::min(b.x0, x);
        b.x1 = std::max(b.x1, x);
        b.y0 = std::min(b.y0, y);
        b.y1 = std::max(b.y1, y);
    };
    if (you) add(yx, yy);
    for (std::size_t i = 0; i < dots.size() && i < v.mobs.size(); ++i)
        if (!v.mobs[i].dead && dots[i].shown()) add(dots[i].x, dots[i].y);
    if (!any) return Bounds{};
    b.x0 -= kGridPad;
    b.x1 += kGridPad;
    b.y0 -= kGridPad;
    b.y1 += kGridPad;
    if (b.w() < kGridMinW) {
        const double mid = (b.x0 + b.x1) / 2.0;
        b.x0 = mid - kGridMinW / 2.0;
        b.x1 = mid + kGridMinW / 2.0;
    }
    if (b.h() < kGridMinH) {
        const double mid = (b.y0 + b.y1) / 2.0;
        b.y0 = mid - kGridMinH / 2.0;
        b.y1 = mid + kGridMinH / 2.0;
    }
    b.x0 = std::floor(b.x0 / kGridPx) * kGridPx;
    b.y0 = std::floor(b.y0 / kGridPx) * kGridPx;
    b.x1 = std::ceil(b.x1 / kGridPx) * kGridPx;
    b.y1 = std::ceil(b.y1 / kGridPx) * kGridPx;
    return b;
}

} // namespace

/// What the tab keeps. The GUI thread's alone but for the slot the worker
/// hands its results over in, under `mx`.
struct GuiApp::MapUi {
    enum class Mode : std::uint8_t { Follow, Fit, Free };

    // ---- the GUI thread's ----
    std::string   path;                     ///< configs\spots.ini, beside GoBot
    MapCamera     cam;
    Mode          mode = Mode::Follow;
    std::string   cam_for;                  ///< the map the camera was placed for
    double        zoom_by = 1.0;            ///< [-] [+] asked since the last frame
    bool          details = false;          ///< "Chi tiết": this session only
    MapViewResult cur;                      ///< the worker's last, as copied
    bool          ran = false;              ///< a job's result has come
    ZoneBook      book;
    bool          book_asked = false;
    bool          book_loaded = false;
    bool          book_bad = false;
    ZoneSaveQueue saves;
    std::uint64_t save_retry_ms = 0;        ///< after a failed save, not before this
    bool          save_failed = false;      ///< the last save failed: said in the card
    /// The zone before the last commit, for Hoàn tác: of that map, while it
    /// is the one shown.
    struct Undo {
        bool                    set = false;
        std::string             stem;
        std::optional<HuntZone> before;
    } undo;
    std::string   shown_stem;               ///< the map shown at the last frame
    /// A drag on a grip: from its press, the camera frozen as it was then.
    std::optional<ZoneGesture> gesture;
    bool          gesture_spot = false;     ///< on this run's spot: its release saves it as the map's zone
    std::string   gesture_stem;
    std::optional<HuntZone> draft;          ///< the zone under the drag, drawn live, saved on release only
    ZoneCheck     check;                    ///< the shown zone's, worked out on a change only
    std::string   check_key;                ///< ... for this zone, file and drop setting
    bool          right_down = false;       ///< a right press began on the canvas
    MapPoint      menu_at;                  ///< where the menu was opened, in the world
    std::string   menu_stem;                ///< ... and the map it was opened on
    bool          ask_delete = false;
    std::string   delete_stem;
    /// The zone handed to the hunter, and the map it was set aside for:
    /// sent again when any of these moves (map_zone_tick).
    struct Sent {
        bool          set = false;
        std::string   stem;
        std::uint32_t epoch = 0;
        std::uint64_t trust_gen = 0;
        std::uint64_t rev = 0;
        bool operator==(const Sent&) const = default;
    } sent;
    std::string   aside_sent;
    bool          was_hunting = false;
    bool          was_spot = false;
    bool          zone_due = true;          ///< look at the hand-off at the next pass
    std::uint64_t zone_ms = 0;
    /// The last settled report of yours and its map entry: what a named map's
    /// zone is held to (zone_send).
    MapPoint      last_settled;
    std::uint32_t last_settled_epoch = 0;
    bool          have_settled = false;
    /// Where the character was last Known: drawn grey while the packets
    /// have lost it.
    double        last_x = 0.0, last_y = 0.0;
    bool          have_last = false;
    std::uint32_t posted_epoch = 0;
    std::uint64_t post_ms = 0;
    std::uint64_t shown_ms = 0;
    std::string   note;                     ///< the tab's last note, as simple_note has it
    bool          note_bad = false;         ///< ... said in red: much of a zone just saved is red
    std::uint64_t note_until = 0;
    std::vector<ImVec2> scratch;            ///< one run's points on the screen, reused
    float         below_h = 260.0f;         ///< the tab under the canvas, at the last frame
    bool          hover = false;            ///< the mouse over the canvas at the last frame, and where
    double        hover_x = 0.0, hover_y = 0.0;

    // ---- handed over by the worker, under mx ----
    std::mutex    mx;
    MapViewResult slot;
    bool          slot_new = false;
    std::optional<ZoneBook> book_in;
    ZoneBook::Load book_load = ZoneBook::Load::Missing;
    struct SaveDone {
        ZoneSaveQueue::Batch batch;
        bool                 ok = false;
    };
    std::optional<SaveDone> save_done;

    // ---- the worker's ----
    std::unique_ptr<MapViewState> ws;
    /// Declared last: joined before anything its jobs fill goes.
    OneWorker worker{ "map-view" };
};

void GuiApp::MapUiFree::operator()(MapUi* ui) const { delete ui; }

/// Why a zone was committed, for the log and the note.
enum class GuiApp::MapCommit : std::uint8_t {
    New,        ///< "Tạo vùng quanh nhân vật", "Tạo vùng ở đây"
    Edit,       ///< a drag, "Đặt chỗ về ở đây"
    FromSpot,   ///< this run's spot kept: its box dragged, or "Lưu vùng này cho bản đồ"
    Here,       ///< "Đặt chỗ này" on Đánh quái (the owner's Q2)
    Moved,      ///< "Dời vùng tới nhân vật", "Dời vùng tới đây"
    Delete,     ///< "Xoá vùng"
    Undo,       ///< "Hoàn tác"
};

/// One frame's look at everything the tab draws and decides by.
struct GuiApp::MapView {
    std::uint64_t  now = 0;
    PacketWorld::View v;
    PacketLink     link;
    WorldFix       fix;
    bool           hunt_on = false;             ///< hunting: `hs` is this run's
    Hunter::Status hs;
    KnownMap       u;                           ///< usable_identity of the worker's
    MapShows       shows = MapShows::NothingYet;
    MapEdit        edit = MapEdit::MapUnknown;
    std::string    stem;                        ///< the map known: "" none
    std::string    name;                        ///< its name, or the stem
    const GameMapBook::Page* page = nullptr;    ///< its file, when drawn
    ZoneLimits     limits;
    std::optional<HuntZone> zone;               ///< saved for it
    /// This run's spot, on this map entry: as the hunter has it, and as a
    /// zone it can be saved as (none with no floor near it).
    std::optional<HuntZone> spot_raw;
    std::optional<HuntZone> spot;
    bool           spot_editable = false;
    bool           you = false;                 ///< drawn: Known, or Lost at the last place
    bool           you_lost = false;
    /// Where the packets have you -- the hunter's own place, which the grey
    /// and its monster box go by -- and where you are drawn (you_dot).
    double         you_x = 0.0, you_y = 0.0;
    MapDot         you_at;
    /// The monsters drawn (map_shows_mobs), and where: v.mobs' dots, in its
    /// order (mob_dots); none while they are not.
    bool           mobs = false;
    std::vector<MapDot> dots;
    // the canvas
    ImVec2         origin, size;
    Bounds         bounds;
};

GuiApp::MapUi& GuiApp::map_ui() {
    if (!map_ui_) {
        map_ui_.reset(new MapUi);
        map_ui_->path = exe_relative("configs/spots.ini");
        map_ui_->ws = std::make_unique<MapViewState>();
        // The game's list of places, looked up on the worker at its first
        // need: the installer's record.
        map_ui_->ws->table_path = [] { return MapTable::default_path(); };
    }
    return *map_ui_;
}

bool GuiApp::map_tab_open() const {
    return ImGui::GetCurrentContext() != nullptr && map_tab_frame_ + 1 >= ImGui::GetFrameCount();
}

std::string GuiApp::map_name_of(const std::string& stem) const {
    if (map_ui_ && !stem.empty() && map_ui_->cur.id.stem == stem && !map_ui_->cur.map_name.empty())
        return map_ui_->cur.map_name;
    return stem;
}

bool GuiApp::map_known_now() const {
    // Positions off, or the capture stopped by hand: the map last known may be gone by.
    if (!map_ui_ || !packets_wanted()) return false;
    const MapUi& ui = *map_ui_;
    return usable_identity(ui.cur.id, map_logins_, map_lose_on_, map_lose_epoch_, packet_world_.place().epoch).by !=
           KnownMap::By::None;
}

std::shared_ptr<const GameMapBook::Page> GuiApp::map_page_now() const {
    if (!map_ui_ || !packets_wanted()) return nullptr;
    const MapUi& ui = *map_ui_;
    const KnownMap u =
        usable_identity(ui.cur.id, map_logins_, map_lose_on_, map_lose_epoch_, packet_world_.place().epoch);
    if (u.by == KnownMap::By::None || u.file_doubtful || ui.cur.id.stem != u.stem) return nullptr;
    return ui.cur.page;
}

std::optional<HuntZone> GuiApp::map_zone_now(std::string* name) const {
    if (!map_ui_ || !packets_wanted()) return std::nullopt;
    const MapUi& ui = *map_ui_;
    if (!ui.book_loaded || ui.book_bad) return std::nullopt;
    const KnownMap u =
        usable_identity(ui.cur.id, map_logins_, map_lose_on_, map_lose_epoch_, packet_world_.place().epoch);
    if (u.by == KnownMap::By::None) return std::nullopt;
    if (name != nullptr) *name = map_name_of(u.stem);
    return ui.book.get(u.stem);
}

// ------------------------------------------------------------- the worker

void GuiApp::map_post(MapUi& ui, std::uint64_t now) {
    MapUi* const p = &ui;
    const std::string path = ui.path;
    // spots.ini, once, whatever else is asked: no edit before it is read.
    if (!ui.book_asked) {
        ui.book_asked = ui.worker.post([p, path] {
            ZoneBook fresh;
            const ZoneBook::Load how = fresh.load(path);
            std::lock_guard<std::mutex> lock(p->mx);
            p->book_in = std::move(fresh);
            p->book_load = how;
        });
    }
    // The saves owed: one batch out at a time, the latest value of each map.
    if (!ui.saves.out() && ui.saves.owed() && now >= ui.save_retry_ms) {
        if (std::optional<ZoneSaveQueue::Batch> batch = ui.saves.take()) {
            const bool posted = ui.worker.post([p, path, b = *batch] {
                const bool ok = ZoneBook::save_many(path, b);
                std::lock_guard<std::mutex> lock(p->mx);
                p->save_done = MapUi::SaveDone{ b, ok };
            });
            // The queue full: owed again, for the next pass.
            if (!posted) ui.saves.done(*batch, false);
        }
    }

    // Which map, and its file: while the tab is shown or the hunter goes by
    // a zone, every kJobEveryMs and at once after a map entry; otherwise
    // every kKeepAliveMs. One at a time.
    if (!packets_wanted() || ui.worker.busy()) return;
    const bool shown = ui.shown_ms != 0 && now - ui.shown_ms <= kShownMs;
    const bool wanted = shown || (hunting_ && hunt_config_.spot);
    const std::uint32_t epoch = packet_world_.place().epoch;
    const std::uint64_t every = wanted ? kJobEveryMs : kKeepAliveMs;
    if (now - ui.post_ms < every && (!wanted || epoch == ui.posted_epoch)) return;
    // By value: the log ins counted and the gate's distance as they are now.
    const std::uint64_t gen = map_logins_;
    const int gate_px = hunt_config_.drop_gate_px;
    const bool posted = ui.worker.post([this, p, gen, gate_px] {
        const std::uint64_t at = now_ms();
        // A walk followed as PacketEyes follows it: 3 s while the hunter holds it.
        const std::uint64_t follow = hunter_ && at <= hunter_->walk_held_until_ms() ? PacketWorld::kOwnReckonMs
                                                                                     : PacketWorld::kOwnStepReckonMs;
        PacketLink link;
        link.capturing = capture_.running();
        link.npcap = link.capturing || position_npcap_ok_.load();
        link.last_tcp_ms = capture_.stats().last_tcp_ms;
        MapViewResult r = MapViewJob::run(packet_world_.view(at, follow), packet_world_.stats(), link, at, gen,
                                          gate_px, game_maps_, *p->ws);
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(p->mx);
            changed = r.id.by != p->slot.id.by || r.id.stem != p->slot.id.stem ||
                      r.gates.size() != p->slot.gates.size() || r.map_name != p->slot.map_name;
            p->slot = std::move(r);
            p->slot_new = true;
        }
        // A new map, or its name: drawn now, not at the next paced frame.
        if (changed) request_redraw();
    });
    if (posted) {
        ui.post_ms = now;
        ui.posted_epoch = epoch;
    }
}

void GuiApp::simple_map_tick() {
    const std::uint64_t now = now_ms();
    // The keeper's crossing as it begins: the map entry a log in at its end
    // leaves behind (simple_map_lose).
    const bool crossing = keeper_crossing();
    if (crossing && !was_crossing_) {
        cross_epoch_ = packet_world_.place().epoch;
        cross_ms_ = now;
    }
    was_crossing_ = crossing;

    // Made when the tab is first drawn, or a hunt goes by a zone.
    if (!map_ui_ && !(hunting_ && hunt_config_.spot)) {
        std::lock_guard<std::mutex> lock(plate_floors_mx_);
        plate_floors_.reset();
        return;
    }
    MapUi& ui = map_ui();
    map_post(ui, now);

    // What the worker has finished.
    bool fresh = false;
    std::optional<ZoneBook> book;
    ZoneBook::Load load = ZoneBook::Load::Missing;
    std::optional<MapUi::SaveDone> saved;
    {
        std::lock_guard<std::mutex> lock(ui.mx);
        if (ui.slot_new) {
            ui.cur = ui.slot;
            ui.slot_new = false;
            fresh = true;
        }
        book.swap(ui.book_in);
        load = ui.book_load;
        saved.swap(ui.save_done);
    }
    if (fresh) {
        ui.ran = true;
        ui.zone_due = true;
        if (ui.cur.fix.settled()) {
            ui.last_settled = { round_px(ui.cur.fix.rx), round_px(ui.cur.fix.ry) };
            ui.last_settled_epoch = ui.cur.fix.epoch;
            ui.have_settled = true;
        }
    }
    // Positions off, or the capture stopped by hand: no job looks, so the map last
    // known is not known now -- set aside at the hand-off as at a capture lost.
    if (!packets_wanted()) ui.cur.id = KnownMap{};
    if (book) {
        ui.book = std::move(*book);
        ui.book_bad = load == ZoneBook::Load::Bad;
        ui.book_loaded = !ui.book_bad;
        ui.zone_due = true;
        if (ui.book_bad)
            GB_WARN("Map tab: configs\\spots.ini exists but does not read -- hunting areas are not edited or saved "
                    "this session");
        else if (ui.book.size() > 0 || ui.book.skipped() > 0)
            GB_INFO("Map tab: %zu hunting areas read from configs\\spots.ini%s", ui.book.size(),
                    ui.book.skipped() > 0 ? " (some sections not read as one, left in the file)" : "");
    }
    if (saved) {
        ui.saves.done(saved->batch, saved->ok);
        if (!saved->ok) {
            ui.save_retry_ms = now + kSaveRetryMs;
            // Once a run of failures: it is tried again every few seconds.
            if (!ui.save_failed) {
                std::string stems;
                for (const auto& owed : saved->batch) stems += (stems.empty() ? "" : ", ") + owed.first;
                GB_WARN("Map tab: could not write configs\\spots.ini (%s)", stems.c_str());
            }
            ui.save_failed = true;
        } else {
            ui.save_failed = false;
        }
    }
    map_zone_tick(ui, now);

    // The floors the hunter's plates and its seek place the monsters on
    // (2026.11.05b, "Tự tìm quái trong vùng"): the dots' own -- the map known,
    // its file not found off -- while the hunter goes by this map's area with
    // the seek on. None otherwise: the packets' own points, as 2026.11.05.
    std::shared_ptr<const GameMapBook::Page> floors;
    if (hunting_ && hunter_ && hunt_config_.spot && hunt_config_.seek_in_zone && packets_wanted() && ui.cur.page) {
        const KnownMap u =
            usable_identity(ui.cur.id, map_logins_, map_lose_on_, map_lose_epoch_, packet_world_.place().epoch);
        if (u.by != KnownMap::By::None && !u.file_doubtful && ui.cur.id.stem == u.stem) {
            const Hunter::Status hs = hunter_->status();
            if (hs.spot_from_map && !hs.spot_suspended && hs.spot_stem == u.stem) floors = ui.cur.page;
        }
    }
    std::lock_guard<std::mutex> lock(plate_floors_mx_);
    plate_floors_ = std::move(floors);
}

void GuiApp::map_zone_tick(MapUi& ui, std::uint64_t now) {
    // A start, a stop, the spot ticked or not: what was sent goes again --
    // a start's fresh_run drops whatever the hunter was asked.
    if (hunting_ != ui.was_hunting || hunt_config_.spot != ui.was_spot) {
        ui.was_hunting = hunting_;
        ui.was_spot = hunt_config_.spot;
        ui.sent = {};
        ui.aside_sent.clear();
        ui.zone_due = true;
    }
    if (!hunting_ || !hunter_ || !hunt_config_.spot) return;
    if (!ui.zone_due && now - ui.zone_ms < kZoneTickMs) return;
    ui.zone_due = false;
    ui.zone_ms = now;

    const Hunter::Status hs = hunter_->status();
    const KnownMap u = packets_wanted() ? usable_identity(ui.cur.id, map_logins_, map_lose_on_, map_lose_epoch_,
                                                          packet_world_.place().epoch)
                                        : KnownMap{};
    // The tab no longer places the character on the map of the zone in use,
    // or of one waiting: its pixels may be another map's. Set aside, once a
    // stay, until the map is known again and the zone sent again.
    const auto gone = [&](const std::string& stem) {
        return !stem.empty() && (u.by == KnownMap::By::None || u.stem != stem) && ui.aside_sent != stem;
    };
    std::string aside;
    if (hs.spot_from_map && !hs.spot_suspended && gone(hs.spot_stem)) aside = hs.spot_stem;
    else if (hs.zone_wait != Hunter::ZoneWait::None && gone(hs.zone_wait_stem)) aside = hs.zone_wait_stem;
    if (!aside.empty()) {
        hunter_->zone_aside(aside);
        ui.aside_sent = aside;
        ui.sent = {};
    }

    // The saved zone of the map the character is on, sent for this map
    // entry: again whenever the entry, the log ins or the zone change.
    if (!ui.book_loaded || ui.book_bad || u.by == KnownMap::By::None) return;
    const std::optional<HuntZone> z = ui.book.get(u.stem);
    if (!z) return;
    const GameMapBook::Page* page = ui.cur.page && ui.cur.id.stem == u.stem ? ui.cur.page.get() : nullptr;
    if (zone_send(u, *z, page, ui.have_settled, ui.last_settled_epoch, ui.last_settled) != ZoneSend::Send) return;
    const MapUi::Sent key{ true, u.stem, u.epoch, map_logins_, ui.book.revision(u.stem) };
    if (key == ui.sent) return;
    Hunter::Zone hz;
    hz.stem = u.stem;
    hz.epoch = u.epoch;
    hz.home_x = static_cast<float>(z->home_x);
    hz.home_y = static_cast<float>(z->home_y);
    hz.left = static_cast<float>(z->left);
    hz.top = static_cast<float>(z->top);
    hz.right = static_cast<float>(z->right);
    hz.bottom = static_cast<float>(z->bottom);
    hunter_->set_zone(hz);
    ui.sent = key;
    ui.aside_sent.clear();
    GB_INFO("Map tab: hunting area for %s handed to the hunter -- %s (map entry %u, %s)", u.stem.c_str(),
            zone_text(*z).c_str(), static_cast<unsigned>(u.epoch),
            u.by == KnownMap::By::Entry ? "by its map entry" : "named from standing points");
}

void GuiApp::simple_map_lose() {
    // Counted whether the tab is made or not: a tab made after it never goes
    // by what came before.
    ++map_logins_;
    map_lose_epoch_ = lose_epoch_for(now_ms(), cross_ms_, cross_epoch_, packet_world_.place().epoch);
    map_lose_on_ = true;
    if (!map_ui_) return;
    MapUi& ui = *map_ui_;
    ui.sent = {};
    ui.aside_sent.clear();
    ui.zone_due = true;
    GB_INFO("Map tab: logged back in -- the map is known again from the next map entry, or named afresh (map entry "
            "%u left behind)",
            static_cast<unsigned>(map_lose_epoch_));
}

void GuiApp::simple_map_stop() {
    if (!map_ui_) return;
    MapUi& ui = *map_ui_;
    // The job under way let finish -- it reads the packets and the map files,
    // which go after this -- and the zones still owed written, a batch at a
    // time, waited for: a save takes milliseconds.
    ui.worker.wait_idle();
    const auto take_done = [&ui] {
        std::lock_guard<std::mutex> lock(ui.mx);
        if (!ui.save_done) return;
        ui.saves.done(ui.save_done->batch, ui.save_done->ok);
        ui.save_done.reset();
    };
    take_done();
    for (int tries = 0; tries < 3 && ui.saves.owed() && !ui.book_bad; ++tries) {
        std::optional<ZoneSaveQueue::Batch> batch = ui.saves.take();
        if (!batch) break;
        MapUi* const p = &ui;
        const std::string path = ui.path;
        if (!ui.worker.post([p, path, b = *batch] {
                const bool ok = ZoneBook::save_many(path, b);
                std::lock_guard<std::mutex> lock(p->mx);
                p->save_done = MapUi::SaveDone{ b, ok };
            })) {
            ui.saves.done(*batch, false);
            break;
        }
        ui.worker.wait_idle();
        take_done();
    }
    if (ui.saves.owed())
        GB_WARN("Map tab: could not write configs\\spots.ini as GoBot closed -- the last hunting areas edited are "
                "not saved");
    ui.worker.stop();
}

// ---------------------------------------------------------------- commits

void GuiApp::map_note(MapUi& ui, const std::string& msg, bool bad) {
    simple_note(msg, kNoteMs);
    ui.note = msg;
    ui.note_bad = bad;
    ui.note_until = now_ms() + kNoteMs;
}

void GuiApp::map_commit_zone(MapUi& ui, const std::string& stem, const std::optional<HuntZone>& next,
                             MapCommit how) {
    const std::optional<HuntZone> before = ui.book.get(stem);
    std::optional<HuntZone> after = next;
    if (after) {
        after->saved = local_minute();
        if (!ui.book.put(stem, *after)) return;
    } else {
        ui.book.erase(stem);
    }
    ui.saves.commit(zone_stem(stem), after);
    if (how == MapCommit::Undo) ui.undo = {};
    else ui.undo = { true, stem, before };
    // Worked out again for the zone as it is now, and sent again.
    ui.check_key.clear();
    ui.sent = {};
    ui.zone_due = true;
    // Gone: a spot of it at the hunter is dropped, and taken again where the
    // character stands.
    if (!after && hunter_) hunter_->clear_zone(stem);

    const std::string was = before ? "was " + zone_text(*before) : std::string("none before");
    const std::string name = map_name_of(stem);
    if (how == MapCommit::Undo) {
        GB_INFO("Map tab: undo -- hunting area for %s put back (%s)", stem.c_str(),
                after ? zone_text(*after).c_str() : "none");
        map_note(ui, after ? "Đã hoàn tác: vùng đánh của bản đồ " + name + " như trước lần sửa vừa rồi."
                           : "Đã hoàn tác: bản đồ " + name + " không còn vùng đánh.");
        return;
    }
    if (!after) {
        GB_INFO("Map tab: hunting area for %s deleted (%s)", stem.c_str(), was.c_str());
        map_note(ui, "Đã xoá vùng đánh của bản đồ " + name + ". Chỗ đánh quái lấy chỗ nhân vật đứng như trước.");
        return;
    }
    const char* why = how == MapCommit::New        ? "new"
                      : how == MapCommit::FromSpot ? "from this run's spot"
                      : how == MapCommit::Here     ? "moved by Đặt chỗ này"
                      : how == MapCommit::Moved    ? "moved"
                                                   : "edited";
    GB_INFO("Map tab: hunting area for %s saved: %s (%s; %s)", stem.c_str(), zone_text(*after).c_str(), why,
            was.c_str());
    switch (how) {
        case MapCommit::FromSpot:
            map_note(ui, "Đã lưu chỗ này thành vùng của bản đồ " + name + " — bấm Hoàn tác để bỏ.");
            break;
        case MapCommit::Here:
            map_note(ui, "Đã dời vùng đánh của bản đồ " + name +
                             " tới chỗ nhân vật đứng (đã lưu) — bấm Hoàn tác ở tab Bản đồ để bỏ.");
            break;
        case MapCommit::Moved:
            map_note(ui, "Đã dời vùng đánh của bản đồ " + name + " (đã lưu) — bấm Hoàn tác để bỏ.");
            break;
        default:
            map_note(ui, "Đã lưu vùng đánh của bản đồ " + name + ".");
            break;
    }
}

bool GuiApp::map_try(MapUi& ui, const MapView& mv, const std::optional<HuntZone>& next, MapCommit how,
                     bool around_you) {
    if (mv.edit != MapEdit::Ok || mv.stem.empty()) return false;
    if (!next) {
        map_note(ui, kNoFloorNear);
        return false;
    }
    const std::span<const MapRun> marks =
        ui.cur.no_drop ? std::span<const MapRun>(*ui.cur.no_drop) : std::span<const MapRun>();
    const ZoneCheck c = zone_check(mv.page, *next, mv.limits, marks, hunt_config_.drop_down);
    if (c.refuse_vi != nullptr) {
        map_note(ui, c.refuse_vi);
        return false;
    }
    map_commit_zone(ui, mv.stem, next, how);
    // "Vùng mới đặt chỗ về ở chỗ nhân vật đứng" only for the zone made round
    // the character: one made at a point clicked has its home there.
    map_red_note(ui, mv.stem, c, around_you);
    return true;
}

void GuiApp::map_red_note(MapUi& ui, const std::string& stem, const ZoneCheck& c, bool made_new) {
    const std::string note = red_note_vi(c, made_new);
    if (note.empty()) return;
    GB_INFO("Map tab: hunting area for %s has %d of %d floor columns red -- more than one jump below its home, not "
            "walked back from",
            stem.c_str(), c.red_cols, c.floor_cols);
    map_note(ui, note, true);
}

bool GuiApp::map_zone_here() {
    // The owner's Q2: (b) leaves "Đặt chỗ này" as it was, the spot for this
    // run only (Hunter::retake_spot).
    if (!kSpotHereMovesZone || !map_ui_) return false;
    MapUi& ui = *map_ui_;
    MapView mv;
    map_look(ui, mv);
    if (mv.edit != MapEdit::Ok || !mv.zone) return false;
    if (!mv.fix.settled()) {
        map_note(ui, "Chờ nhân vật đứng yên trên mặt đất rồi bấm lại.");
        return true;
    }
    const MapPoint at{ round_px(mv.fix.rx), round_px(mv.fix.ry) };
    // A named map with something of yours off its floors: maybe another map's pixels.
    if (!zone_here_ok(mv.u, mv.page, at)) {
        map_note(ui, kNotSureHere);
        return true;
    }
    map_try(ui, mv, zone_moved(mv.page, *mv.zone, at, mv.limits), MapCommit::Here);
    return true;
}

// ------------------------------------------------------------------- look

void GuiApp::map_look(MapUi& ui, MapView& mv) {
    mv.now = now_ms();
    const std::uint64_t follow = hunter_ && mv.now <= hunter_->walk_held_until_ms() ? PacketWorld::kOwnReckonMs
                                                                                     : PacketWorld::kOwnStepReckonMs;
    mv.v = packet_world_.view(mv.now, follow);
    mv.link.capturing = capture_.running();
    mv.link.npcap = mv.link.capturing || position_npcap_ok_.load();
    mv.link.last_tcp_ms = capture_.stats().last_tcp_ms;
    mv.fix = world_fix(mv.v, packet_world_.stats(), mv.link, mv.now);
    mv.hunt_on = hunting_ && hunter_ != nullptr;
    if (mv.hunt_on) mv.hs = hunter_->status();
    if (mv.fix.settled()) {
        ui.last_settled = { round_px(mv.fix.rx), round_px(mv.fix.ry) };
        ui.last_settled_epoch = mv.fix.epoch;
        ui.have_settled = true;
    }
    if (mv.fix.state == WorldFix::State::Known) {
        ui.last_x = mv.fix.x;
        ui.last_y = mv.fix.y;
        ui.have_last = true;
    }
    mv.you = mv.fix.state == WorldFix::State::Known ||
             (mv.fix.state == WorldFix::State::Lost && ui.have_last);
    mv.you_lost = mv.fix.state == WorldFix::State::Lost;
    mv.you_x = mv.you_lost ? ui.last_x : mv.fix.x;
    mv.you_y = mv.you_lost ? ui.last_y : mv.fix.y;

    // Which map: the worker's, while its entry and the log ins are the live ones.
    mv.u = usable_identity(ui.cur.id, map_logins_, map_lose_on_, map_lose_epoch_, mv.fix.epoch);
    MapShowsIn in;
    in.positions_off = hunt_config_.packet_positions == PacketPositions::Off;
    in.state = mv.fix.state;
    in.live_epoch = mv.fix.epoch;
    in.ran = ui.ran;
    in.id = ui.cur.id;
    in.usable = mv.u;
    in.page = ui.cur.page != nullptr;
    in.data_dir_ok = ui.cur.data_dir_ok;
    mv.shows = map_shows(in);
    // Nothing places the character: no map is known, whatever the job last said.
    if (mv.shows == MapShows::PositionsOff || mv.shows == MapShows::NoCapture) mv.u = KnownMap{};
    mv.edit = map_edit(ui.book_loaded, ui.book_bad, mv.u, mv.shows);
    if (mv.u.by != KnownMap::By::None) {
        mv.stem = mv.u.stem;
        mv.name = map_name_of(mv.stem);
    }
    if (map_drawn(mv.shows) && ui.cur.page && ui.cur.id.stem == mv.stem) mv.page = ui.cur.page.get();
    // The dots, on that map's floors: a walker's height its frame's floor's
    // rather than its frame's own; the monsters not at all with nothing to
    // place them by. A file found off is drawn faded, its floors not gone by.
    const GameMapBook::Page* floors = dot_floors(mv.page, mv.shows);
    mv.you_at = you_dot(floors, mv.fix, mv.you_x, mv.you_y);
    mv.mobs = map_shows_mobs(mv.shows);
    mv.dots = mob_dots(floors, mv.v, mv.shows, mv.now);
    mv.limits = zone_limits(mv.page, mv.u.file_doubtful);
    if (!mv.stem.empty() && ui.book_loaded && !ui.book_bad) mv.zone = ui.book.get(mv.stem);

    // This run's spot, of this map entry: drawn; kept as the map's zone by a
    // drag on it or "Lưu vùng này cho bản đồ" while the map has none.
    const Hunter::Status& hs = mv.hs;
    if (mv.hunt_on && hunt_config_.spot && hs.spot_set && !hs.spot_from_map && !hs.spot_suspended &&
        hs.spot_epoch == mv.fix.epoch) {
        HuntZone raw;
        raw.home_x = round_px(hs.spot_x);
        raw.home_y = round_px(hs.spot_y);
        raw.left = round_px(hs.spot_left);
        raw.top = round_px(hs.spot_top);
        raw.right = round_px(hs.spot_right);
        raw.bottom = round_px(hs.spot_bottom);
        mv.spot_raw = raw;
        if (!mv.stem.empty() && hs.spot_epoch == mv.u.epoch) {
            mv.spot = zone_from_spot(mv.page, hs.spot_x, hs.spot_y, hs.spot_left, hs.spot_top, hs.spot_right,
                                     hs.spot_bottom, mv.limits);
            mv.spot_editable = mv.spot.has_value() && !mv.zone && mv.edit == MapEdit::Ok && spot_keep_ok(mv.u);
        }
    }
}

// --------------------------------------------------------------- the tab

void GuiApp::simple_map_tab() {
    MapUi& ui = map_ui();
    MapView mv;
    map_look(ui, mv);
    ui.shown_ms = mv.now;
    map_tab_frame_ = ImGui::GetFrameCount();

    // Another map shown: what went with the last one goes -- Hoàn tác but
    // for this map's own, which "Đặt chỗ này" may have left with the tab
    // shut, and kept through a moment with no map known.
    if (mv.stem != ui.shown_stem) {
        ui.shown_stem = mv.stem;
        if (!mv.stem.empty() && ui.undo.stem != mv.stem) ui.undo = {};
        ui.check_key.clear();
        ui.gesture.reset();
        ui.draft.reset();
        ui.ask_delete = false;
    }
    // The shown zone's warnings: worked out when it -- at every edit -- its
    // file, its gates or the drop setting changes, never a frame at a time.
    if (mv.zone) {
        char key[176];
        std::snprintf(key, sizeof key, "%s#%llu#%d#%p#%p#%d#%zu", mv.stem.c_str(),
                      static_cast<unsigned long long>(ui.book.revision(mv.stem)), hunt_config_.drop_down ? 1 : 0,
                      static_cast<const void*>(mv.page), static_cast<const void*>(ui.cur.no_drop.get()),
                      mv.limits.floors ? 1 : 0, ui.cur.gates.size());
        if (ui.check_key != key) {
            ui.check_key = key;
            const std::span<const MapRun> marks =
                ui.cur.no_drop ? std::span<const MapRun>(*ui.cur.no_drop) : std::span<const MapRun>();
            // The gates as the tab has them, the page's and those learned --
            // of this map: with its page -- a home by one warned of.
            const std::span<const MapGate> gates =
                mv.page != nullptr ? std::span<const MapGate>(ui.cur.gates) : std::span<const MapGate>();
            ui.check = zone_check(mv.page, *mv.zone, mv.limits, marks, hunt_config_.drop_down, gates);
        }
    }

    trial_line(kTrialMap);

    // ---- which map ----
    const std::string title = mv.name == mv.stem ? mv.stem : mv.name + " (" + mv.stem + ")";
    const char* const how = mv.u.by == KnownMap::By::Entry ? "game báo khi vào bản đồ" : "nhận ra từ chỗ đứng";
    const MapTrust::Report& rep = mv.u.report;
    switch (mv.shows) {
        case MapShows::PositionsOff:
            coloured(kWait, "Vị trí từ gói tin đang tắt: bản đồ cần gói tin của game để biết nhân vật và quái ở đâu. "
                            "Đánh dấu “Vị trí từ gói tin” ở tab Đánh quái.");
            break;
        case MapShows::NoCapture:
            if (!mv.link.npcap)
                coloured(kWait, "Chưa có Npcap: bản đồ cần gói tin của game để biết nhân vật và quái ở đâu. Cài "
                                "Npcap, hoặc bật dịch vụ Npcap (máy từng chạy GoC thì thường đã có).");
            else
                coloured(kWait, "Chưa nghe được gói tin của game (chưa bắt được, hoặc game chưa kết nối) — chưa "
                                "biết nhân vật và quái ở đâu.");
            break;
        case MapShows::Changing:
            hint("Đang đổi bản đồ…");
            break;
        case MapShows::Naming:
            hint(("Đang nhận ra bản đồ: " + std::to_string(rep.points) + "/" + std::to_string(rep.needed) +
                  " chỗ đứng — đi lại vài bước trên các bục.")
                     .c_str());
            break;
        case MapShows::Ambiguous:
            hint("Chưa phân biệt được bản đồ (có bản đồ khác giống chỗ này) — đi tới bục khác vài bước, hoặc qua "
                 "cửa.");
            break;
        case MapShows::NamedDoubtful:
            hint(("Chưa chắc bản đồ: file " + rep.stem +
                  " không khớp chỗ nhân vật đứng — đi tới bục khác vài bước, hoặc qua cửa.")
                     .c_str());
            break;
        case MapShows::NothingYet:
            hint("Chưa biết bản đồ — qua cửa hoặc đăng nhập lại một lần khi GoBot đang mở, hoặc đi vài bước để GoBot "
                 "nhận ra.");
            break;
        case MapShows::NoGame:
            coloured(kWait, "Không tìm thấy thư mục bản đồ của game (data\\Map) — chỉ hiện vị trí.");
            break;
        case MapShows::NoFile:
            coloured(kWait, ("Không có file bản đồ " + mv.stem + " trong thư mục của game — chỉ hiện vị trí.").c_str());
            break;
        case MapShows::EntryDoubtful:
            coloured(kWait, ("Bản đồ: " + title +
                             " — file bản đồ có thể không khớp chỗ nhân vật đứng: bục vẽ mờ, Nhảy xuống không dùng "
                             "file này; vùng đánh vẫn dùng được.")
                                .c_str());
            break;
        case MapShows::NoGates:
        case MapShows::ByEntry:
        case MapShows::ByName:
            coloured(kGood, ("Bản đồ: " + title + " · " + how).c_str());
            break;
    }
    if (mv.page != nullptr) {
        const std::size_t learned = ui.cur.learned_gates;
        const std::size_t own = ui.cur.gates.size() - std::min(learned, ui.cur.gates.size());
        std::string tip = std::to_string(mv.page->floors.w_px()) + "×" + std::to_string(mv.page->floors.h_px()) +
                          " px · " + std::to_string(own) + " cửa trong file .prj";
        if (learned > 0) tip += ", thêm " + std::to_string(learned) + " cửa GoBot đã thấy trong lần chạy này";
        ImGui::SetItemTooltip("%s", tip.c_str());
    }
    if (mv.shows == MapShows::NoGates)
        hint("File .prj của bản đồ này không có cửa: Nhảy xuống không dùng file này.");
    if (mv.fix.state == WorldFix::State::FirstReport)
        hint("Chờ nhân vật đi một bước hay dùng một kỹ năng để gói tin báo vị trí.");
    else if (mv.fix.state == WorldFix::State::Lost)
        coloured(kWait, "Gói tin tạm không biết nhân vật ở đâu.");
    if (mv.you) {
        std::string line = "Nhân vật: " + place_vi(mv.you_at, mv.you_x, mv.you_y);
        if (mv.mobs) {
            int alive = 0;
            for (const PacketWorld::Mob& m : mv.v.mobs)
                if (!m.dead) ++alive;
            line += " · Quái: " + std::to_string(alive);
        }
        line += " · Tỉ lệ " + px(ui.cam.zoom * 100.0) + "%";
        hint(line.c_str());
    }

    // ---- the view row ----
    {
        const ImVec4 on = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        const auto mode_button = [&on](const char* text, bool lit) {
            if (lit) ImGui::PushStyleColor(ImGuiCol_Button, on);
            const bool pressed = ImGui::Button(text);
            if (lit) ImGui::PopStyleColor();
            return pressed;
        };
        if (mode_button("Theo nhân vật", ui.mode == MapUi::Mode::Follow)) ui.mode = MapUi::Mode::Follow;
        ImGui::SetItemTooltip("Bản đồ đi theo nhân vật. Nhấp đúp lên bản đồ cũng được.");
        ImGui::SameLine();
        if (mode_button("Cả bản đồ", ui.mode == MapUi::Mode::Fit)) ui.mode = MapUi::Mode::Fit;
        ImGui::SetItemTooltip("Xem cả bản đồ.");
        ImGui::SameLine();
        if (ImGui::Button(" - ")) ui.zoom_by /= MapCamera::kZoomStep;
        ImGui::SetItemTooltip("Thu nhỏ (lăn chuột trên bản đồ cũng được).");
        ImGui::SameLine();
        if (ImGui::Button(" + ")) ui.zoom_by *= MapCamera::kZoomStep;
        ImGui::SetItemTooltip("Phóng to (lăn chuột trên bản đồ cũng được).");
        ImGui::SameLine(0, 14);
        ImGui::Checkbox("Chi tiết", &ui.details);
        ImGui::SetItemTooltip("Vẽ thêm: đường rơi khi nhân vật đang ở trên không, chỗ gói tin báo lần cuối, đường "
                              "đi về chỗ về, và khung bot tìm quái quanh nhân vật. Chỉ trong lần mở GoBot này.");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextUnformatted(
                "Kéo bản đồ để di chuyển · lăn chuột hay bấm - + để phóng to · nhấp đúp: theo nhân vật · kéo chỗ về, "
                "cạnh, góc hay dải trên cùng của vùng để sửa · chuột phải: thêm lệnh.\n"
                "Bản đồ lấy từ thư mục data của game; vị trí lấy từ gói tin.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (ui.hover) {
            const std::string at = "x " + px(ui.hover_x) + " · y " + px(ui.hover_y);
            ImGui::SameLine();
            ImGui::PushFont(nullptr, kSmall);
            const float w = ImGui::CalcTextSize(at.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", at.c_str());
            ImGui::PopFont();
        }
    }

    map_canvas(ui, mv);
    map_menu(ui, mv);

    // ---- what the colours are, then the zone's card ----
    const float below_top = ImGui::GetCursorPosY();
    {
        ImGui::PushFont(nullptr, kSmall);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = ImGui::GetTextLineHeight();
        enum class Sym { Platform, Bottom, Gate, You, Mob, Target, Zone, Home, Jump, Seek };
        const struct { Sym sym; const char* text; } items[] = {
            { Sym::Platform, "bục" }, { Sym::Bottom, "nền dưới cùng" }, { Sym::Gate, "cửa" },
            { Sym::You, "nhân vật" }, { Sym::Mob, "quái" },           { Sym::Target, "đang đánh" },
            { Sym::Zone, "vùng đánh" }, { Sym::Home, "chỗ về" },      { Sym::Jump, "nhảy lên" },
            { Sym::Seek, "đi tìm quái" },
        };
        bool first = true;
        for (const auto& it : items) {
            const float w = 16.0f + 4.0f + ImGui::CalcTextSize(it.text).x;
            if (!first) {
                ImGui::SameLine(0, 12);
                if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
            }
            first = false;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 c(p.x + 8.0f, p.y + h * 0.5f);
            switch (it.sym) {
                case Sym::Platform: dl->AddLine(ImVec2(p.x, c.y), ImVec2(p.x + 16.0f, c.y), MapColors::kPlatform, 2.0f); break;
                case Sym::Bottom:   dl->AddLine(ImVec2(p.x, c.y), ImVec2(p.x + 16.0f, c.y), MapColors::kBottom, 3.0f); break;
                case Sym::Gate:     plus_mark(dl, c, 4.0f, MapColors::kGateMark); break;
                case Sym::You:
                    dl->AddCircleFilled(c, 4.5f, MapColors::kYou);
                    dl->AddCircle(c, 6.0f, MapColors::kWhite, 0, 1.5f);
                    break;
                case Sym::Mob:
                    dl->AddCircleFilled(c, 3.5f, MapColors::kMob);
                    dl->AddCircle(c, 4.5f, MapColors::kMobRing, 0, 1.0f);
                    break;
                case Sym::Target:   dl->AddCircle(c, 6.0f, MapColors::kTarget, 0, 2.0f); break;
                case Sym::Zone:
                    dl->AddRectFilled(ImVec2(p.x + 1.0f, c.y - 5.0f), ImVec2(p.x + 15.0f, c.y + 5.0f), MapColors::kZoneFill);
                    dl->AddRect(ImVec2(p.x + 1.0f, c.y - 5.0f), ImVec2(p.x + 15.0f, c.y + 5.0f), MapColors::kZoneLine, 0.0f, 1.5f);
                    break;
                case Sym::Home:     home_pin(dl, ImVec2(c.x, c.y + 6.0f), MapColors::kHome); break;
                case Sym::Jump:
                    arrow(dl, ImVec2(c.x, c.y + 5.0f), ImVec2(c.x, c.y - 5.0f), MapColors::kJump, false,
                          ImVec2(p.x, p.y), ImVec2(p.x + 16.0f, p.y + h));
                    break;
                case Sym::Seek:
                    dashed_line(dl, ImVec2(p.x, c.y), ImVec2(p.x + 11.0f, c.y), MapColors::kTarget, 1.5f,
                                ImVec2(p.x, p.y), ImVec2(p.x + 16.0f, p.y + h));
                    dl->AddRect(ImVec2(p.x + 11.0f, c.y - 2.5f), ImVec2(p.x + 16.0f, c.y + 2.5f), MapColors::kTarget,
                                0.0f, 0, 1.5f);
                    break;
            }
            ImGui::Dummy(ImVec2(16.0f, h));
            ImGui::SameLine(0, 4);
            ImGui::TextDisabled("%s", it.text);
            if (it.sym == Sym::Mob)
                ImGui::SetItemTooltip("Quái màu xám: ở ngoài vùng đánh và ngoài tầm đánh — hoặc ở ngoài vùng đánh "
                                      "trong lúc bot đi bộ về vùng trước, rồi mới đánh tiếp — bot không đi tới."
                                      " Có Tự tìm quái trong vùng: cả con trong vùng mà bot không tới được, hoặc "
                                      "đang tạm bỏ qua.");
            if (it.sym == Sym::Jump)
                ImGui::SetItemTooltip("Mũi tên: chỗ GoBot sẽ nhảy lên (nét đứt: lẽ ra đã nhảy, khi Nhảy lên bục trên "
                                      "đang tắt). Dấu xám: chỗ đã nhảy hỏng và bục không nhảy xuyên lên được, GoBot "
                                      "nhớ tới khi tắt GoBot.");
            if (it.sym == Sym::Seek)
                ImGui::SetItemTooltip("Đường nét đứt: GoBot đang đi tới chỗ đứng đánh (ô vuông nhỏ) con quái có "
                                      "vòng. Vòng nét đứt: con quái đi khuất, GoBot đoán chỗ nó đang ở.");
        }
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0, 4));
    map_card(ui, mv);
    ui.below_h = ImGui::GetCursorPosY() - below_top;
}

// ------------------------------------------------------------- the canvas

void GuiApp::map_canvas(MapUi& ui, MapView& mv) {
    ImGuiIO& io = ImGui::GetIO();
    const float width = std::max(100.0f, ImGui::GetContentRegionAvail().x);
    const float height = std::clamp(ImGui::GetContentRegionAvail().y - ui.below_h - start_block_h_ - version_h_,
                                    kCanvasMinH, kCanvasMaxH);
    mv.origin = ImGui::GetCursorScreenPos();
    mv.size = ImVec2(width, height);
    ImGui::InvisibleButton("##map", mv.size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    // The wheel zooms here; the column does not scroll under it.
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const bool visible = ImGui::IsItemVisible();
    const ImVec2 o = mv.origin, s = mv.size;
    const ImVec2 mouse = io.MousePos;

    // ---- the camera ----
    if (mv.page != nullptr)
        mv.bounds = Bounds{ 0.0, 0.0, static_cast<double>(mv.page->floors.w_px()),
                            static_cast<double>(mv.page->floors.h_px()) };
    else
        mv.bounds = grid_bounds(mv.v, mv.dots, mv.you, mv.you_at.x, mv.you_at.y);
    const Bounds& b = mv.bounds;
    const double zmin = MapCamera::zmin_for(round_px(b.w()), round_px(b.h()), s.x, s.y);
    const double zmax = MapCamera::zmax_for(zmin);
    const double fz = MapCamera::follow_zoom(zmin, zmax);
    // Another map: placed afresh, following the character when it is known.
    // A moment with no map known (a map entry, a log in, a map named again)
    // keeps the camera as it was; the first frame places it.
    const std::string cam_for = mv.stem.empty() && !ui.cam_for.empty() ? ui.cam_for
                                : mv.page != nullptr                    ? mv.stem
                                                                        : mv.stem + "#grid";
    if (ui.cam_for != cam_for) {
        ui.cam_for = cam_for;
        ui.mode = mv.you ? MapUi::Mode::Follow : MapUi::Mode::Fit;
        ui.cam.zoom = fz;
        ui.gesture.reset();
        ui.draft.reset();
    }
    // A drag on a grip freezes the camera until its release: what is under
    // the mouse stays what was under it.
    if (!ui.gesture) {
        // A zoom leaves Fit, and Follow with nobody to follow: else the
        // camera's placing would undo it at once.
        const bool follow = ui.mode == MapUi::Mode::Follow && mv.you;
        if (ui.zoom_by != 1.0) {
            if (ui.mode != MapUi::Mode::Free && !follow) ui.mode = MapUi::Mode::Free;
            ui.cam.zoom = std::clamp(ui.cam.zoom * ui.zoom_by, zmin, zmax);
        }
        if (hovered && io.MouseWheel != 0.0f) {
            const double f = std::pow(MapCamera::kZoomStep, static_cast<double>(io.MouseWheel));
            if (follow) {
                ui.cam.zoom = std::clamp(ui.cam.zoom * f, zmin, zmax);
            } else {
                ui.mode = MapUi::Mode::Free;
                ui.cam.zoom_about(cam_p(mouse), f, cam_p(o), cam_p(s), zmin, zmax);
            }
        }
        // A drag that grips nothing pans, with any button.
        if (active) {
            for (const ImGuiMouseButton button : { ImGuiMouseButton_Left, ImGuiMouseButton_Right,
                                                    ImGuiMouseButton_Middle }) {
                if (!ImGui::IsMouseDragging(button) || (io.MouseDelta.x == 0.0f && io.MouseDelta.y == 0.0f))
                    continue;
                ui.mode = MapUi::Mode::Free;
                ui.cam.cx -= io.MouseDelta.x / ui.cam.zoom;
                ui.cam.cy -= io.MouseDelta.y / ui.cam.zoom;
                break;
            }
        }
        if (ui.mode == MapUi::Mode::Follow && mv.you) {
            ui.cam.zoom = std::clamp(ui.cam.zoom, zmin, zmax);
            ui.cam.cx = mv.you_at.x;
            ui.cam.cy = mv.you_at.y;
        } else if (ui.mode != MapUi::Mode::Free) {
            fit_to(ui.cam, b, s);
        } else {
            ui.cam.zoom = std::clamp(ui.cam.zoom, zmin, zmax);
            clamp_to(ui.cam, b);
        }
    }
    ui.zoom_by = 1.0;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        ui.mode = mv.you ? MapUi::Mode::Follow : MapUi::Mode::Fit;

    // ---- the grips ----
    // The saved zone, or this run's spot while the map has none; edges and
    // corners only with no floors to put the home on.
    const HuntZone* gripped = mv.zone ? &*mv.zone : mv.spot_editable ? &*mv.spot : nullptr;
    const bool can_edit = mv.edit == MapEdit::Ok && gripped != nullptr;
    ZoneGrip hot = ZoneGrip::None;
    if (can_edit && hovered && !ui.gesture)
        hot = zone_grip(*gripped, ui.cam, cam_p(o), cam_p(s), cam_p(mouse), mv.limits.floors);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ui.gesture && hot != ZoneGrip::None) {
        ZoneGesture g;
        g.grip = hot;
        g.from = *gripped;
        g.cam = ui.cam;
        g.origin = cam_p(o);
        g.size = cam_p(s);
        g.press = cam_p(mouse);
        ui.gesture = g;
        ui.gesture_spot = !mv.zone;
        ui.gesture_stem = mv.stem;
    }
    if (ui.gesture) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) map_drag(ui, mv);
        else map_release(ui, mv);
    } else if (hot != ZoneGrip::None) {
        ImGui::SetMouseCursor(grip_cursor(hot));
    }

    // ---- the menu: a right click that did not drag ----
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ui.right_down = true;
    if (ui.right_down && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ui.right_down = false;
        if (hovered && !ui.gesture && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right)) {
            const MapCamera::P w = ui.cam.to_world(cam_p(mouse), cam_p(o), cam_p(s));
            ui.menu_at = { round_px(w.x), round_px(w.y) };
            ui.menu_stem = mv.stem;
            ImGui::OpenPopup(kMenu);
        }
    }

    ui.hover = hovered;
    if (hovered) {
        const MapCamera::P w = ui.cam.to_world(cam_p(mouse), cam_p(o), cam_p(s));
        ui.hover_x = w.x;
        ui.hover_y = w.y;
    }
    if (!visible) return;

    // ---- drawing ----
    const ToScreen ts(ui.cam, o, s);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 lo = o, hi(o.x + s.x, o.y + s.y);
    dl->AddRectFilled(lo, hi, MapColors::kGround);
    dl->PushClipRect(lo, hi, true);
    const Hunter::Status& hs = mv.hs;
    const bool same_map = !mv.stem.empty() && ui.cur.id.stem == mv.stem;

    // 2. The grid, with no file.
    if (mv.page == nullptr) {
        double step = kGridPx;
        while (step * ts.z < 12.0) step *= 5.0;
        for (double x = std::floor(ts.vx0 / step) * step; x <= ts.vx1; x += step) {
            const ImVec2 a = ts.at(x, ts.vy0);
            dl->AddLine(ImVec2(a.x, lo.y), ImVec2(a.x, hi.y), MapColors::kGrid, 1.0f);
        }
        for (double y = std::floor(ts.vy0 / step) * step; y <= ts.vy1; y += step) {
            const ImVec2 a = ts.at(ts.vx0, y);
            dl->AddLine(ImVec2(lo.x, a.y), ImVec2(hi.x, a.y), MapColors::kGrid, 1.0f);
        }
    }

    // 3. Where a drop is refused for a gate, while drops are on.
    const std::vector<MapRun>* marks = same_map && mv.page != nullptr && hunt_config_.drop_down && ui.cur.no_drop
                                           ? ui.cur.no_drop.get()
                                           : nullptr;
    if (marks != nullptr)
        for (const MapRun& r : *marks)
            if (ts.sees(r.x0, r.y0, r.x1, r.y1, 8.0)) stroke(dl, r.pts, ts, ui.scratch, MapColors::kNoDrop, 6.0f);

    // 4. The zone's fill: the draft under a drag, else the saved zone; this
    // run's spot beside it.
    const bool dragging = ui.gesture.has_value() && ui.draft.has_value();
    const HuntZone* shown = dragging ? &*ui.draft : mv.zone ? &*mv.zone : nullptr;
    const HuntZone* spot = dragging && ui.gesture_spot ? nullptr : mv.spot_raw ? &*mv.spot_raw : nullptr;
    const bool lit = dragging || hot != ZoneGrip::None;
    const bool spot_lit = lit && !mv.zone;
    if (spot != nullptr) {
        const ZoneScreen zs = zone_screen(*spot, ui.cam, cam_p(o), cam_p(s));
        dl->AddRectFilled(ImVec2(static_cast<float>(zs.l), static_cast<float>(zs.t)),
                          ImVec2(static_cast<float>(zs.r), static_cast<float>(zs.b)),
                          spot_lit ? MapColors::kHotFill : MapColors::kSpotFill);
    }
    if (shown != nullptr) {
        const ZoneScreen zs = zone_screen(*shown, ui.cam, cam_p(o), cam_p(s));
        dl->AddRectFilled(ImVec2(static_cast<float>(zs.l), static_cast<float>(zs.t)),
                          ImVec2(static_cast<float>(zs.r), static_cast<float>(zs.b)),
                          lit && (!ui.gesture_spot || dragging) ? MapColors::kHotFill : MapColors::kZoneFill);
    }

    // 5. The floors -- faded on a doubtful file -- then the zone's warnings.
    if (mv.page != nullptr && same_map && ui.cur.sketch) {
        const float fade = mv.u.file_doubtful ? 0.35f : 1.0f;
        for (const MapRun& r : ui.cur.sketch->runs) {
            if (!ts.sees(r.x0, r.y0, r.x1, r.y1, 4.0)) continue;
            ImU32 col = 0;
            float thick = 2.0f;
            floor_style(r.attr, col, thick);
            stroke(dl, r.pts, ts, ui.scratch, alpha_of(col, fade), thick);
        }
        if (mv.zone && !dragging && !ui.check_key.empty()) {
            for (const std::vector<MapPoint>& piece : ui.check.red)
                stroke(dl, piece, ts, ui.scratch, MapColors::kRed, 3.0f);
            for (const std::vector<MapPoint>& piece : ui.check.amber)
                stroke(dl, piece, ts, ui.scratch, MapColors::kAmber, 3.0f);
        }
    }

    // 5b. What jumps up learned this session (2026.11.05b), grey: the floors
    // found solid from below, and the points a key did not get up from.
    int no_jump_hover = -1;
    bool solid_hover = false;
    if (same_map && mv.page != nullptr) {
        for (const MapRun& r : ui.cur.solid_runs) {
            if (!ts.sees(r.x0, r.y0, r.x1, r.y1, 8.0)) continue;
            stroke(dl, r.pts, ts, ui.scratch, MapColors::kLearned, 5.0f);
            for (std::size_t i = 0; hovered && !solid_hover && i + 1 < r.pts.size(); ++i)
                solid_hover = to_segment(mouse, ts.at(r.pts[i].x, r.pts[i].y), ts.at(r.pts[i + 1].x, r.pts[i + 1].y)) <=
                              5.0f;
        }
        for (std::size_t i = 0; i < ui.cur.no_jumps.size(); ++i) {
            const GameMapBook::NoJump& n = ui.cur.no_jumps[i];
            if (!ts.sees(n.take.x, n.land.y, n.take.x, n.take.y, 12.0)) continue;
            const ImVec2 at = ts.at(n.take.x, n.take.y);
            dashed_line(dl, at, ts.at(n.take.x, n.land.y), MapColors::kLearned, 1.0f, lo, hi);
            dl->AddLine(ImVec2(at.x - 4.0f, at.y - 4.0f), ImVec2(at.x + 4.0f, at.y + 4.0f), MapColors::kLearned, 2.0f);
            dl->AddLine(ImVec2(at.x - 4.0f, at.y + 4.0f), ImVec2(at.x + 4.0f, at.y - 4.0f), MapColors::kLearned, 2.0f);
            if (hovered && std::hypot(mouse.x - at.x, mouse.y - at.y) <= kHoverPx) no_jump_hover = static_cast<int>(i);
        }
    }

    // 6. The gates: the .prj's, and those GoBot learned faded.
    int gate_hover = -1;
    if (same_map) {
        for (std::size_t i = 0; i < ui.cur.gates.size(); ++i) {
            const MapGate& g = ui.cur.gates[i];
            if (!ts.sees(g.x - 12, g.y - 40, g.x + 12, g.y, 8.0)) continue;
            const float a = g.kind == 0 ? 0.5f : 1.0f;
            const ImVec2 at = ts.at(g.x, g.y);
            const ImVec2 top = ts.at(g.x - 12, g.y - 40), bottom = ts.at(g.x + 12, g.y);
            dl->AddRectFilled(top, bottom, alpha_of(MapColors::kGateFill, a));
            dl->AddRect(top, bottom, alpha_of(MapColors::kGateLine, a), 0.0f, 1.0f);
            plus_mark(dl, at, 4.0f, alpha_of(MapColors::kGateMark, a));
            if (hovered && std::hypot(mouse.x - at.x, mouse.y - at.y) <= kHoverPx) gate_hover = static_cast<int>(i);
        }
    }

    // 7. The zone's border, strip, grips and home; this run's spot dashed.
    bool pin_hover = false;
    const bool drop_out = mv.hunt_on && hunt_config_.drop_down && hs.drop_wait == Hunter::DropWhy::OutsideArea &&
                          hs.drop_wait_ms != 0 && mv.now >= hs.drop_wait_ms && mv.now - hs.drop_wait_ms <= kDropFreshMs;
    const bool waits = mv.hunt_on && ((hs.spot_from_map && hs.spot_stem == mv.stem && hs.spot_suspended) ||
                                      (hs.zone_wait != Hunter::ZoneWait::None && hs.zone_wait_stem == mv.stem));
    if (spot != nullptr) {
        const ZoneScreen zs = zone_screen(*spot, ui.cam, cam_p(o), cam_p(s));
        const ImVec2 a(static_cast<float>(zs.l), static_cast<float>(zs.t));
        const ImVec2 c(static_cast<float>(zs.r), static_cast<float>(zs.b));
        const ImU32 line = spot_lit ? MapColors::kZoneHot : MapColors::kSpotLine;
        dashed_line(dl, a, ImVec2(c.x, a.y), line, 1.5f, lo, hi);
        dashed_line(dl, ImVec2(c.x, a.y), c, line, 1.5f, lo, hi);
        dashed_line(dl, c, ImVec2(a.x, c.y), line, 1.5f, lo, hi);
        dashed_line(dl, ImVec2(a.x, c.y), a, line, 1.5f, lo, hi);
        const char* tag = mv.zone ? "chỗ của lần chạy này"
                                  : "chỗ của lần chạy này — chưa lưu (kéo để lưu thành vùng)";
        small_text(dl, ImVec2(std::max(a.x, lo.x) + 4.0f, std::max(a.y, lo.y) + 2.0f), MapColors::kLabel, tag);
        const ImVec2 home(static_cast<float>(zs.home.x), static_cast<float>(zs.home.y));
        home_pin(dl, home, MapColors::kHome);
    }
    if (shown != nullptr) {
        const ZoneScreen zs = zone_screen(*shown, ui.cam, cam_p(o), cam_p(s));
        const ImVec2 a(static_cast<float>(zs.l), static_cast<float>(zs.t));
        const ImVec2 c(static_cast<float>(zs.r), static_cast<float>(zs.b));
        const ImU32 line = drop_out ? MapColors::kAmberLine : lit ? MapColors::kZoneHot : MapColors::kZoneLine;
        dl->AddRect(a, c, line, 0.0f, 2.0f);
        if (mv.limits.floors && (mv.zone || dragging)) {
            const float strip_b = std::min(c.y, a.y + static_cast<float>(kZoneStripPx));
            dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(c.x, strip_b), MapColors::kStrip);
            if (c.x - a.x > 70.0f) small_text(dl, ImVec2(std::max(a.x, lo.x) + 4.0f, a.y), MapColors::kLabel, "vùng đánh");
        }
        if (lit) {
            for (const ImVec2& k : { a, ImVec2(c.x, a.y), c, ImVec2(a.x, c.y) })
                dl->AddRectFilled(ImVec2(k.x - 3.0f, k.y - 3.0f), ImVec2(k.x + 3.0f, k.y + 3.0f), MapColors::kZoneHot);
        }
        const ImVec2 home(static_cast<float>(zs.home.x), static_cast<float>(zs.home.y));
        home_pin(dl, home, waits ? MapColors::kGrey : MapColors::kHome);
        pin_hover = hovered && !ui.gesture && std::hypot(mouse.x - zs.head.x, mouse.y - zs.head.y) <= kZonePinPx;
        if (dragging) {
            const std::string size = std::to_string(shown->width()) + " × " + std::to_string(shown->height());
            const ImVec2 ts_size = small_size(size.c_str());
            small_text(dl, ImVec2(std::min(c.x, hi.x) - ts_size.x - 4.0f, std::min(c.y, hi.y) - ts_size.y - 2.0f),
                       MapColors::kZoneHot, size.c_str());
        }
    }

    // 8. The monsters, each at its dot (mob_dots) -- a walker on its frame's
    // floor, a spawn set down on its own, the dead not at all
    // (kMapDeadShownMs) -- the dead fading out first, should they be drawn,
    // then the living, then the one being fought. None, nor their ring or
    // hover, with nothing to place them by (map_shows_mobs). Grey is judged
    // where the hunter has them, never at the dot (mob_grey).
    const bool area_on = mv.hunt_on && hunt_config_.spot && hs.spot_set && !hs.spot_suspended;
    const bool fought = mv.hunt_on && hs.has_target && hs.target.slot >= 0;
    const double scale = hunt_config_.packet_scale > 0.0f ? static_cast<double>(hunt_config_.packet_scale) : 1.0;
    std::size_t mob_hover = mv.dots.size();
    float mob_dist = kHoverPx;
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < mv.dots.size() && i < mv.v.mobs.size(); ++i) {
            const PacketWorld::Mob& m = mv.v.mobs[i];
            const MapDot& dot = mv.dots[i];
            if (m.dead != (pass == 0) || !dot.shown()) continue;
            if (!ts.sees(dot.x, dot.y, dot.x, dot.y, 12.0)) continue;
            const ImVec2 c = ts.at(dot.x, dot.y);
            const bool target =
                fought && m.slot == static_cast<std::uint32_t>(hs.target.slot) && m.gen == hs.target.gen;
            ImU32 col = MapColors::kMob;
            // Grey too: one the seek has no way to, or passes over a while
            // (2026.11.05b, seek_grey_of) -- never the one fought.
            if (m.dead) col = alpha_of(col, 0.35f);
            else if (mob_grey(hunt_config_, hs, area_on, mv.you, mv.you_x, mv.you_y, m, target) ||
                     (mv.hunt_on && !target && seek_grey_of(hs, m) != nullptr))
                col = MapColors::kGrey;
            else if (m.moving) col = alpha_of(col, 0.7f);
            dl->AddCircleFilled(c, 3.5f, col);
            if (!m.dead) dl->AddCircle(c, 4.5f, alpha_of(MapColors::kMobRing, col == MapColors::kGrey ? 0.5f : 1.0f), 0, 1.0f);
            if (!m.dead && m.max_hp_known && m.max_hp > 0 && ts.z >= 0.5) {
                const float f = std::clamp(static_cast<float>(m.hp) / static_cast<float>(m.max_hp), 0.0f, 1.0f);
                dl->AddRectFilled(ImVec2(c.x - 6.0f, c.y - 10.0f), ImVec2(c.x + 6.0f, c.y - 8.0f), MapColors::kHpBack);
                dl->AddRectFilled(ImVec2(c.x - 6.0f, c.y - 10.0f), ImVec2(c.x - 6.0f + 12.0f * f, c.y - 8.0f),
                                  MapColors::kHp);
            }
            const float d = std::hypot(mouse.x - c.x, mouse.y - c.y);
            if (hovered && d <= mob_dist) {
                mob_dist = d;
                mob_hover = i;
            }
        }
    }
    if (mv.mobs && mv.hunt_on && hs.has_target) {
        if (hs.target.slot >= 0) {
            for (std::size_t i = 0; i < mv.dots.size() && i < mv.v.mobs.size(); ++i) {
                const PacketWorld::Mob& m = mv.v.mobs[i];
                if (m.slot != static_cast<std::uint32_t>(hs.target.slot) || m.gen != hs.target.gen) continue;
                if (mv.dots[i].shown())
                    dl->AddCircle(ts.at(mv.dots[i].x, mv.dots[i].y), 7.0f, MapColors::kTarget, 0, 2.0f);
                break;
            }
        } else if (mv.you) {
            // A plate off the screen: laid back over the world from your dot
            // as the hunter lays it (packet_scale both ways, packet_dy on the
            // height) -- the screen's own distance from you, wherever the
            // packets' height has you.
            dl->AddCircle(ts.at(mv.you_at.x + hs.dx * scale, mv.you_at.y + (hs.dy - hunt_config_.packet_dy) * scale),
                          7.0f, MapColors::kTarget, 0, 2.0f);
        }
    }

    // 8b. The seek (2026.11.05b): a dashed line from you to the point it
    // fights the monster gone for from -- a small hollow square -- a thin line
    // on to that monster where the hunter places it, and a ring round it,
    // dashed when it is where a walk out of view was followed on to.
    bool attack_hover = false, sought_hover = false;
    if (mv.mobs && mv.hunt_on && hs.seek_on && mv.you && !mv.you_lost) {
        const ImVec2 me = ts.at(mv.you_at.x, mv.you_at.y);
        const ImVec2 mob = ts.at(hs.seek_mob_x, hs.seek_mob_y);
        if (hs.seek_planned) {
            const ImVec2 at = ts.at(hs.seek_at_x, hs.seek_at_y);
            dashed_line(dl, me, at, MapColors::kTarget, 1.5f, lo, hi);
            dl->AddRect(ImVec2(at.x - 3.5f, at.y - 3.5f), ImVec2(at.x + 3.5f, at.y + 3.5f), MapColors::kTarget, 0.0f, 0,
                        1.5f);
            dl->AddLine(at, mob, alpha_of(MapColors::kTarget, 0.6f), 1.0f);
            attack_hover = hovered && std::hypot(mouse.x - at.x, mouse.y - at.y) <= kHoverPx;
        } else {
            dashed_line(dl, me, mob, MapColors::kTarget, 1.5f, lo, hi);
        }
        if (hs.seek_projected) dashed_ring(dl, mob, 8.0f, MapColors::kTarget, 1.5f);
        else                   dl->AddCircle(mob, 8.0f, alpha_of(MapColors::kTarget, 0.7f), 0, 1.0f);
        sought_hover = hovered && !attack_hover && std::hypot(mouse.x - mob.x, mouse.y - mob.y) <= kHoverPx;
    }

    // 8c. The jump up: an arrow from its take-off to its landing, labelled
    // with its key -- dashed for one that would have gone with the tick off
    // or jumps up off for the run (jump_arrow).
    bool arrow_hover = false;
    std::optional<JumpArrow> jump = mv.hunt_on && same_map ? jump_arrow(hs, mv.now) : std::nullopt;
    if (jump) {
        const ImVec2 from = ts.at(jump->take.x, jump->take.y), to = ts.at(jump->land.x, jump->land.y);
        const ImU32 col = jump->would ? alpha_of(MapColors::kJump, 0.6f) : MapColors::kJump;
        arrow(dl, from, to, col, jump->would, lo, hi);
        const std::string key = std::string(jump_kind_key(jump->kind)) + (jump->would ? " (đang tắt)" : "");
        const ImVec2 w = small_size(key.c_str());
        small_text(dl, ImVec2(std::max(from.x, to.x) + 6.0f, (from.y + to.y) / 2.0f - w.y / 2.0f), col, key.c_str());
        arrow_hover = hovered && to_segment(mouse, from, to) <= 5.0f;
    }

    // 9. "Chi tiết", then you, at your dot (you_dot): on your floor while you
    // walk on the ground.
    if (mv.you) {
        const ImVec2 me = ts.at(mv.you_at.x, mv.you_at.y);
        if (ui.details && !mv.you_lost) {
            if (mv.fix.airborne && mv.page != nullptr) {
                if (const std::optional<MapFloor> below =
                        mv.page->floor_below(round_px(mv.you_at.x), round_px(mv.you_at.y), 0))
                    dashed_line(dl, me, ts.at(mv.you_at.x, below->y - 1), MapColors::kExtra, 1.0f, lo, hi);
            }
            if (std::hypot(mv.fix.rx - mv.you_at.x, mv.fix.ry - mv.you_at.y) > 8.0)
                dashed_ring(dl, ts.at(mv.fix.rx, mv.fix.ry), 5.0f, MapColors::kExtra, 1.0f);
            if (mv.hunt_on && hs.state == Hunter::State::Returning && hs.spot_set)
                dashed_line(dl, me, ts.at(hs.spot_x, hs.spot_y), MapColors::kHome, 1.0f, lo, hi);
            int cw = 0, ch = 0;
            if (input_.window_size(cw, ch) && cw > 0 && ch > 0) {
                const double hw = cw * PacketEyes::kBoxFraction * scale, hh = ch * PacketEyes::kBoxFraction * scale;
                // A plate is kept with its height plus packet_dy within the box:
                // in the world, the box's middle is packet_dy above you -- where
                // the packets have you, as the hunter's box is.
                const double cy = mv.you_y - hunt_config_.packet_dy * scale;
                dl->AddRect(ts.at(mv.you_x - hw, cy - hh), ts.at(mv.you_x + hw, cy + hh), MapColors::kExtra, 0.0f,
                            1.0f);
            }
        }
        const ImU32 body = mv.you_lost ? MapColors::kLost : MapColors::kYou;
        if (mv.fix.airborne && !mv.you_lost) dl->AddCircle(me, 4.5f, body, 0, 2.5f);
        else dl->AddCircleFilled(me, 5.0f, body);
        if (mv.fix.uncertain || mv.you_lost) dashed_ring(dl, me, 6.5f, MapColors::kGrey, 1.5f);
        else dl->AddCircle(me, 6.5f, MapColors::kWhite, 0, 1.5f);
        // Which way the character faces, as the packets last said: read only.
        const int face = mv.v.me.facing != 0 ? mv.v.me.facing : mv.fix.facing;
        if (face != 0 && !mv.you_lost) {
            const float sx = static_cast<float>(face);
            dl->AddTriangleFilled(ImVec2(me.x + sx * 11.0f, me.y), ImVec2(me.x + sx * 7.5f, me.y - 3.0f),
                                  ImVec2(me.x + sx * 7.5f, me.y + 3.0f), MapColors::kWhite);
        }
        // 10. Why it does not drop, over the character.
        if (mv.hunt_on && hunt_config_.drop_down && !mv.you_lost) {
            const std::string why = drop_label(hs, mv.now);
            if (!why.empty()) {
                const ImVec2 w = small_size(why.c_str());
                small_text(dl, ImVec2(me.x - w.x / 2.0f, me.y - 14.0f - w.y), MapColors::kAmberLine, why.c_str());
            }
        }
    }

    // 11. Nothing to place anything by: said in the middle.
    const char* empty = mv.shows == MapShows::PositionsOff ? "Vị trí từ gói tin đang tắt"
                        : mv.shows == MapShows::NoCapture  ? "Chưa có gói tin của game"
                        : mv.fix.state == WorldFix::State::FirstReport ? "Chờ gói tin báo vị trí nhân vật…"
                                                                       : nullptr;
    if (empty != nullptr) {
        const ImVec2 w = small_size(empty);
        small_text(dl, ImVec2(o.x + (s.x - w.x) / 2.0f, o.y + (s.y - w.y) / 2.0f), MapColors::kLabel, empty);
    }
    dl->PopClipRect();
    dl->AddRect(lo, hi, ImGui::GetColorU32(ImGuiCol_Border), 0.0f, 1.0f);

    // What the mouse is over.
    if (hovered && !ui.gesture && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        std::string tip;
        if (pin_hover) {
            tip = "Chỗ về: bot đi bộ về đây khi hết quái";
        } else if (attack_hover) {
            tip = "Chỗ đứng đánh: GoBot đi tới đây (" + std::to_string(hs.seek_at_x) + ", " +
                  std::to_string(hs.seek_at_y) + ") rồi đánh con quái có vòng";
        } else if (sought_hover) {
            tip = std::string(hs.seek_projected ? "Chỗ đoán: con quái đi khuất, GoBot đoán nó đi tiếp dọc bục tới đây"
                                                : "Con quái GoBot đang đi tìm") +
                  " · " + std::to_string(hs.seek_mob_x) + ", " + std::to_string(hs.seek_mob_y);
        } else if (mob_hover < mv.dots.size()) {
            const PacketWorld::Mob& m = mv.v.mobs[mob_hover];
            tip = m.dead ? "Quái (đã chết)" : "Quái";
            if (m.level > 0) tip += " · cấp " + std::to_string(m.level);
            if (!m.dead && m.max_hp_known && m.max_hp > 0)
                tip += " · máu " + thousands(m.hp) + "/" + thousands(m.max_hp) + " (" +
                       std::to_string(static_cast<unsigned long long>(m.hp) * 100ull / m.max_hp) + "%)";
            tip += " · " + place_vi(mv.dots[mob_hover], m.x, m.y);
            // What the seek makes of it (2026.11.05b).
            if (const Hunter::Status::SeekGrey* g = mv.hunt_on ? seek_grey_of(hs, m) : nullptr) {
                const std::string why = seek_grey_vi(*g);
                if (!why.empty()) tip += " · " + why;
            } else if (mv.hunt_on && hs.seek_on && hs.seek_slot >= 0 &&
                       m.slot == static_cast<std::uint32_t>(hs.seek_slot) && m.gen == hs.seek_gen) {
                tip += " · GoBot đang đi tìm con này";
            }
        } else if (arrow_hover && jump) {
            const std::string key = jump_kind_key(jump->kind);
            tip = jump->would ? "Lẽ ra GoBot đã nhảy " + key + " ở đây (x " + std::to_string(jump->take.x) +
                                    ") lên bục " + std::to_string(jump->land.y + 1) +
                                    " — Nhảy lên bục trên đang tắt, hoặc đã tắt tới lần BẮT ĐẦU sau"
                              : "GoBot sẽ nhảy " + key + " ở x " + std::to_string(jump->take.x) + " lên bục " +
                                    std::to_string(jump->land.y + 1);
        } else if (no_jump_hover >= 0) {
            const GameMapBook::NoJump& n = ui.cur.no_jumps[static_cast<std::size_t>(no_jump_hover)];
            const std::string key = jump_kind_key(static_cast<JumpKind>(std::min<int>(n.kind, kJumpKinds - 1)));
            tip = "Đã nhảy " + key + " hỏng ở đây: GoBot không nhảy " + key + (n.kind > 0 ? " (hay phím yếu hơn)" : "") +
                  " lên bục " + std::to_string(n.land.y + 1) + " trong vòng " +
                  std::to_string(GameMapBook::kNoJumpXPx) + " px quanh chỗ này, tới khi tắt GoBot";
        } else if (solid_hover) {
            tip = "Bục này không nhảy xuyên lên được (đã rơi lại " + std::to_string(Hunter::kJumpUpSolidAfter) +
                  " lần): GoBot không nhảy lên bục này tới khi tắt GoBot";
        } else if (gate_hover >= 0) {
            const MapGate& g = ui.cur.gates[static_cast<std::size_t>(gate_hover)];
            tip = (g.kind == 0 ? "Cửa GoBot đã thấy trong lần chạy này · " : "Cửa · ") + std::to_string(g.x) + ", " +
                  std::to_string(g.y);
        } else if (marks != nullptr) {
            bool on_mark = false;
            for (const MapRun& r : *marks) {
                if (on_mark || !ts.sees(r.x0, r.y0, r.x1, r.y1, kHoverPx)) continue;
                for (std::size_t i = 0; i + 1 < r.pts.size() && !on_mark; ++i)
                    on_mark = to_segment(mouse, ts.at(r.pts[i].x, r.pts[i].y),
                                         ts.at(r.pts[i + 1].x, r.pts[i + 1].y)) <= 5.0f;
                if (r.pts.size() == 1) {
                    const ImVec2 p = ts.at(r.pts[0].x, r.pts[0].y);
                    on_mark = std::hypot(mouse.x - p.x, mouse.y - p.y) <= 5.0f;
                }
            }
            if (on_mark)
                tip = "Không nhảy xuống từ đoạn này: có cửa cách không quá " + std::to_string(hunt_config_.drop_gate_px) +
                      " px nằm trong đường rơi (một trong các điều kiện của Nhảy xuống).";
        }
        if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
    }
}

void GuiApp::map_drag(MapUi& ui, const MapView& mv) {
    // A frame of the drag: the draft through the camera frozen at the press,
    // nothing else -- the release decides. Under ImGui's drag threshold it is
    // the zone as it was, as the release would keep it.
    const ImVec2 m = ImGui::GetIO().MousePos;
    ui.draft = ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left) ? gesture_draft(*ui.gesture, cam_p(m), mv.limits)
                                                                      : ui.gesture->from;
    ImGui::SetMouseCursor(grip_cursor(ui.gesture->grip));
}

void GuiApp::map_release(MapUi& ui, const MapView& mv) {
    const ZoneGesture g = *ui.gesture;
    const bool spot = ui.gesture_spot;
    const std::string stem = ui.gesture_stem;
    ui.gesture.reset();
    ui.draft.reset();
    // A click, or a slip under ImGui's drag threshold: nothing saved.
    if (!ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left)) return;
    // Only on the map it began on, while it may still be edited.
    if (mv.edit != MapEdit::Ok || stem.empty() || stem != mv.stem) return;
    const ImVec2 m = ImGui::GetIO().MousePos;
    HuntZone next = gesture_draft(g, cam_p(m), mv.limits);
    // The home put on a floor, where the pin or the whole box was moved.
    if ((g.grip == ZoneGrip::Home || g.grip == ZoneGrip::Strip) && mv.limits.floors && mv.page != nullptr &&
        !same_place(next, g.from)) {
        const std::optional<MapPoint> on = snap_home(*mv.page, next.home_x, next.home_y, &next);
        if (!on) {
            map_note(ui, kHomeRefused);
            return;
        }
        next.home_x = on->x;
        next.home_y = on->y;
    }
    // A click, or a drag back to where it began: nothing saved, nothing to undo.
    if (same_place(next, g.from)) return;
    const std::span<const MapRun> marks =
        ui.cur.no_drop ? std::span<const MapRun>(*ui.cur.no_drop) : std::span<const MapRun>();
    const ZoneCheck c = zone_check(mv.page, next, mv.limits, marks, hunt_config_.drop_down);
    if (c.refuse_vi != nullptr) {
        map_note(ui, c.refuse_vi);
        return;
    }
    map_commit_zone(ui, stem, next, spot ? MapCommit::FromSpot : MapCommit::Edit);
    map_red_note(ui, stem, c, false);
}

void GuiApp::map_menu(MapUi& ui, const MapView& mv) {
    if (!ImGui::BeginPopup(kMenu)) return;
    // Opened on another map, or before this one was known: its point is not this map's pixels.
    if (mv.stem != ui.menu_stem) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const bool can = mv.edit == MapEdit::Ok;
    const char* const why = edit_refusal_vi(mv.edit);
    const MapPoint at = ui.menu_at;
    if (mv.zone && mv.zone->contains(at.x, at.y) && mv.limits.floors) {
        if (ImGui::MenuItem("Đặt chỗ về ở đây", nullptr, false, can)) {
            HuntZone next = *mv.zone;
            const std::optional<MapPoint> on =
                mv.page != nullptr ? snap_home(*mv.page, at.x, at.y, &next) : std::optional<MapPoint>();
            if (!on) {
                map_note(ui, kHomeRefused);
            } else {
                next.home_x = on->x;
                next.home_y = on->y;
                if (!same_place(next, *mv.zone)) map_try(ui, mv, next, MapCommit::Edit);
            }
        }
        if (!can) ImGui::SetItemTooltip("%s", why);
    }
    const bool here_ok = can && mv.limits.floors;   // no floors: the home only where the character stands
    if (ImGui::MenuItem(mv.zone ? "Dời vùng tới đây" : "Tạo vùng ở đây", nullptr, false, here_ok)) {
        if (mv.zone)
            map_try(ui, mv, zone_moved(mv.page, *mv.zone, at, mv.limits), MapCommit::Moved);
        else
            map_try(ui, mv, zone_around(mv.page, at, hunt_config_.spot_half_w, hunt_config_.spot_half_h, mv.limits),
                    MapCommit::New);
    }
    if (!can)
        ImGui::SetItemTooltip("%s", why);
    else if (!mv.limits.floors)
        ImGui::SetItemTooltip("%s", mv.zone ? "Không có file bản đồ khớp để đặt chỗ về lên bục — bấm “Dời vùng tới "
                                              "nhân vật”."
                                            : "Không có file bản đồ khớp để đặt chỗ về lên bục — bấm “Tạo vùng quanh "
                                              "nhân vật”.");
    ImGui::Separator();
    if (ImGui::MenuItem("Theo nhân vật")) ui.mode = MapUi::Mode::Follow;
    if (ImGui::MenuItem("Xem cả bản đồ")) ui.mode = MapUi::Mode::Fit;
    ImGui::Separator();
    const std::string xy = std::to_string(at.x) + ", " + std::to_string(at.y);
    if (ImGui::MenuItem(("Chép toạ độ " + xy).c_str())) ImGui::SetClipboardText(xy.c_str());
    ImGui::EndPopup();
}

// --------------------------------------------------------------- the card

void GuiApp::map_card(MapUi& ui, const MapView& mv) {
    if (!begin_card("##zone")) {
        end_card();
        return;
    }
    heading(strong_font_, "Vùng đánh quái của bản đồ này");
    if (ImGui::Checkbox("Chỗ đánh quái", &hunt_config_.spot)) simple_changed();
    ImGui::SetItemTooltip("%s", trial_hint(kTrialMap, "Cùng dấu với Chỗ đánh quái ở tab Đánh quái. Có dấu: bản đồ có "
                                                      "vùng đã lưu thì bot dùng vùng đó, chưa có thì lấy chỗ nhân vật "
                                                      "đứng khi bắt đầu như trước.")
                                    .c_str());

    // What the hunter does with the zone.
    const Hunter::Status& hs = mv.hs;
    ZoneCardIn in;
    in.hunt_enabled = hunt_config_.enabled;
    in.spot = hunt_config_.spot;
    in.hunting = mv.hunt_on;
    in.book_loaded = ui.book_loaded;
    in.book_bad = ui.book_bad;
    in.book_any = ui.book.size() > 0;
    in.stem = mv.stem;
    in.zone = mv.zone.has_value();
    in.send = mv.zone ? zone_send(mv.u, *mv.zone, mv.page, ui.have_settled, ui.last_settled_epoch, ui.last_settled)
                      : ZoneSend::NoMap;
    switch (zone_card(in, hs)) {
        case ZoneCard::HuntOff:
            hint("Đánh dấu Tự đánh quái ở tab Đánh quái để dùng vùng.");
            break;
        case ZoneCard::BookBad:
            coloured(kBad, "Không đọc được configs\\spots.ini — không sửa được vùng, để khỏi ghi đè file. Kiểm tra "
                           "file rồi mở lại GoBot.");
            break;
        case ZoneCard::BookLoading:
            hint("Đang đọc vùng đã lưu…");
            break;
        case ZoneCard::MapUnknown:
            // A zone's spot set aside meanwhile: the bot stands still, as the hunter's own line says.
            if (hs.spot_from_map && hs.spot_suspended)
                simple_spot_state(hs);
            else
                coloured(kWait, "Chưa biết bản đồ nên chưa dùng vùng đã lưu — bot đánh ở chỗ nhân vật đứng khi bắt "
                                "đầu, như trước.");
            break;
        case ZoneCard::NoMap:
            hint("Chờ GoBot nhận ra bản đồ để đặt vùng đánh.");
            break;
        case ZoneCard::Empty:
            hint("Bản đồ này chưa có vùng đánh. Bấm “Tạo vùng quanh nhân vật”, hoặc chuột phải trên bản đồ → “Tạo "
                 "vùng ở đây”.");
            break;
        case ZoneCard::SessionSpot:
            simple_spot_state(hs);
            hint("Chỗ này chỉ dùng trong lần chạy này. Bấm “Lưu vùng này cho bản đồ” hoặc kéo ô của nó để lần sau tự "
                 "dùng.");
            break;
        case ZoneCard::SpotOff:
            coloured(kWait, "Chỗ đánh quái đang tắt — vùng đã lưu chưa dùng. Đánh dấu Chỗ đánh quái để dùng.");
            break;
        case ZoneCard::NotHunting:
            hint("Bấm BẮT ĐẦU: bot chỉ đi tới quái trong vùng này, hết quái thì đi bộ về chỗ về.");
            break;
        case ZoneCard::InUse:
            if (hs.spot_why == Hunter::SpotWhy::None)
                coloured(kGood, ("Đang dùng vùng này: " + spot_offset_vi(hs) + ".").c_str());
            else
                simple_spot_state(hs);
            break;
        case ZoneCard::OutsideArea:
            // Out of the box, by the hunter's own count: how far, and the way
            // back it walks -- first, or once nothing is left to fight.
            coloured(kWait, spot_outside_vi(hs).c_str());
            hint(("Vùng này: " + spot_offset_vi(hs) + ".").c_str());
            break;
        case ZoneCard::Suspended:
            // Set aside by the tab itself (a capture started again or switched off, a name let go of): the
            // hunter's line says so.
            if (hs.spot_map_aside)
                simple_spot_state(hs);
            else
                coloured(kWait, ("Chỗ đánh quái tạm dừng: đã sang bản đồ khác hoặc vào lại game — bot đứng yên tới "
                                 "khi nhân vật về lại vùng của bản đồ " +
                                 mv.name + ".")
                                    .c_str());
            break;
        case ZoneCard::Outside:
            coloured(kWait, ("Nhân vật đang ở ngoài vùng đã lưu của bản đồ này (" + std::to_string(hs.zone_wait_px) +
                             " px) — đánh ở chỗ đang đứng. Đưa nhân vật vào vùng (hoặc tới cách vùng không quá " +
                             std::to_string(kZoneTakePx) + " px), hoặc bấm “Dời vùng tới nhân vật”.")
                                .c_str());
            break;
        case ZoneCard::Waiting:
            hint("Chờ nhân vật đứng yên trên mặt đất để dùng vùng đã lưu.");
            break;
        case ZoneCard::DoesNotFit:
            coloured(kWait, "Vùng đã lưu không nằm trên bục nào của file bản đồ này — kéo chỗ về lên một bục, hoặc "
                            "tạo lại vùng.");
            break;
        case ZoneCard::Other:
            simple_spot_state(hs);
            break;
    }
    if (ui.save_failed) coloured(kBad, "Không lưu được vùng đánh vào configs\\spots.ini.");

    // The zone's numbers, and what it warns of.
    if (mv.zone) {
        hint(("Vùng: " + zone_vi(*mv.zone)).c_str());
        if (!mv.zone->saved.empty())
            ImGui::SetItemTooltip("%s", ("Lưu lúc " + mv.zone->saved + " trong configs\\spots.ini.").c_str());
        if (!ui.check_key.empty()) {
            const ZoneCheck& c = ui.check;
            const std::string jump = std::to_string(Hunter::kOneJumpPx);
            const std::string red_tip = "Đứng ở bục thấp hơn chỗ về quá " + jump +
                                        " px thì chỗ về cao hơn một lần nhảy: bot không đi về. Chỗ về đặt ở bục thấp "
                                        "nhất muốn đánh, rồi kéo vùng lên trên: bục cao hơn thì bot về được bằng cách "
                                        "đi ra mép bục rồi rơi xuống.";
            // Much of the box red, at every edit (zone_red_is_bad): in red.
            // On 2026-11-04 the owner's box had 1068 of 1814 floor columns
            // red, and the bot left the home's ledge for good.
            if (c.red_bad) {
                coloured(kBad, red_bad_vi(c).c_str());
                ImGui::SetItemTooltip("%s", red_tip.c_str());
            } else if (c.below_unreachable) {
                coloured(kWait, "Có bục bot không tự đi về được (tô đỏ).");
                ImGui::SetItemTooltip("%s", red_tip.c_str());
            }
            // A home by a gate: warned of, drops on or off -- never refused.
            if (c.home_by_gate) {
                coloured(kWait, ("Chỗ về sát cửa (" + std::to_string(c.home_gate_px) +
                                 " px): thường là chỗ vào bản đồ, ít quái" +
                                 (hunt_config_.drop_down ? ", và quanh đó bot không nhảy xuống" : "") +
                                 ". Kéo ghim cam ra chỗ có quái.")
                                    .c_str());
                ImGui::SetItemTooltip("Không phải lỗi: đi bộ về chỗ về thì bot không bấm ↓.");
            }
            if (c.home_slope_low)
                coloured(kWait, ("Phần dốc tô đỏ thấp hơn chỗ về quá " + jump + " px: từ đó bot không tự đi về.").c_str());
            if (c.above_by_edge) {
                coloured(kWait, "Có bục cao hơn chỗ về (tô vàng).");
                ImGui::SetItemTooltip("Từ đó bot chỉ về được nếu đi ra mép bục rồi rơi xuống (tuỳ bục). Bot không bao "
                                      "giờ bấm ↓ để về.");
            }
            if (c.gate_near) {
                coloured(kWait, "Có cửa trong hoặc gần vùng.");
                ImGui::SetItemTooltip("%s", ("Bot không nhảy xuống ở các đoạn tô đỏ nhạt (cửa cách không quá " +
                                             std::to_string(hunt_config_.drop_gate_px) +
                                             " px nằm trong đường rơi). Đi ngang qua cửa thì không sao: bot không bấm "
                                             "↓ ở cửa.")
                                                .c_str());
            }
            if (c.too_short_for_drops) {
                coloured(kWait, ("Vùng chỉ cao " + std::to_string(mv.zone->height()) +
                                 " px: bot gần như sẽ không nhảy xuống.")
                                    .c_str());
                ImGui::SetItemTooltip("Hầu như không lần nhảy xuống nào nằm trọn trong vùng (chỗ đứng, chỗ rơi và con "
                                      "quái đều phải ở trong vùng). Kéo cạnh dưới xuống thấp hơn.");
            }
        }
    } else if (mv.spot_raw) {
        hint(("Chỗ của lần chạy này: " + zone_vi(*mv.spot_raw)).c_str());
    }

    // ---- the buttons ----
    const bool can = mv.edit == MapEdit::Ok;
    const char* const why = edit_refusal_vi(mv.edit);
    const bool settled = mv.fix.settled();
    bool first = true;
    const auto next_to = [&first](const char* text) {
        if (!first) {
            const float w = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
        }
        first = false;
    };
    const char* const to_you = mv.zone ? "Dời vùng tới nhân vật" : "Tạo vùng quanh nhân vật";
    next_to(to_you);
    ImGui::BeginDisabled(!can || !settled);
    if (ImGui::Button(to_you)) {
        const MapPoint at{ round_px(mv.fix.rx), round_px(mv.fix.ry) };
        if (!zone_here_ok(mv.u, mv.page, at))
            map_note(ui, kNotSureHere);
        else if (mv.zone)
            map_try(ui, mv, zone_moved(mv.page, *mv.zone, at, mv.limits), MapCommit::Moved);
        else
            map_try(ui, mv, zone_around(mv.page, at, hunt_config_.spot_half_w, hunt_config_.spot_half_h, mv.limits),
                    MapCommit::New, true);
    }
    ImGui::EndDisabled();
    {
        const std::string tip =
            !can       ? std::string(why)
            : !settled ? std::string("Chờ nhân vật đứng yên trên mặt đất")
            : mv.zone  ? std::string("Dời vùng tới chỗ nhân vật đứng, giữ nguyên cỡ, rồi lưu.")
                       : "Tạo vùng ngang ±" + std::to_string(hunt_config_.spot_half_w) + ", dọc ±" +
                             std::to_string(hunt_config_.spot_half_h) +
                             " px quanh chỗ nhân vật đứng (cỡ ở nút Tầm đánh…), rồi lưu cho bản đồ này.";
        ImGui::SetItemTooltip("%s", trial_hint(kTrialMap, tip.c_str()).c_str());
    }
    if (mv.spot_raw && !mv.zone && !mv.stem.empty() && hs.spot_epoch == mv.u.epoch) {
        // Where the character stood: on a named map, held to it as the hand-off is.
        const bool keep_ok = spot_keep_ok(mv.u);
        next_to("Lưu vùng này cho bản đồ");
        ImGui::BeginDisabled(!can || !keep_ok);
        if (ImGui::Button("Lưu vùng này cho bản đồ")) map_try(ui, mv, mv.spot, MapCommit::FromSpot);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", !can       ? why
                                    : !keep_ok ? kNotSureHere
                                               : "Giữ chỗ của lần chạy này làm vùng đánh của bản đồ: lần sau tự dùng.");
    }
    if (ui.undo.set && ui.undo.stem == mv.stem) {
        next_to("Hoàn tác");
        ImGui::BeginDisabled(!can);
        if (ImGui::Button("Hoàn tác")) {
            const std::string stem = ui.undo.stem;
            const std::optional<HuntZone> before = ui.undo.before;
            map_commit_zone(ui, stem, before, MapCommit::Undo);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", can ? "Đưa vùng về như trước lần sửa vừa rồi (cũng lưu luôn)." : why);
    }
    if (mv.zone) {
        next_to("Xoá vùng");
        ImGui::BeginDisabled(!can);
        if (ImGui::Button("Xoá vùng")) {
            ui.ask_delete = true;
            ui.delete_stem = mv.stem;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", can ? "Xoá vùng đánh của bản đồ này (có hỏi lại)." : why);
    }

    // Why it does not drop, while it hunts with drops on.
    if (mv.hunt_on && hunt_config_.drop_down) {
        bool off = false;
        const std::string drop = drop_wait_vi(hs, !mv.stem.empty(), &off);
        if (!drop.empty()) {
            if (off) coloured(kWait, drop.c_str());
            else     hint(drop.c_str());
        }
    }
    if (!ui.note.empty() && mv.now < ui.note_until) {
        if (ui.note_bad) coloured(kBad, ui.note.c_str());
        else             hint(ui.note.c_str());
    }

    // "Xoá vùng" asks first.
    if (ui.ask_delete) {
        ui.ask_delete = false;
        ImGui::OpenPopup(kDeleteTitle);
    }
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kDeleteTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", ("Xoá vùng đánh của bản đồ " + map_name_of(ui.delete_stem) +
                                  "? Chỗ đánh quái sẽ lấy chỗ nhân vật đứng như trước.")
                                     .c_str());
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Xoá")) {
            // Still the map it was asked for, still editable.
            if (ui.delete_stem == mv.stem && can && ui.book.get(ui.delete_stem)) {
                const std::string stem = ui.delete_stem;
                map_commit_zone(ui, stem, std::nullopt, MapCommit::Delete);
            } else {
                map_note(ui, "Bản đồ vừa đổi: chưa xoá vùng nào.");
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Không")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    end_card();
}

} // namespace gobot
