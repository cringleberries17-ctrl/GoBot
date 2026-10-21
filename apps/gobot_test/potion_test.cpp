// gobot_test -- potion stock: the counts on the hotbar, read off frames saved
// from the live client, and when they are worth a message.
//
// The frames are the ones the live tests saved: frame.png beside this test,
// as the package puts it, and in the main checkout's build the 1024 x 768
// playerlist.png, the r1-r4 rounds and the trash cleaner's debug frames. A
// test is skipped only when its frame is not there. Nothing is pressed
// anywhere, and no game is needed.

#include "gobot/potion_stock.hpp"
#include "gobot/alerts.hpp"
#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
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

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// The main checkout's build, where the live tests saved their frames.
const char* const kLiveBuild = "C:\\Users\\player\\source\\GoBot\\build\\";

/// A frame saved from the live client: beside this test first, as the
/// package puts frame.png, then the main checkout's build.
bool live_frame(const std::string& rel, Frame& out) {
    const std::string name = rel.substr(rel.find_last_of('\\') + 1);
    for (const std::string& p : { exe_relative(name), std::string(kLiveBuild) + rel })
        if (exists(p) && load_png(p, out) && out.valid()) return true;
    return false;
}

std::vector<std::string> pngs_in(const std::string& dir, const std::string& pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(dir + fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

/// What the hotbar tests read with: the digits, and the hotbar's picture --
/// cut from frame.png the way the one that ships was cut.
struct Eyes {
    CountReader digits;
    Frame       picture;
    Frame       frame;      ///< frame.png itself
    bool        ok = false;
};

const Eyes& eyes() {
    static Eyes e = [] {
        Eyes x;
        x.digits.load(GlyphFont::default_dir());
        if (live_frame("bin\\Release\\frame.png", x.frame) && x.frame.width == 1280 && x.frame.height == 720)
            x.picture = hotbar_picture_from(x.frame, 475, 631);
        x.ok = x.digits.loaded() && x.picture.valid();
        return x;
    }();
    return e;
}

/// One frame's reading of `keys`, the hotbar found the way the window finds it.
std::vector<SlotReading> read_keys(const Frame& f, const std::vector<std::uint32_t>& keys, HotbarSpot* spot) {
    const HotbarSpot bar = find_hotbar(f, eyes().picture);
    if (spot) *spot = bar;
    std::vector<SlotReading> out;
    for (const std::uint32_t vk : keys) out.push_back(read_slot(f, bar, vk, eyes().digits, eyes().picture));
    return out;
}

std::string said(const SlotReading& r) {
    return r.state == SlotState::Count ? std::to_string(r.count) : slot_state_name(r.state);
}

/// How far the client prints a digit after the one before: its digits are 5
/// pixels wide and 6 apart, a 1 included.
constexpr int kDigitStep = 6;

/// The digits as the game prints them -- white, with a dark outline -- drawn
/// into `f` with their top-left at `x`, `y`.
void draw_number(Frame& f, int x, int y, const std::string& digits) {
    const std::string dir = GlyphFont::default_dir() + "\\lower\\";
    for (const char c : digits) {
        Frame g;
        if (!load_png(dir + c + ".png", g) || !g.valid()) return;
        const auto set = [&](int px, int py, std::uint8_t v) {
            if (px < 0 || py < 0 || px >= f.width || py >= f.height) return;
            std::uint8_t* p = &f.pixels[(static_cast<std::size_t>(py) * f.width + px) * 4];
            p[0] = p[1] = p[2] = v;
        };
        const auto lit = [&](int gx, int gy) {
            if (gx < 0 || gy < 0 || gx >= g.width || gy >= g.height) return false;
            const std::uint8_t* p = &g.pixels[(static_cast<std::size_t>(gy) * g.width + gx) * 4];
            return (p[0] + p[1] + p[2]) / 3 > 128;
        };
        for (int gy = -1; gy <= g.height; ++gy)
            for (int gx = -1; gx <= g.width; ++gx) {
                if (lit(gx, gy)) { set(x + gx, y + gy, 255); continue; }
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; ++dy)
                    for (int dx = -1; dx <= 1 && !edge; ++dx) edge = lit(gx + dx, gy + dy);
                if (edge) set(x + gx, y + gy, 0);
            }
        x += kDigitStep;
    }
}

void copy_rect(const Frame& from, int fx, int fy, Frame& to, int tx, int ty, int w, int h) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            std::copy_n(&from.pixels[(static_cast<std::size_t>(fy + y) * from.width + fx + x) * 4], 4,
                        &to.pixels[(static_cast<std::size_t>(ty + y) * to.width + tx + x) * 4]);
}

/// The digits as the client draws them over an icon, by its own font: lit
/// pixels white; the outline round them a soft darkening of what is under it,
/// a fifth of its colour, and so is the rest of a 1's 5-pixel cell; 6 pixels
/// from digit to digit. What the two live misreads look like on the saved
/// frames -- draw_number's hard black ring does not reproduce them.
void draw_game_number(Frame& f, int x, int y, const std::string& digits) {
    const std::string dir = GlyphFont::default_dir() + "\\lower\\";
    for (const char c : digits) {
        Frame g;
        if (!load_png(dir + c + ".png", g) || !g.valid()) return;
        const auto lit = [&](int gx, int gy) {
            if (gx < 0 || gy < 0 || gx >= g.width || gy >= g.height) return false;
            const std::uint8_t* p = &g.pixels[(static_cast<std::size_t>(gy) * g.width + gx) * 4];
            return (p[0] + p[1] + p[2]) / 3 > 128;
        };
        const auto at = [&](int px, int py) -> std::uint8_t* {
            if (px < 0 || py < 0 || px >= f.width || py >= f.height) return nullptr;
            return &f.pixels[(static_cast<std::size_t>(py) * f.width + px) * 4];
        };
        for (int gy = -1; gy <= g.height; ++gy)
            for (int gx = -1; gx <= 5; ++gx) {
                if (lit(gx, gy)) continue;
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; ++dy)
                    for (int dx = -1; dx <= 1 && !edge; ++dx) edge = lit(gx + dx, gy + dy);
                if (!edge && g.width < 5 && gx >= g.width && gx < 5 && gy >= 0 && gy < g.height) edge = true;
                if (std::uint8_t* p = edge ? at(x + gx, y + gy) : nullptr)
                    for (int k = 0; k < 3; ++k) p[k] = static_cast<std::uint8_t>(p[k] / 5);
            }
        for (int gy = 0; gy < g.height; ++gy)
            for (int gx = 0; gx < g.width; ++gx)
                if (std::uint8_t* p = lit(gx, gy) ? at(x + gx, y + gy) : nullptr) p[0] = p[1] = p[2] = 255;
        x += kDigitStep;
    }
}

// ---------------------------------------------------------------- the frames

