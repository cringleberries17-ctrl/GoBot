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
        x += g.width + 1;
    }
}

void copy_rect(const Frame& from, int fx, int fy, Frame& to, int tx, int ty, int w, int h) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            std::copy_n(&from.pixels[(static_cast<std::size_t>(fy + y) * from.width + fx + x) * 4], 4,
                        &to.pixels[(static_cast<std::size_t>(ty + y) * to.width + tx + x) * 4]);
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

void test_drawn() {
    section("Potions: slots drawn into frame.png (PS-A5)");
    const Eyes& e = eyes();
    if (!e.ok) {
        std::printf("  [skip] no frame.png from the live client, or no digits, here\n");
        return;
    }
    const HotbarSpot bar = find_hotbar(e.frame, e.picture);
    const ScreenRect three = bar.slot(0, 8), one = bar.slot(0, 6);

    // A number written into the empty slot 3 reads back.
    for (const char* n : { "7", "42", "315" }) {
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

int feed(PotionWatch& w, std::uint32_t vk, const std::vector<SlotReading>& seq, PotionWatch::Event::Kind kind) {
    int n = 0;
    for (const SlotReading& r : seq)
        for (const auto& ev : w.feed(vk, r)) n += ev.kind == kind ? 1 : 0;
    return n;
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
        check(feed(w, '1', { count(15), count(15) }, Kind::Low) == 1, "then 15, 15: a second one");
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
    test_frame_png();
    test_1024();
    test_rounds();
    test_covered();
    test_drawn();
    test_watch();
    test_texts_and_settings();
}