void test_picture() {
    section("Potions: the hotbar's picture");
    const Eyes& e = eyes();
    if (!e.ok) {
        std::printf("  [skip] no frame.png from the live client, or no digits, here\n");
        return;
    }
    int opaque = 0, labels = 0;
    for (std::size_t i = 3; i < e.picture.pixels.size(); i += 4) {
        opaque += e.picture.pixels[i] == 255 ? 1 : 0;
        labels += e.picture.pixels[i] == hotbar::kLabelAlpha ? 1 : 0;
    }
    check(e.picture.width == hotbar::kPictureW && e.picture.height == hotbar::kPictureH && opaque > 6000 &&
              labels > 400,
          fmt("cut from frame.png: %dx%d, %d pixels of frame, %d of key labels", e.picture.width,
              e.picture.height, opaque, labels));

    // The one that ships is that same cut.
    Frame staged;
    std::string from;
    if (TemplateStore::load_one(hotbar::kPicture, exe_relative("configs/templates.ini"), staged, &from)) {
        bool same = staged.width == e.picture.width && staged.height == e.picture.height;
        for (std::size_t i = 0; same && i < staged.pixels.size(); i += 4) {
            const std::uint8_t a = staged.pixels[i + 3];
            same = a == e.picture.pixels[i + 3] &&
                   (a == 0 || std::equal(&staged.pixels[i], &staged.pixels[i] + 3, &e.picture.pixels[i]));
        }
        check(same, "the staged grabbed/hotbar_frame.png is that cut, pixel for pixel (" + from + ")");
    } else {
        std::printf("  [skip] no grabbed/hotbar_frame.png staged beside this test\n");
    }
#ifdef GOBOT_SOURCE_DIR
    const std::string names = read_all(std::string(GOBOT_SOURCE_DIR) + "/configs/templates.ini");
    check(names.find("hotbar_frame                   = grabbed/hotbar_frame") != std::string::npos,
          "templates.ini names it, so that the package keeps it");
#endif

    // It is not found where it is not: frame.png with the bar painted over.
    Frame covered = e.frame;
    for (int y = 600; y < 720; ++y)
        for (int x = 400; x < 900; ++x) {
            std::uint8_t* p = &covered.pixels[(static_cast<std::size_t>(y) * covered.width + x) * 4];
            p[0] = static_cast<std::uint8_t>(30 + (x * 7 + y * 3) % 40);
            p[1] = p[2] = 40;
        }
    const HotbarSpot none = find_hotbar(covered, e.picture);
    check(!none.found, fmt("the bar painted over: not found (best %.3f)", none.score));

    // What a look costs: where it was, then the bottom of the screen. The
    // window takes it off the drawing thread either way.
    const auto t0 = std::chrono::steady_clock::now();
    const HotbarSpot full = find_hotbar(e.frame, e.picture);
    const auto t1 = std::chrono::steady_clock::now();
    const HotbarSpot again = find_hotbar(e.frame, e.picture, &full);
    const auto t2 = std::chrono::steady_clock::now();
    const auto ms = [](auto d) {
        return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(d).count()) / 1000.0;
    };
    std::printf("  [info] finding the hotbar: %.1f ms along the bottom, %.1f ms where it was\n",
                ms(t1 - t0), ms(t2 - t1));
    check(full.found && again.found && again.x == full.x && again.y == full.y,
          "found where it was, as along the bottom");
}

void test_picture_blank() {
    section("Potions: the hotbar's picture keeps nothing of the screen under its see-through pixels (round 8)");
    // A frame of loud colour, nothing like the bar's, with a label drawn into
    // each slot's corner -- a white stroke and a dark ring round it -- so
    // that the picture has labels to keep.
    Frame f;
    f.width = 1280;
    f.height = 720;
    f.pixels.assign(static_cast<std::size_t>(f.width) * f.height * 4, 255);
    const auto px = [&](int x, int y) { return &f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4]; };
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            std::uint8_t* p = px(x, y);
            p[2] = static_cast<std::uint8_t>(200 + x % 50);    // red
            p[1] = static_cast<std::uint8_t>(30 + y % 90);     // green
            p[0] = 90;                                         // blue
        }
    constexpr int kSlotX = 475, kSlotY = 631;
    int label_px = 0;
    for (int row = 0; row < hotbar::kRows; ++row)
        for (int col = 0; col < hotbar::kCols; ++col) {
            const int sx = kSlotX + col * hotbar::kPitch, sy = kSlotY + row * hotbar::kPitch;
            for (int y = 23; y <= 29; ++y)
                for (int x = 26; x <= 30; ++x) {
                    const bool stroke = x >= 27 && x <= 29 && y >= 24 && y <= 28;
                    std::uint8_t* p = px(sx + x, sy + y);
                    p[0] = p[1] = p[2] = stroke ? 250 : 20;
                    ++label_px;
                }
        }
    const Frame pic = hotbar_picture_from(f, kSlotX, kSlotY);
    check(pic.width == hotbar::kPictureW && pic.height == hotbar::kPictureH, "the picture is cut");
    if (!pic.valid()) return;
    int clear = 0, coloured_clear = 0, labels = 0, frame_px = 0, wrong_rgb = 0, wrong_alpha = 0;
    const int x0 = kSlotX - hotbar::kSlotDx, y0 = kSlotY - hotbar::kSlotDy;
    for (int y = 0; y < pic.height; ++y)
        for (int x = 0; x < pic.width; ++x) {
            const std::uint8_t* q = &pic.pixels[(static_cast<std::size_t>(y) * pic.width + x) * 4];
            const std::uint8_t* s = px(x0 + x, y0 + y);
            // What the alpha is to be: the rounded corners and the slots
            // see-through, the labels at kLabelAlpha, the rest the frame.
            const bool corner = (x < 3 || x >= pic.width - 3) && (y < 3 || y >= pic.height - 3);
            const int sx = x - hotbar::kSlotDx, sy = y - hotbar::kSlotDy;
            const bool in_slot = sx >= 0 && sy >= 0 && sx % hotbar::kPitch < hotbar::kCell &&
                                 sy % hotbar::kPitch < hotbar::kCell && sx / hotbar::kPitch < hotbar::kCols &&
                                 sy / hotbar::kPitch < hotbar::kRows;
            const int lx = in_slot ? sx % hotbar::kPitch : -1, ly = in_slot ? sy % hotbar::kPitch : -1;
            const bool label = in_slot && lx >= 26 && lx <= 30 && ly >= 23 && ly <= 29;
            const std::uint8_t want = corner || (in_slot && !label) ? 0 : label ? hotbar::kLabelAlpha : 255;
            wrong_alpha += q[3] != want ? 1 : 0;
            if (q[3] == 0) {
                ++clear;
                coloured_clear += q[0] || q[1] || q[2] ? 1 : 0;
            } else {
                labels += q[3] == hotbar::kLabelAlpha ? 1 : 0;
                frame_px += q[3] == 255 ? 1 : 0;
                wrong_rgb += std::equal(q, q + 3, s) ? 0 : 1;
            }
        }
    const int slots = hotbar::kRows * hotbar::kCols * hotbar::kCell * hotbar::kCell;
    check(wrong_alpha == 0 && labels == label_px,
          fmt("its alpha as ever: the corners and slots see-through, %d label pixels at %d (%d wrong)", labels,
              hotbar::kLabelAlpha, wrong_alpha));
    check(coloured_clear == 0 && clear == 36 + slots - label_px,
          fmt("every see-through pixel black -- %d of them, the 36 of the corners and the slots' but the "
              "labels' -- %d with colour",
              clear, coloured_clear));
    check(wrong_rgb == 0 && frame_px > 6000,
          fmt("the frame round the slots and the labels keep the screen's colours (%d of %d differ)", wrong_rgb,
              frame_px + labels));
    check(!hotbar_picture_from(f, 2, 2).valid() && !hotbar_picture_from(f, 1270, 700).valid(),
          "a frame the picture would not fit in gives none");
    // And the one that ships has nothing under its see-through pixels either.
    Frame staged;
    if (TemplateStore::load_one(hotbar::kPicture, exe_relative("configs/templates.ini"), staged)) {
        int coloured = 0;
        for (std::size_t i = 0; i + 3 < staged.pixels.size(); i += 4)
            coloured += staged.pixels[i + 3] == 0 && (staged.pixels[i] || staged.pixels[i + 1] || staged.pixels[i + 2])
                            ? 1
                            : 0;
        check(coloured == 0, fmt("the staged grabbed/hotbar_frame.png: %d see-through pixels with colour", coloured));
    } else {
        std::printf("  [skip] no grabbed/hotbar_frame.png staged beside this test\n");
    }
}

void test_frame_png() {
    section("Potions: frame.png (PS-A1)");
    const Eyes& e = eyes();
    if (!e.ok) {
        std::printf("  [skip] no frame.png from the live client, or no digits, here\n");
        return;
    }
    HotbarSpot bar;
    const auto r = read_keys(e.frame, { '1', '2', 'D', '3', 'Q', 'V' }, &bar);
    check(bar.found && std::abs(bar.x - 475) <= 2 && std::abs(bar.y - 631) <= 2 && bar.score >= 0.90f,
          fmt("the hotbar at %d,%d (%.3f)", bar.x, bar.y, bar.score));
    check(r[0].state == SlotState::Count && r[0].count == 89, "key 1: 89 -- " + said(r[0]));
    check(r[1].state == SlotState::Count && r[1].count == 94, "key 2: 94 -- " + said(r[1]));
    check(r[2].state == SlotState::Count && r[2].count == 88, "key D: 88 -- " + said(r[2]));
    check(r[3].state == SlotState::Empty, "key 3: empty -- " + said(r[3]));
    check(r[4].state == SlotState::NotOnBar, "key Q: not on the hotbar -- " + said(r[4]));
    check(r[5].state == SlotState::Single, "key V, a skill with nothing printed: one, no number -- " + said(r[5]));
    check(r[0].label >= 0.95f && r[1].label >= 0.95f, fmt("the labels 1 and 2 are theirs (%.2f, %.2f)",
                                                         r[0].label, r[1].label));
}

void test_1024() {
    section("Potions: 1024 x 768 (PS-A2)");
    const Eyes& e = eyes();
    Frame f;
    if (!e.ok || !live_frame("bin\\Release\\playerlist.png", f)) {
        std::printf("  [skip] no playerlist.png from the live client here\n");
        return;
    }
    HotbarSpot bar;
    const auto r = read_keys(f, { '1', '2' }, &bar);
    check(f.width == 1024 && f.height == 768 && bar.found && std::abs(bar.x - 419) <= 2 &&
              std::abs(bar.y - 679) <= 2,
          fmt("a %dx%d frame: the hotbar at %d,%d (%.3f)", f.width, f.height, bar.x, bar.y, bar.score));
    check(r[0].state == SlotState::Count && r[0].count == 95 && r[1].state == SlotState::Count &&
              r[1].count == 14,
          "key 1: 95, key 2: 14 -- " + said(r[0]) + ", " + said(r[1]));
}

void test_rounds() {
    section("Potions: the live rounds (PS-A3)");
    const Eyes& e = eyes();
    Frame f;
    if (!e.ok || !live_frame("live\\shots\\r3_B_000.png", f)) {
        std::printf("  [skip] no live round frames here\n");
        return;
    }
    const auto r = read_keys(f, { '1', '2' }, nullptr);
    check(r[0].count == 27 && r[1].count == 62 && r[0].state == SlotState::Count,
          "r3_B_000: key 1 shows 27 -- the stack in use; the bag held 614 -- and key 2 62");

    // Twenty more, spread over the four rounds.
    std::vector<std::string> rounds;
    for (const char* round : { "r1_", "r2_", "r3_", "r4_" })
        for (const char* part : { "A_", "B_", "C_" })
            for (const std::string& p :
                 pngs_in(std::string(kLiveBuild) + "live\\shots\\", std::string(round) + part + "*.png"))
                rounds.push_back(p);
    // Not the frames another window covers: those are the next test's.
    rounds.erase(std::remove_if(rounds.begin(), rounds.end(),
                                [](const std::string& p) {
                                    return p.find("r2_B_000") != std::string::npos ||
                                           p.find("r2_B_001") != std::string::npos;
                                }),
                 rounds.end());
    int read = 0, right = 0;
    std::string wrong;
    const std::size_t step = std::max<std::size_t>(1, rounds.size() / 20);
    for (std::size_t i = 0; i < rounds.size() && read < 20; i += step) {
        Frame g;
        if (!load_png(rounds[i], g)) continue;
        ++read;
        const auto k = read_keys(g, { '1', '2' }, nullptr);
        if (k[0].state == SlotState::Count && k[0].count == 27 && k[1].state == SlotState::Count &&
            k[1].count == 62)
            ++right;
        else if (wrong.empty())
            wrong = rounds[i].substr(rounds[i].find_last_of('\\') + 1) + ": " + said(k[0]) + "/" + said(k[1]);
    }
    check(read == 20 && right == read, fmt("%d of %d more round frames read 27 and 62", right, read) +
                                           (wrong.empty() ? std::string{} : " -- " + wrong));

    const auto debug = pngs_in(std::string(kLiveBuild) + "bin\\Release\\trash_debug\\", "20261009_*.png");
    int ok = 0;
    for (const std::string& p : debug) {
        Frame g;
        if (!load_png(p, g)) continue;
        const auto k = read_keys(g, { '1', '2' }, nullptr);
        ok += k[0].count == 89 && k[1].count == 86 && k[0].state == SlotState::Count ? 1 : 0;
    }
    if (debug.empty()) std::printf("  [skip] no trash_debug frames of 9 October here\n");
    else check(ok == static_cast<int>(debug.size()),
               fmt("%d of %zu trash_debug frames of 9 October read 89 and 86", ok, debug.size()));
}

void test_covered() {
    section("Potions: another window over the hotbar (PS-A4)");
    const Eyes& e = eyes();
    if (!e.ok) {
        std::printf("  [skip] no frame.png from the live client, or no digits, here\n");
        return;
    }
    const char* const kCovered[] = {
        "live\\shots\\r1_before.png", "live\\shots\\r1_pre.png",   "live\\shots\\r2_before.png",
        "live\\shots\\r2_B_000.png",  "live\\shots\\r2_B_001.png", "live\\shots\\r3_D_000.png",
        "live\\shots\\r3_D_001.png",  "live\\shots\\r3_D_002.png", "live\\shots\\r3_D_003.png",
        "live\\shots\\r3_pre.png",    "frozen\\live\\shots\\p1_now.png",
    };
    int seen = 0, never_a_count = 0;
    std::string wrong;
    for (const char* rel : kCovered) {
        Frame f;
        if (!exists(std::string(kLiveBuild) + rel) || !load_png(std::string(kLiveBuild) + rel, f)) continue;
        ++seen;
        const auto r = read_keys(f, { '1', '2' }, nullptr);
        const bool safe = std::all_of(r.begin(), r.end(), [](const SlotReading& s) {
            return s.state == SlotState::NoHotbar || s.state == SlotState::Unreadable;
        });
        never_a_count += safe ? 1 : 0;
        if (!safe && wrong.empty()) wrong = std::string(rel) + ": " + said(r[0]) + "/" + said(r[1]);
    }
    if (seen == 0) {
        std::printf("  [skip] none of the covered frames are here\n");
        return;
    }
    check(seen == never_a_count, fmt("%d of %d covered frames: no hotbar, or nothing readable -- never 1",
                                     never_a_count, seen) + (wrong.empty() ? std::string{} : " -- " + wrong));
}

/// `f` with a rectangle laid over it in a colour given as red, green, blue,
/// `alpha` of the way through: 255 is a window, less one seen through.
void lay_over(Frame& f, int x0, int y0, int x1, int y1, int r, int g, int b, int alpha) {
    for (int y = std::max(0, y0); y < std::min(f.height, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(f.width, x1); ++x) {
            std::uint8_t* p = &f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4];
            p[0] = static_cast<std::uint8_t>((p[0] * (255 - alpha) + b * alpha) / 255);
            p[1] = static_cast<std::uint8_t>((p[1] * (255 - alpha) + g * alpha) / 255);
            p[2] = static_cast<std::uint8_t>((p[2] * (255 - alpha) + r * alpha) / 255);
        }
}

void test_part_covered() {
    section("Potions: a window over part of a slot (the 2A tester's step 9)");
    const Eyes& e = eyes();
    if (!e.ok) {
        std::printf("  [skip] no frame.png from the live client, or no digits, here\n");
        return;
    }
    const HotbarSpot bar = find_hotbar(e.frame, e.picture);
    const ScreenRect one = bar.slot(0, 6);
    // A window's edge over the top rows of slot 1, its label still showing:
    // the count under it, and nothing white left in the corner, read as an
    // icon with no number -- "Còn 1 bình" in orange on the tab.
    struct Over {
        const char* what;
        int r, g, b, alpha;
        int rows;
    };
    for (const Over& o : { Over{ "a dark window", 20, 20, 25, 255, 16 },
                           Over{ "the same, 22 rows down", 20, 20, 25, 255, 22 },
                           Over{ "black seen through at half", 0, 0, 0, 128, 16 },
                           Over{ "black seen through at a third", 0, 0, 0, 90, 16 },
                           Over{ "a grey window", 60, 60, 70, 255, 16 },
                           Over{ "a window the bar's own blue", 33, 57, 90, 255, 16 } }) {
        Frame f = e.frame;
        lay_over(f, one.x - 60, 0, one.x + 80, one.y + o.rows, o.r, o.g, o.b, o.alpha);
        const HotbarSpot b = find_hotbar(f, e.picture);
        const SlotReading r = read_slot(f, b, '1', e.digits, e.picture);
        check(r.state == SlotState::Unreadable || r.state == SlotState::NoHotbar,
              std::string(o.what) + " over the top of slot 1: " + said(r) + ", never one left" +
                  fmt(" (hotbar %.3f)", b.score));
    }
    // A small window over the left of slot 1 -- a tooltip, say -- hiding the
    // 8 of its 89 and leaving the 9: never read as 9.
    {
        Frame f = e.frame;
        lay_over(f, one.x - 4, one.y - 30, one.x + 12, one.y + 20, 24, 24, 30, 255);
        const HotbarSpot b = find_hotbar(f, e.picture);
        const SlotReading r = read_slot(f, b, '1', e.digits, e.picture);
        check(b.found && r.state == SlotState::Unreadable,
              "a small window over the left of slot 1, hiding the 8 of its 89: " + said(r) + ", never 9");
    }
    // Slot 2 beside it, untouched by either: read as ever.
    {
        Frame f = e.frame;
        lay_over(f, one.x - 60, 0, one.x + 30, one.y + 16, 20, 20, 25, 255);
        const HotbarSpot b = find_hotbar(f, e.picture);
        const SlotReading two = read_slot(f, b, '2', e.digits, e.picture);
        check(two.state == SlotState::Count && two.count == 94,
              "and slot 2 beside it, nothing over it: 94 -- " + said(two));
    }
    // Something inside slot 1 alone, the bar's frame round it as it was: a
    // dark corner over its count, or a dark shade seen through over the top
    // of the slot or all of it, which leaves the 89 grey. The count is under
    // it: never "one left" (the round 2 tester's problem 1).
    {
        std::string wrong;
        int tried = 0;
        const struct {
            int alpha;
            int rows;
        } inside[] = { { 255, 16 }, { 255, 18 }, { 255, 20 }, { 255, 22 }, { 90, 16 },
                       { 90, 18 },  { 90, 20 },  { 90, 22 },  { 90, 34 } };
        for (const auto& c : inside) {
            Frame f = e.frame;
            lay_over(f, one.x, one.y, one.x + hotbar::kCell, one.y + c.rows, 0, 0, 0, c.alpha);
            const HotbarSpot b = find_hotbar(f, e.picture);
            const SlotReading r = read_slot(f, b, '1', e.digits, e.picture);
            ++tried;
            if (r.state != SlotState::Unreadable && r.state != SlotState::NoHotbar && wrong.empty())
                wrong = fmt(" -- %s over the top %d rows read %s", c.alpha == 255 ? "black" : "a shade", c.rows,
                            said(r).c_str());
        }
        check(wrong.empty(), fmt("%d covers inside slot 1 alone, opaque and seen through: never one left", tried) +
                                 wrong);
    }
    // And an icon with no number is still one: V, a skill, on frame.png; and
    // every icon of the catalog drawn into the empty slot 3 with nothing white
    // where its count would be.
    {
        const SlotReading v = read_slot(e.frame, bar, 'V', e.digits, e.picture);
        check(v.state == SlotState::Single, "V, a skill with nothing printed, still one -- " + said(v));
        ItemCatalog catalog;
        if (!catalog.load(ItemCatalog::default_dir())) {
            std::printf("  [skip] the item catalog is not installed here\n");
            return;
        }
        const ScreenRect three = bar.slot(0, 8);
        int drawn = 0, ones = 0;
        std::string wrong;
        for (const auto& g : catalog.groups)
            for (const auto& entry : g.entries) {
                const Frame& icon = entry.icon;
                if (!icon.valid() || icon.width > hotbar::kCell || icon.height > hotbar::kCell) continue;
                Frame f = e.frame;
                const int ox = three.x + (hotbar::kCell - icon.width) / 2;
                const int oy = three.y + (hotbar::kCell - icon.height) / 2;
                bool white = false;
                for (int y = 0; y < icon.height; ++y)
                    for (int x = 0; x < icon.width; ++x) {
                        const int fx = ox + x, fy = oy + y;
                        // The label's corner is the slot's own.
                        if (fx >= three.x + hotbar::kCell - hotbar::kLabelCorner &&
                            fy >= three.y + hotbar::kCell - hotbar::kLabelCorner)
                            continue;
                        const std::uint8_t* p = &icon.pixels[(static_cast<std::size_t>(y) * icon.width + x) * 4];
                        if (p[3] < TemplateMatcher::kOpaque) continue;
                        std::uint8_t* d = &f.pixels[(static_cast<std::size_t>(fy) * f.width + fx) * 4];
                        std::copy_n(p, 3, d);
                        const int lo = std::min({ p[0], p[1], p[2] }), hi = std::max({ p[0], p[1], p[2] });
                        if (fx < three.x + hotbar::kCountW && fy < three.y + hotbar::kCountH &&
                            (p[0] + p[1] + p[2]) / 3 > 175 && hi - lo < 40)
                            white = true;
                    }
                // White in the corner is a count or not, never "one": the
                // rest of read_slot's, tested above.
                if (white) continue;
                ++drawn;
                const SlotReading r = read_slot(f, bar, '3', e.digits, e.picture);
                ones += r.state == SlotState::Single ? 1 : 0;
                if (r.state == SlotState::Unreadable && wrong.empty()) wrong = " -- " + entry.id + ": " + said(r);
            }
        check(drawn > 10 && wrong.empty(),
              fmt("%d catalog icons drawn into slot 3 with no number: %d read as one, none as unreadable", drawn,
                  ones) + wrong);
    }
}

void test_drawn() {
    section("Potions: slots drawn into frame.png (PS-A5)");
    const Eyes& e = eyes();
    if (!e.ok) {
        std::printf("  [skip] no frame.png from the live client, or no digits, here\n");
        return;
    }
    const HotbarSpot bar = find_hotbar(e.frame, e.picture);
    const ScreenRect three = bar.slot(0, 8), one = bar.slot(0, 6);

    // A number written into the empty slot 3 reads back: every count the
    // client prints, from 2 -- the least, a stack of one being its icon with
    // no number -- to four digits.
    for (const char* n : { "2", "7", "42", "315", "1000" }) {
        Frame f = e.frame;
        draw_number(f, three.x + 2, three.y + 2, n);
        const SlotReading r = read_slot(f, bar, '3', e.digits, e.picture);
        check(r.state == SlotState::Count && r.count == std::atoi(n),
              std::string("slot 3 with ") + n + " drawn in: " + said(r));
    }
    // A lone 1: the client prints counts from 2 up, so that is the icon's own
    // white, never a count of one.
    {
        Frame f = e.frame;
        draw_number(f, three.x + 2, three.y + 2, "1");
        const SlotReading r = read_slot(f, bar, '3', e.digits, e.picture);
        check(r.state == SlotState::Unreadable, "slot 3 with a lone 1 drawn in: unreadable -- " + said(r) +
                                                    " (R2A-5)");
    }
    // Slot 1 made as empty as slot 3, its own label kept.
    {
        Frame f = e.frame;
        copy_rect(e.frame, three.x, three.y, f, one.x, one.y, hotbar::kCell - hotbar::kLabelCorner,
                  hotbar::kCell);
        copy_rect(e.frame, three.x + hotbar::kCell - hotbar::kLabelCorner, three.y, f,
                  one.x + hotbar::kCell - hotbar::kLabelCorner, one.y, hotbar::kLabelCorner,
                  hotbar::kCell - hotbar::kLabelCorner);
        const SlotReading r = read_slot(f, bar, '1', e.digits, e.picture);
        check(r.state == SlotState::Empty, "slot 1 emptied, its label left: empty -- " + said(r));
    }
    // Slot 3's own label swapped for slot 1's: the key's label is not there.
    {
        Frame f = e.frame;
        const int c = hotbar::kCell - hotbar::kLabelCorner;
        copy_rect(e.frame, one.x + c, one.y + c, f, three.x + c, three.y + c, hotbar::kLabelCorner,
                  hotbar::kLabelCorner);
        const SlotReading r = read_slot(f, bar, '3', e.digits, e.picture);
        check(r.state == SlotState::Unreadable, fmt("slot 3 labelled 1: unreadable (label %.2f)", r.label));
    }
    // Keys that are not on the bar, and a bar that is not there.
    check(read_slot(e.frame, bar, 'Q', e.digits, e.picture).state == SlotState::NotOnBar &&
              read_slot(e.frame, bar, VK_NUMPAD1, e.digits, e.picture).state == SlotState::NotOnBar,
          "Q and Numpad 1: not on the hotbar");
    HotbarSpot gone;
    check(read_slot(e.frame, gone, '1', e.digits, e.picture).state == SlotState::NoHotbar,
          "no hotbar found: no hotbar, never a number");
    CountReader no_digits;
    check(read_slot(e.frame, bar, '1', no_digits, e.picture).state == SlotState::Unreadable &&
              read_slot(e.frame, bar, '3', no_digits, e.picture).state == SlotState::Empty,
          "without the digits a number is unreadable, and an empty slot still empty");
    int row = -1, col = -1;
    check(hotbar_slot_of('1', row, col) && row == 0 && col == 6 && hotbar_slot_of('6', row, col) &&
              row == 1 && col == 8 && hotbar_slot_of('A', row, col) && row == 0 && col == 0,
          "the key table: 1 at row 0 column 6, 6 at row 1 column 8, A at the start");
}

void test_game_digits() {
    section("Potions: every slot, the game's potion icons and digits -- a 1 over a bright icon (round 8)");
    // No saved frame: black, the shipped picture of the bar drawn at its
    // place on 1280 x 720 -- its frame and labels -- and an empty slot's dark
    // gradient in every slot.
    Frame picture;
    if (!TemplateStore::load_one(hotbar::kPicture, exe_relative("configs/templates.ini"), picture) ||
        picture.width != hotbar::kPictureW || picture.height != hotbar::kPictureH) {
        std::printf("  [skip] no grabbed/hotbar_frame.png staged beside this test\n");
        return;
    }
    CountReader digits;
    if (!digits.load(GlyphFont::default_dir())) {
        std::printf("  [skip] no digits of the game's font here\n");
        return;
    }
    const std::string images = ItemCatalog::default_dir();
    std::vector<std::string> icons;
    for (const char* folder : { "\\spend\\hp\\", "\\spend\\mana\\" })
        for (const std::string& p : pngs_in(images + folder, "*.png")) icons.push_back(p);
    if (images.empty() || icons.empty()) {
        std::printf("  [skip] the game's potion icons are not installed here\n");
        return;
    }
    constexpr int kPicX = 469, kPicY = 625;
    Frame base;
    base.width = 1280;
    base.height = 720;
    base.pixels.assign(static_cast<std::size_t>(base.width) * base.height * 4, 0);
    for (std::size_t i = 3; i < base.pixels.size(); i += 4) base.pixels[i] = 255;
    for (int y = 0; y < picture.height; ++y)
        for (int x = 0; x < picture.width; ++x) {
            const std::uint8_t* p = &picture.pixels[(static_cast<std::size_t>(y) * picture.width + x) * 4];
            std::uint8_t* d = &base.pixels[(static_cast<std::size_t>(kPicY + y) * base.width + kPicX + x) * 4];
            const int sx = x - hotbar::kSlotDx, sy = y - hotbar::kSlotDy;
            const bool in_slot = sx >= 0 && sy >= 0 && sx % hotbar::kPitch < hotbar::kCell &&
                                 sy % hotbar::kPitch < hotbar::kCell && sx / hotbar::kPitch < hotbar::kCols &&
                                 sy / hotbar::kPitch < hotbar::kRows;
            if (p[3] >= hotbar::kLabelAlpha) {
                std::copy_n(p, 3, d);
            } else if (in_slot) {
                d[0] = d[1] = d[2] = static_cast<std::uint8_t>(6 + sy % hotbar::kPitch);   // 6 .. 39
            }
        }
    const HotbarSpot bar = find_hotbar(base, picture);
    check(bar.found && bar.x == kPicX + hotbar::kSlotDx && bar.y == kPicY + hotbar::kSlotDy,
          fmt("the bar drawn from its picture is found at %d,%d (%.3f)", bar.x, bar.y, bar.score));
    if (!bar.found) return;

    const int counts[] = { 2, 7, 11, 21, 31, 41, 51, 61, 71, 81, 91, 99, 100, 101, 107, 110, 111, 117, 121, 211, 311,
                           999, 1000 };
    Frame f = base;
    int used = 0, reads = 0, wrong = 0, lone_counts = 0, blank_counts = 0, egg_reads = 0, egg_wrong = 0;
    std::string first_wrong, egg_said;
    for (const std::string& path : icons) {
        Frame icon;
        if (!load_png(path, icon) || !icon.valid() || icon.width > hotbar::kCell || icon.height > hotbar::kCell)
            continue;
        ++used;
        const std::string name = path.substr(path.find_last_of('\\') + 1);
        const bool egg = name == "8820141.png";     // the boiled egg: slot 7's HP potion on 2026-10-20
        for (int row = 0; row < hotbar::kRows; ++row)
            for (int col = 0; col < hotbar::kCols; ++col) {
                const ScreenRect s = bar.slot(row, col);
                const std::uint32_t vk = hotbar::kKeys[row][col];
                // The icon at the slot's 2,2, centred when smaller, its
                // opaque pixels, the label's corner left as it is.
                const int ox = 2 + (30 - icon.width) / 2, oy = 2 + (30 - icon.height) / 2;
                for (int y = 0; y < icon.height; ++y)
                    for (int x = 0; x < icon.width; ++x) {
                        const std::uint8_t* p = &icon.pixels[(static_cast<std::size_t>(y) * icon.width + x) * 4];
                        const int sx = ox + x, sy = oy + y;
                        if (p[3] < TemplateMatcher::kOpaque || sx >= hotbar::kCell || sy >= hotbar::kCell ||
                            (sx >= hotbar::kCell - hotbar::kLabelCorner && sy >= hotbar::kCell - hotbar::kLabelCorner))
                            continue;
                        std::copy_n(p, 3, &f.pixels[(static_cast<std::size_t>(s.y + sy) * f.width + s.x + sx) * 4]);
                    }
                Frame with_icon;
                with_icon.width = with_icon.height = hotbar::kCell;
                with_icon.pixels.assign(static_cast<std::size_t>(hotbar::kCell) * hotbar::kCell * 4, 255);
                copy_rect(f, s.x, s.y, with_icon, 0, 0, hotbar::kCell, hotbar::kCell);

                // Nothing printed is never a count; nor is a lone 1, which
                // the client never prints.
                blank_counts += read_slot(f, bar, vk, digits, picture).state == SlotState::Count ? 1 : 0;
                draw_game_number(f, s.x + 2, s.y + 4, "1");
                lone_counts += read_slot(f, bar, vk, digits, picture).state == SlotState::Count ? 1 : 0;
                copy_rect(with_icon, 0, 0, f, s.x, s.y, hotbar::kCell, hotbar::kCell);

                for (const int n : counts) {
                    draw_game_number(f, s.x + 2, s.y + 4, std::to_string(n));
                    const SlotReading r = read_slot(f, bar, vk, digits, picture);
                    copy_rect(with_icon, 0, 0, f, s.x, s.y, hotbar::kCell, hotbar::kCell);
                    ++reads;
                    const bool right = r.state == SlotState::Count && r.count == n;
                    if (egg && row == 0 && col == 6 && (n == 61 || n == 51 || n == 41 || n == 21 || n == 11)) {
                        ++egg_reads;
                        egg_wrong += right ? 0 : 1;
                        egg_said += (egg_said.empty() ? "" : ", ") + std::to_string(n) + " " + said(r);
                    }
                    if (right) continue;
                    ++wrong;
                    if (first_wrong.empty())
                        first_wrong = fmt(" -- e.g. %s in slot %d row %d: %d read %s", name.c_str(), col + 1, row + 1, n,
                                          said(r).c_str());
                }
                copy_rect(base, s.x, s.y, f, s.x, s.y, hotbar::kCell, hotbar::kCell);
            }
    }
    check(used >= 20, fmt("%d of the game's potion icons drawn into every slot", used));
    if (egg_reads > 0)
        check(egg_wrong == 0, "slot 7 (key 1) with the boiled egg: " + egg_said + " (61 read as 6 on 2026-10-20)");
    check(wrong == 0, fmt("%d counts read over %d icons in all 18 slots: %d wrong", reads, used, wrong) + first_wrong);
    check(lone_counts == 0, fmt("a lone 1 drawn over each: never a count (%d were)", lone_counts));
    check(blank_counts == 0, fmt("nothing printed over each: never a count (%d were)", blank_counts));
}

// ------------------------------------------------------------- the warnings

SlotReading count(int n) {
    SlotReading r;
    r.state = SlotState::Count;
    r.count = n;
    return r;
}

SlotReading empty() {
    SlotReading r;
    r.state = SlotState::Empty;
    return r;
}

/// Each reading is one look's (PotionWatch::look_of), fed once.
int feed(PotionWatch& w, std::uint32_t vk, const std::vector<SlotReading>& seq, PotionWatch::Event::Kind kind) {
    int n = 0;
    for (const SlotReading& r : seq)
        for (const auto& ev : w.feed(vk, r)) n += ev.kind == kind ? 1 : 0;
    return n;
}

/// Every event of a run of looks, and on which look (0 on) each came.
struct Fed {
    std::vector<PotionWatch::Event> events;
    std::vector<int>                look;
    std::vector<int>                shown;      ///< what believed() said after each look; -1 none or no count

    int of(PotionWatch::Event::Kind k) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(),
                                              [&](const PotionWatch::Event& e) { return e.kind == k; }));
    }
    /// The look the first event of kind `k` came on; -1 none.
    int first(PotionWatch::Event::Kind k) const {
        for (std::size_t i = 0; i < events.size(); ++i)
            if (events[i].kind == k) return look[i];
        return -1;
    }
    const PotionWatch::Event* find(PotionWatch::Event::Kind k) const {
        for (const auto& e : events)
            if (e.kind == k) return &e;
        return nullptr;
    }
};

Fed run_looks(PotionWatch& w, std::uint32_t vk, const std::vector<int>& counts) {
    Fed f;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        for (const auto& ev : w.feed(vk, count(counts[i]))) {
            f.events.push_back(ev);
            f.look.push_back(static_cast<int>(i));
        }
        SlotReading b;
        f.shown.push_back(w.believed(vk, b) && b.has_count() ? b.count : -1);
    }
    return f;
}

std::string seq_text(const std::vector<int>& counts) {
    std::string s;
    for (const int n : counts) s += (s.empty() ? "" : ",") + std::to_string(n);
    return s;
}

void test_watch() {
    section("Potions: when it is worth a message (PS-A6)");
    using Kind = PotionWatch::Event::Kind;
    {
        PotionWatch w;
        w.set_warn_below(20);
        std::vector<PotionWatch::Event> got;
        for (const int n : { 25, 22, 19, 19, 18, 17 })
            for (const auto& ev : w.feed('1', count(n))) got.push_back(ev);
        check(got.size() == 1 && got[0].kind == Kind::Low && got[0].count == 19 && got[0].vk == '1',
              "25, 22, 19, 19, 18, 17: exactly one 'sắp hết', at 19");
        check(feed(w, '1', { count(100) }, Kind::Low) == 0, "100: the next stack, nothing said");
        // 100 straight to 15 is a wild drop now (a fifth or less), so the
        // stack is taken down a way it can go.
        check(feed(w, '1', { count(60), count(30), count(15), count(15) }, Kind::Low) == 1,
              "then 60, 30, 15, 15: a second one");
        check(feed(w, '1', { count(14), count(14), count(13), count(13) }, Kind::Low) == 0,
              "and not again while it goes on down");
        check(feed(w, '1', { count(22), count(16), count(16) }, Kind::Low) == 0,
              "22 is not 5 over: not re-armed");
        check(feed(w, '1', { count(25), count(16), count(16) }, Kind::Low) == 1,
              "25 is: re-armed, and 16, 16 warns again");
    }
    {
        PotionWatch w;
        w.set_warn_below(0);
        check(feed(w, '1', { count(5), count(5), count(1), count(1) }, Kind::Low) == 0,
              "warn_below=0: never 'sắp hết'");
        check(feed(w, '1', { empty(), empty() }, Kind::Out) == 1, "while an empty slot is still said");
    }
    {
        PotionWatch w;
        check(feed(w, '2', { empty() }, Kind::Out) == 0, "empty once: nothing");
        check(feed(w, '2', { empty() }, Kind::Out) == 1, "empty twice: one 'hết bình'");
        check(feed(w, '2', { empty(), empty(), empty() }, Kind::Out) == 0, "and only one");
        check(feed(w, '2', { count(100), count(100), empty(), empty() }, Kind::Out) == 1,
              "a stack again, then empty again: one more");
    }
    {
        PotionWatch w;
        check(feed(w, '1', { count(25), count(25), count(5), count(25), count(25) }, Kind::Low) == 0,
              "one misread 5 between agreeing readings: nothing");
        SlotReading b;
        check(w.believed('1', b) && b.count == 25, "and what is shown stays 25");
        SlotReading unread;
        unread.state = SlotState::NoHotbar;
        check(feed(w, '1', { unread, count(3), unread, count(3) }, Kind::Low) == 0,
              "3s with the hotbar covered between them are never two in a row: nothing");
    }
    {
        PotionWatch w;
        SlotReading single;
        single.state = SlotState::Single;
        single.count = 1;
        check(feed(w, '1', { single, single, single }, Kind::Low) == 0,
              "an icon with no number -- a skill on the key looks the same: no 'sắp hết'");
        SlotReading shown;
        check(w.believed('1', shown) && shown.state == SlotState::Single, "though it is shown as one left");
    }
    {
        PotionWatch w;
        feed(w, '1', { count(10), count(10) }, Kind::Low);
        w.reset();
        check(feed(w, '1', { count(10), count(10) }, Kind::Low) == 1, "a new run: warned again");
        check(feed(w, '2', { count(10), count(10) }, Kind::Low) == 1, "each key on its own");
    }
}

void test_wild() {
    section("Potions: a misread is not believed -- one reading per look, wild drops held (round 8)");
    using Kind = PotionWatch::Event::Kind;
    // The five misreads of 2026-10-20, as the looks read them -- a trailing 1
    // lost, 61 read as 6 for two looks, and then the count back: each raised
    // "Sắp hết bình" then. Now nothing is said, the old count is shown
    // meanwhile, and the misread is told once, with what it read and when.
    for (const std::vector<int>& seq : { std::vector<int>{ 62, 6, 6, 60 }, std::vector<int>{ 52, 5, 5, 50 },
                                         std::vector<int>{ 42, 4, 4, 40 }, std::vector<int>{ 32, 3, 3, 28 },
                                         std::vector<int>{ 22, 2, 2, 19, 19 } }) {
        PotionWatch w;
        w.set_warn_below(20);
        const Fed f = run_looks(w, '1', seq);
        const PotionWatch::Event* mis = f.find(Kind::Misread);
        const bool shown_old = f.shown[1] == seq[0] && f.shown[2] == seq[0];
        const bool last_low = seq.size() == 5;      // 22,2,2,19,19: under the mark twice at the end
        check(f.of(Kind::Wild) == 2 && f.of(Kind::Misread) == 1 && mis && mis->count == seq[1] &&
                  mis->was == seq[0] && mis->back == seq[3] && f.first(Kind::Misread) == 3 && shown_old &&
                  f.of(Kind::WildBelieved) == 0 &&
                  (last_low ? f.of(Kind::Low) == 1 && f.first(Kind::Low) == 4 && f.find(Kind::Low)->count == 19
                            : f.of(Kind::Low) == 0),
              "the live misread " + seq_text(seq) + ": two looks held, " + std::to_string(seq[0]) +
                  " still shown, one misread told on the look it came back" +
                  (last_low ? ", and 'sắp hết' only at the second 19" : ", no 'sắp hết'"));
    }
    // A real drop -- potions given away -- is believed on the third look that
    // reads it, and warned of then: the three looks agreed under the mark.
    {
        PotionWatch w;
        w.set_warn_below(20);
        const Fed f = run_looks(w, '1', { 62, 6, 6, 6 });
        const PotionWatch::Event* wb = f.find(Kind::WildBelieved);
        check(wb && wb->count == 6 && wb->was == 62 && f.first(Kind::WildBelieved) == 3 &&
                  f.of(Kind::Low) == 1 && f.first(Kind::Low) == 3 && f.shown[3] == 6 && f.shown[2] == 62,
              "62, 6, 6, 6: believed on the third 6, and 'sắp hết' with it");
    }
    {
        PotionWatch w;
        w.set_warn_below(20);
        const Fed f = run_looks(w, '1', { 62, 6, 5, 4 });
        check(f.of(Kind::WildBelieved) == 1 && f.first(Kind::WildBelieved) == 3 && f.shown[3] == 4,
              "62, 6, 5, 4 -- drinking on: believed on the third look, at 4");
    }
    {
        PotionWatch w;
        w.set_warn_below(20);
        const Fed f = run_looks(w, '1', { 62, 6, 7, 6 });
        check(f.of(Kind::WildBelieved) == 0 && f.of(Kind::Low) == 0 && f.shown[3] == 62,
              "62, 6, 7, 6: the 7 starts the run again, nothing believed");
    }
    {
        PotionWatch w;
        w.set_warn_below(20);
        const Fed f = run_looks(w, '1', { 62, 6, 6, 62 });
        check(f.of(Kind::Misread) == 1 && f.of(Kind::Low) == 0 && f.shown[3] == 62,
              "62, 6, 6, 62: a misread, no 'sắp hết'");
    }
    // An ordinary run down: the warning needs two looks under the mark, and
    // does not need them to read the same -- the count falls while the
    // player drinks.
    {
        PotionWatch w;
        w.set_warn_below(20);
        const Fed f = run_looks(w, '1', { 25, 22, 19, 18, 17 });
        check(f.of(Kind::Low) == 1 && f.first(Kind::Low) == 3 && f.find(Kind::Low)->count == 18,
              "25, 22, 19, 18, 17: one 'sắp hết', at 18 -- no two looks the same");
    }
    {
        PotionWatch w;
        w.set_warn_below(20);
        const Fed f = run_looks(w, '1', { 62, 6, 62 });
        check(f.of(Kind::Misread) == 1 && f.of(Kind::Wild) == 1 && f.events.size() == 2 && f.shown[2] == 62,
              "62, 6, 62: one look held, then a misread, and nothing else");
    }
    {
        PotionWatch w;
        w.set_warn_below(20);
        const Fed f = run_looks(w, '1', { 24, 5, 24 });
        check(f.of(Kind::Low) == 0 && f.of(Kind::Wild) == 0 && f.shown[2] == 24,
              "24, 5, 24: not a fifth (25 > 24), so believed, but one look under the mark is no 'sắp hết'");
    }
    {
        PotionWatch w;
        w.set_warn_below(20);
        const Fed f = run_looks(w, '1', { 8, 1, 1 });
        check(f.of(Kind::Wild) == 0 && f.of(Kind::Low) == 1 && f.first(Kind::Low) == 1 &&
                  f.find(Kind::Low)->count == 1,
              "8, then 1: under 10 nothing is a wild drop -- 8 and 1 are two looks under the mark, one 'sắp hết'");
    }
    // A look that is no count neither confirms a wild drop nor clears it --
    // not when the slot has been covered long enough to be believed so
    // either: the stack is still there, out of sight, and the last count
    // believed is what the next is told from (BA rule 4; the round 8
    // tester's C3 and C5).
    {
        PotionWatch w;
        w.set_warn_below(20);
        SlotReading covered;
        covered.state = SlotState::NoHotbar;
        std::vector<PotionWatch::Event> got;
        bool held_through = true;
        int look = 0;
        for (const SlotReading& r : { count(62), count(6), covered, covered, count(60) }) {
            for (const auto& ev : w.feed('1', r)) got.push_back(ev);
            ++look;
            if (look >= 2 && look <= 4) held_through = held_through && w.holding('1');
        }
        int wild = 0, misread = 0;
        const PotionWatch::Event* mis = nullptr;
        for (const auto& ev : got) {
            wild += ev.kind == Kind::Wild ? 1 : 0;
            if (ev.kind == Kind::Misread) {
                ++misread;
                mis = &ev;
            }
        }
        SlotReading b;
        check(wild == 1 && misread == 1 && mis && mis->count == 6 && mis->was == 62 && mis->back == 60 &&
                  held_through && !w.holding('1') && w.believed('1', b) && b.state == SlotState::Count &&
                  b.count == 60 && got.size() == 2,
              "62, 6, the bar covered on two looks, 60: the 6 held through the cover, then told as a misread of 62");
    }
    {
        PotionWatch w;
        w.set_warn_below(20);
        SlotReading covered;
        covered.state = SlotState::NoHotbar;
        std::vector<PotionWatch::Event> got;
        for (const SlotReading& r : { count(62), covered, covered, count(6), count(6) })
            for (const auto& ev : w.feed('1', r)) got.push_back(ev);
        int wild = 0, low = 0;
        for (const auto& ev : got) {
            wild += ev.kind == Kind::Wild ? 1 : 0;
            low += ev.kind == Kind::Low ? 1 : 0;
        }
        check(wild == 2 && low == 0 && w.holding('1'),
              "62, the bar covered on two looks, 6, 6: a drop from 62 all the same -- held, no 'sắp hết'");
        std::vector<PotionWatch::Event> third = w.feed('1', count(6));
        SlotReading b;
        check(third.size() == 2 && third[0].kind == Kind::WildBelieved && third[0].was == 62 &&
                  third[1].kind == Kind::Low && w.believed('1', b) && b.count == 6,
              "and a third 6: believed, with 'sắp hết'");
        SlotReading unread;
        unread.state = SlotState::Unreadable;
        PotionWatch u;
        u.set_warn_below(20);
        int misreads = 0;
        for (const SlotReading& r : { count(41), count(4), unread, unread, unread, count(40) })
            for (const auto& ev : u.feed('1', r)) misreads += ev.kind == Kind::Misread ? 1 : 0;
        check(misreads == 1, "41, 4, three unreadable looks, 40: a misread too");
    }
    {
        // The slot believed empty -- two looks -- is a stack gone: the next
        // count is a new stack, believed at once.
        PotionWatch w;
        w.set_warn_below(20);
        std::vector<PotionWatch::Event> got;
        for (const SlotReading& r : { count(62), empty(), empty(), count(6), count(6) })
            for (const auto& ev : w.feed('1', r)) got.push_back(ev);
        int wild = 0, low = 0, out = 0;
        for (const auto& ev : got) {
            wild += ev.kind == Kind::Wild ? 1 : 0;
            low += ev.kind == Kind::Low ? 1 : 0;
            out += ev.kind == Kind::Out ? 1 : 0;
        }
        SlotReading b;
        check(wild == 0 && out == 1 && low == 1 && w.believed('1', b) && b.count == 6,
              "62, empty, empty, 6, 6: 'hết bình', then a new stack of 6 believed at once, and 'sắp hết' on the second");
    }
    // A look that is no count between two under the mark breaks the pair.
    {
        PotionWatch w;
        w.set_warn_below(20);
        SlotReading covered;
        covered.state = SlotState::NoHotbar;
        check(feed(w, '1', { count(25), count(18), covered, count(17) }, Kind::Low) == 0,
              "18, the hotbar covered, 17: not two looks in a row, no 'sắp hết'");
        check(feed(w, '1', { count(16) }, Kind::Low) == 1, "and 16 on the next look: now");
    }
    // A look's two frames make its one reading.
    {
        const SlotReading a = count(27), b = count(27), c = count(26);
        SlotReading single;
        single.state = SlotState::Single;
        single.count = 1;
        check(PotionWatch::look_of(a, b).state == SlotState::Count && PotionWatch::look_of(a, b).count == 27,
              "two frames that agree: their reading");
        check(PotionWatch::look_of(a, c).state == SlotState::Unreadable &&
                  PotionWatch::look_of(empty(), single).state == SlotState::Unreadable,
              "two that do not: unreadable -- the slot changed between them, or one misread it");
        check(PotionWatch::look_of(empty(), empty()).state == SlotState::Empty, "two empties: empty");
    }
}

/// A pretend Discord that counts what it is sent.
class CountingChannel final : public AlertChannel {
public:
    explicit CountingChannel(std::shared_ptr<int> sent) : sent_(std::move(sent)) {}
    ChannelReply send(const std::string&) override {
        ++*sent_;
        ChannelReply r;
        r.http_status = 204;
        return r;
    }

private:
    std::shared_ptr<int> sent_;
};

/// How many messages one potion_low raised under `s` reaches the pretend
/// Discord with, the service stepped by hand for three minutes.
int sent_with(const AlertSettings& s, bool& raised) {
    auto sent = std::make_shared<int>(0);
    AlertService svc("");
    svc.set_settings(s);
    svc.set_channel(std::make_unique<CountingChannel>(sent));
    raised = svc.raise("potion_low", alert_text::potion_low(true, "1", 18), 1000);
    for (std::uint64_t t = 1000; t <= 181'000; t += 500) svc.step_discord(t);
    return *sent;
}

void test_texts_and_settings() {
    section("Potions: what is said, and [Potions] (PS-A7, PS-R7, PS-R10)");
    check(potion_line_vi(count(27), "1") == "Còn 27 bình (ô phím 1)", "Còn 27 bình (ô phím 1)");
    SlotReading single;
    single.state = SlotState::Single;
    single.count = 1;
    check(potion_line_vi(single, "2") == "Còn 1 bình (ô phím 2)", "one left");
    check(potion_line_vi(empty(), "1") == "Ô phím 1 đang trống", "Ô phím 1 đang trống");
    SlotReading off;
    off.state = SlotState::NotOnBar;
    check(potion_line_vi(off, "Q") == "Phím Q không có trên thanh phím", "Phím Q không có trên thanh phím");
    SlotReading none;
    check(potion_line_vi(none, "1") == "Chưa đọc được số bình", "Chưa đọc được số bình");

    check(alert_text::potion_low(true, "1", 18) == "Sắp hết bình HP: ô phím 1 còn 18 bình.",
          "the alert: " + alert_text::potion_low(true, "1", 18));
    check(alert_text::potion_out(false, "2") ==
              "Hết bình MP ở ô phím 2 — GoBot vẫn bấm phím nhưng ô đã trống.",
          "the alert: " + alert_text::potion_out(false, "2"));
    check(alert_text::potion_low(true, "a\"b/c", 3) == "Sắp hết bình HP: ô phím ? còn 3 bình.",
          "a key name with anything but letters and digits in it is not said");
    const AlertEventInfo* low = find_alert_event("potion_low");
    const AlertEventInfo* out = find_alert_event("potion_out");
    check(low && !low->critical && low->on && std::string(low->label_vi) == "Sắp hết bình",
          "potion_low: not critical, ticked, 'Sắp hết bình'");
    check(out && out->critical && out->on && std::string(out->label_vi) == "Hết bình",
          "potion_out: critical, ticked, 'Hết bình'");

    // Through the alert service, as GuiApp::alert hands it on.
    {
        bool raised = true;
        AlertSettings off_s;
        off_s.enabled = false;
        const int n_off = sent_with(off_s, raised);
        check(!raised && n_off == 0, "alerts off: the pretend Discord gets nothing");
        AlertSettings unticked;
        unticked.enabled = true;
        unticked.events["potion_low"] = false;
        const int n_unticked = sent_with(unticked, raised);
        check(!raised && n_unticked == 0, "potion_low unticked: nothing either");
        AlertSettings on;
        on.enabled = true;
        const int n_on = sent_with(on, raised);
        check(raised && n_on == 1, "alerts on, ticked as shipped: one message (" + std::to_string(n_on) + ")");
    }

    IniFile ini;
    ini.set("Potions", "warn_below", "900");
    ini.set("Potions", "read_every_s", "0");
    PotionSettings s;
    s.load(ini);
    check(s.warn_below == PotionSettings::kMaxWarn && s.read_every_s == PotionSettings::kMinEvery,
          "out of range: brought into range");
    IniFile blank;
    PotionSettings d;
    d.load(blank);
    check(d.warn_below == 20 && d.read_every_s == 10, "nothing there: 20 and 10 s");
#ifdef GOBOT_SOURCE_DIR
    IniFile shipped;
    PotionSettings p;
    p.warn_below = -1;
    check(shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/app.ini") &&
              shipped.has_key("Potions", "warn_below") && shipped.has_key("Potions", "read_every_s"),
          "the shipped app.ini has [Potions]");
    p.load(shipped);
    check(p.warn_below == 20 && p.read_every_s == 10, "warn_below=20, read_every_s=10");
    const std::string alerts = read_all(std::string(GOBOT_SOURCE_DIR) + "/configs/alerts.ini");
    check(alerts.find("\npotion_low=true") != std::string::npos &&
              alerts.find("\npotion_out=true") != std::string::npos,
          "and alerts.ini lists potion_low and potion_out, ticked");
    // Look only: the reader holds nothing, takes no mouse and presses nothing.
    for (const char* file : { "/src/potion_stock.cpp", "/apps/gobot_gui/simple_potions.cpp" }) {
        const std::string src = read_all(std::string(GOBOT_SOURCE_DIR) + file);
        check(!src.empty() && src.find("take_hold") == std::string::npos &&
                  src.find("game_mouse_mutex_") == std::string::npos && src.find("click_at") == std::string::npos &&
                  src.find("key_press") == std::string::npos && src.find("input_.") == std::string::npos,
              std::string(file) + " takes no hold and no mouse, and calls no InputManager function (PS-A8)");
    }
#endif
}

} // namespace

void test_potions(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_picture();
    test_picture_blank();
    test_frame_png();
    test_1024();
    test_rounds();
    test_covered();
    test_part_covered();
    test_drawn();
    test_game_digits();
    test_watch();
    test_wild();
    test_texts_and_settings();
}
