#include "gobot/vitals_reader.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"
#include "gobot/template_matcher.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <thread>
#include <vector>

namespace gobot {
namespace {

bool near(std::uint8_t a, std::uint8_t b, int tol) {
    return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tol;
}

constexpr int kCaptureBlack = 3;       // typical colour's brightest channel at most this: nothing captured
constexpr int kTroughLightAbove = 90;  // over this, with no sliver of fill: light -- the old cut-off
constexpr int kTroughSpread = 8;       // a trough pixel's distance from that colour, per channel

// A companion slot's frame round its bars, as the HUD draws it: a light grey
// frame (b5b5b5) with a black border (000000) inside it. Black only as black
// as the border: the darkest of a slot bar's empty part is 181818. Grey, and
// no lighter than this: white is the writing and the pictures in the slots
// ("Thức Ăn 7"), which, outlined in black, can look like a frame for a row.
constexpr int kSlotBlackMax = 12;      // brightest channel at most this: the border
constexpr int kSlotLightMin = 144;     // darkest channel at least this,
constexpr int kSlotLightMax = 224;     //   brightest at most this,
constexpr int kSlotGreySpread = 40;    //   and the channels this close: the frame
constexpr int kSlotFramesAlike = 24;   // the frame either side of a bar, per channel
constexpr int kPetMinRows = 3;         // a bar is more than a row or two of a picture

bool slot_black(const Frame& f, int x, int y) {
    std::uint8_t r, g, b;
    f.get(x, y, r, g, b);
    return std::max({ r, g, b }) <= kSlotBlackMax;
}

bool slot_light(const Frame& f, int x, int y) {
    std::uint8_t r, g, b;
    f.get(x, y, r, g, b);
    const int mx = std::max({ r, g, b });
    const int mn = std::min({ r, g, b });
    return mn >= kSlotLightMin && mx <= kSlotLightMax && mx - mn <= kSlotGreySpread;
}

bool frames_alike(const Frame& f, int xa, int xb, int y) {
    std::uint8_t ra, ga, ba, rb, gb, bb;
    f.get(xa, y, ra, ga, ba);
    f.get(xb, y, rb, gb, bb);
    return near(ra, rb, kSlotFramesAlike) && near(ga, gb, kSlotFramesAlike) && near(ba, bb, kSlotFramesAlike);
}

/// The red of a fill, as the left end of the pet's bar is at any HP but 0:
/// ad0800 to ff1008 down its rows, and ff948c along its top.
bool pet_red(const Frame& f, int x, int y) {
    std::uint8_t r, g, b;
    f.get(x, y, r, g, b);
    return r >= 128 && r >= g + 64 && r >= b + 64;
}

/// Whether a bar whose fill starts at `x` on row `y` sits in a companion
/// slot's frame: the black border right before it, the light grey frame
/// before that.
bool in_slot_frame(const Frame& f, int x, int y) {
    return x >= 2 && x < f.width && y >= 0 && y < f.height && slot_black(f, x - 1, y) &&
           slot_light(f, x - 2, y);
}

/// Whether a row with no run of fill in it is the empty part of a bar.
///
/// An empty bar is its trough with at most the caption printed across it, so
/// most of the row is one colour. Anything else with none of the fill in it
/// would read as an empty bar too, and 0% fires the potion: a loading
/// screen's or the login page's artwork, which changes from pixel to pixel; a
/// window in front, which is light; and a capture with nothing in it -- the
/// part of a window off the screen -- which is black. Black only as black as
/// a capture is, because this game's empty bar is near black at one HUD size;
/// and light only without a sliver of fill at the bar's left edge, because it
/// is light grey at the other, where that sliver is a bar nearly out. A fill
/// pixel anywhere else in a light row is a speck in whatever covers the bar.
bool trough_row(const Frame& f, int x, int y, int n, bool sliver) {
    if (n <= 0) return false;
    int hist[3][256] = {};
    for (int i = 0; i < n; ++i) {
        std::uint8_t r, g, b;
        f.get(x + i, y, r, g, b);
        ++hist[0][r]; ++hist[1][g]; ++hist[2][b];
    }
    // Each channel's median: the trough's own colour whenever the trough is
    // more than half the row, whatever is printed over the rest.
    std::uint8_t typical[3] = {};
    for (int c = 0; c < 3; ++c) {
        int seen = 0, v = 0;
        for (; v < 255; ++v) {
            seen += hist[c][v];
            if (seen * 2 > n) break;
        }
        typical[c] = static_cast<std::uint8_t>(v);
    }
    const int brightest = std::max({ typical[0], typical[1], typical[2] });
    if (brightest <= kCaptureBlack) return false;
    if (!sliver && brightest > kTroughLightAbove) return false;

    int alike = 0;
    for (int i = 0; i < n; ++i) {
        std::uint8_t r, g, b;
        f.get(x + i, y, r, g, b);
        if (near(r, typical[0], kTroughSpread) && near(g, typical[1], kTroughSpread) &&
            near(b, typical[2], kTroughSpread))
            ++alike;
    }
    return alike * 2 > n;
}

/// Pull the whole numbers out of a line of text, ignoring everything else.
///
/// Everything else is mostly the slash. Whether the font happens to have a
/// glyph for it does not matter: a run of digits is a number, anything that is
/// not a digit ends one, and "6744/9232", "6744 9232" and "6744 l 9232" all
/// come out as the same pair.
std::vector<long long> numbers_in(const std::string& text) {
    std::vector<long long> out;
    long long value = 0;
    bool in_number = false;
    bool overflowed = false;

    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            if (!in_number) { value = 0; in_number = true; overflowed = false; }
            if (value > 99'999'999) overflowed = true;
            value = value * 10 + (c - '0');
            continue;
        }
        if (in_number) {
            if (!overflowed) out.push_back(value);
            in_number = false;
        }
    }
    if (in_number && !overflowed) out.push_back(value);
    return out;
}

} // namespace

VitalsReader::VitalsReader() = default;
VitalsReader::~VitalsReader() { stop(); }

void VitalsReader::set_target_window(void* hwnd) { hwnd_.store(hwnd); }

// Each sets the landmark to be looked for again at the next read: the bars
// given -- a save, a start, the GUI's own copy -- go where it puts them at once,
// not after a dozen reads with the figures unread, two seconds without HP.
void VitalsReader::set_hp_bar(const BarSpec& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    hp_spec_ = s;
    anchor_found_ = false;
}

void VitalsReader::set_mp_bar(const BarSpec& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    mp_spec_ = s;
    anchor_found_ = false;
}

BarSpec VitalsReader::hp_bar() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hp_spec_;
}

BarSpec VitalsReader::mp_bar() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mp_spec_;
}

BarSpec VitalsReader::pet_bar() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pet_bar_;
}

void VitalsReader::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();

    // Load the font unasked. Reading the printed figures is the better half of
    // this class, and making the caller remember to switch it on would mean
    // most callers silently getting the worse half.
    if (!has_font()) load_font();

    // And the HUD landmark, likewise: the "Level" plate beside the bars,
    // hpmp_indicator2. Without it this falls back to scanning for coloured
    // runs, which works until it doesn't. From the same places every other
    // picture comes from, in the same order -- the pictures staged beside
    // GoBot first -- rather than only out of the other helper's assembly,
    // which a package does not carry.
    if (!has_anchor()) {
        Frame anchor;
        std::string from;
        if (TemplateStore::load_one("hpmp_indicator2", exe_relative("configs/templates.ini"),
                                    anchor, &from)) {
            set_anchor(anchor);
            GB_INFO("Vitals: using the HUD landmark from %s", from.c_str());
        } else {
            GB_INFO("Vitals: no picture of the HUD landmark anywhere -- finding the bars by "
                    "their colours");
        }
    }

    thread_ = std::thread(&VitalsReader::loop, this);
    GB_INFO("Vitals reader started (%s)",
            has_font() ? "reading the printed figures" : "measuring the bars");
}

void VitalsReader::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    hp_.store(-1.0f);
    mp_.store(-1.0f);
    pet_.store(-1.0f);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        values_ = Vitals{};
    }
    GB_INFO("Vitals reader stopped");
}

void VitalsReader::set_anchor(const Frame& anchor) {
    std::lock_guard<std::mutex> lock(mutex_);
    anchor_ = anchor;
    anchor_found_ = false;
}

bool VitalsReader::has_anchor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return anchor_.valid();
}

void VitalsReader::set_layout(const HudLayout& l) {
    std::lock_guard<std::mutex> lock(mutex_);
    layout_ = l;
}

VitalsReader::HudLayout VitalsReader::layout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return layout_;
}

ScreenRect VitalsReader::anchor_at() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return anchor_found_ ? anchor_at_ : ScreenRect{};
}

bool VitalsReader::reads_as_fill(const BarSpec& s, bool hp) {
    // Its own channel strong, and well over the others: red for HP, blue for
    // MP. MP's green runs high in the middle of the bar (0,181,255), so only
    // red is held under blue there.
    if (hp) return s.r >= 120 && s.r >= s.g + 80 && s.r >= s.b + 80;
    return s.b >= 120 && s.b >= s.r + 80;
}

bool VitalsReader::locate_anchor(const Frame& f, bool* recoloured) {
    // Only the top-left corner of the window. The HUD lives there, and
    // searching the whole screen for a thirty-six pixel plate would turn up
    // every similar scrap of interface in the game.
    const ScreenRect look{ 0, 0, std::min(f.width, 560), std::min(f.height, 280) };
    const auto hit = TemplateMatcher::find(f, look, anchor_, 0.75f);
    if (!hit.found) return false;

    // The pet's bar goes with the HUD, as HP and MP do: where it was against
    // the landmark. Whether it is still there is for the next read to see
    // (pet_frame_holds), and to look for it again if not.
    if (anchor_found_ && pet_watch_.bar.valid()) {
        pet_watch_.bar.x += hit.at.x - anchor_at_.x;
        pet_watch_.bar.y += hit.at.y - anchor_at_.y;
    }
    anchor_at_ = hit.at;
    anchor_found_ = true;

    const struct { int dx, dy; BarSpec* spec; } kBars[] = {
        { layout_.hp_dx, layout_.hp_dy, &hp_spec_ },
        { layout_.mp_dx, layout_.mp_dy, &mp_spec_ },
    };
    for (const auto& bar : kBars) {
        bar.spec->x = hit.at.x + bar.dx;
        bar.spec->y = hit.at.y + bar.dy;
        bar.spec->width = layout_.bar_w;
        bar.spec->height = layout_.bar_h;
        // The fill colour is left as configured. It is a property of the game's
        // artwork, not of where the bar happens to be, and sampling it from a
        // bar that might be empty would learn the colour of the trough.
    }

    // Unless what is configured is no fill colour of its bar: a colour learned
    // off something else -- the server list's sky, on 2026-10-27, which the
    // GUI took into its copy of the bars, and a save would have written -- and
    // the bar measures nothing for good. The game's own instead, said once.
    const struct { BarSpec* spec; bool hp; FillColour fill; const char* name; } kFills[] = {
        { &hp_spec_, true, kHpFill, "HP" },
        { &mp_spec_, false, kMpFill, "MP" },
    };
    for (const auto& b : kFills) {
        if (reads_as_fill(*b.spec, b.hp)) continue;
        GB_INFO("Vitals: the %s bar's colour %u,%u,%u is no fill colour of it -- reading it as %u,%u,%u again",
                b.name, static_cast<unsigned>(b.spec->r), static_cast<unsigned>(b.spec->g),
                static_cast<unsigned>(b.spec->b), static_cast<unsigned>(b.fill.r), static_cast<unsigned>(b.fill.g),
                static_cast<unsigned>(b.fill.b));
        b.spec->r = b.fill.r;
        b.spec->g = b.fill.g;
        b.spec->b = b.fill.b;
        b.spec->tolerance = kFillTolerance;
        if (recoloured) *recoloured = true;
    }
    return true;
}

bool VitalsReader::load_font(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    return font_.load(dir.empty() ? GlyphFont::default_dir() : dir);
}

bool VitalsReader::has_font() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return font_.loaded();
}

VitalsReader::Vitals VitalsReader::values() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return values_;
}

ScreenRect VitalsReader::caption_box(const BarSpec& spec) {
    // Sixteen rows tall, anchored seven above the bar's middle. Measured
    // against this game at its default HUD size: an eight-pixel digit sitting
    // two below the middle, with clear rows either side for the reader to see
    // the outline against.
    //
    // Horizontally it runs from the middle of the bar to well past the right
    // edge, and the overshoot is the point.
    //
    // The caption is right-aligned inside the bar, so anchoring the box to the
    // bar's right edge is the obvious thing and it is wrong: the right edge is
    // not reliably known. It is worked out by measuring the coloured fill and
    // then the empty trough after it -- and when the fill happens to end
    // underneath the caption, the trough scan starts on white text, takes that
    // for the trough colour and stops at once. The bar then measures 115 wide
    // instead of 159, the box slides forty pixels left, and the digits fall
    // outside it. Nothing reports an error: the figures simply stop updating
    // and the last good pair sits there looking current.
    //
    // Reaching past the edge costs a few milliseconds of reading and removes
    // the dependency. The row is the bar's own, so the only writing in there
    // is the caption.
    const int middle = spec.y + spec.height / 2;
    const int from   = spec.x + spec.width / 2;
    const int to     = spec.right() + 45;
    return ScreenRect{ from, middle - 7, std::max(60, to - from), 16 };
}

bool VitalsReader::read_numbers(const Frame& f, const BarSpec& spec,
                                const GlyphFont& font, int& current, int& maximum) {
    if (!f.valid() || !spec.valid() || !font.loaded()) return false;

    const ScreenRect box = caption_box(spec);
    if (box.x < 0 || box.y < 0 || box.right() > f.width || box.bottom() > f.height)
        return false;

    const std::string line = font.read_line(f, box);

    // Group the digits by where they sit, not by the spaces the reader put in.
    //
    // The reader decides a space has happened when the gap between two glyphs
    // is wide enough, which is the only thing it can do -- and for this caption
    // it is not good enough. The game lays its digits out on a fixed pitch, six
    // pixels apart, while the glyph files are cropped to their own ink. A '1'
    // is two pixels of ink in a six pixel cell, so the gap after a '1' is just
    // as wide as the gap across the '/'. "6710/9232" comes back as
    // "671 0 9232": three numbers, the first of them wrong.
    //
    // It took a while to see because it depends on the digits. 6744, 6737 and
    // 6720 all read perfectly; 6710 did not, and HP quietly stopped updating
    // while MP carried on, because MP happened to hold no ones.
    //
    // Position settles it where spacing cannot. Within a number the step from
    // one digit to the next is one cell; across the slash it is two, because
    // the slash occupies one. So: step up to about one and a half cells means
    // the same number, more than that means the next one.
    std::vector<std::pair<int, char>> digits;
    for (const GlyphFont::Placed& p : font.last_placed()) {
        if (p.ch.size() != 1) continue;
        if (p.ch[0] < '0' || p.ch[0] > '9') continue;
        digits.emplace_back(p.box.x, p.ch[0]);
    }

    std::vector<long long> got;
    if (digits.size() >= 2) {
        std::vector<int> steps;
        for (std::size_t i = 1; i < digits.size(); ++i)
            steps.push_back(digits[i].first - digits[i - 1].first);
        std::vector<int> sorted = steps;
        std::sort(sorted.begin(), sorted.end());
        // The typical step is one cell. Anything materially wider is the next
        // number starting. The threshold has to sit above one cell and below
        // a cell plus whatever separates the two figures -- in the game that
        // separator is a '/' and the step across it is two cells, so a third
        // of a cell of headroom is plenty and leaves room for a separator
        // narrower than a digit.
        const int cell = std::max(1, sorted[sorted.size() / 2]);
        const int apart = cell + std::max(2, cell / 3);

        std::string number(1, digits[0].second);
        for (std::size_t i = 1; i < digits.size(); ++i) {
            if (steps[i - 1] > apart) {
                got.push_back(std::strtoll(number.c_str(), nullptr, 10));
                number.clear();
            }
            number.push_back(digits[i].second);
        }
        if (!number.empty()) got.push_back(std::strtoll(number.c_str(), nullptr, 10));
    } else {
        got = numbers_in(line);
    }

    // The first two, in the right order.
    //
    // Not "exactly two", which is what this asked for at first: the box has to
    // reach past where the bar is measured to end, because that measurement is
    // not reliable, and reaching past it can catch the edge of whatever the HUD
    // puts beside the bars. The first two are the right two -- the box starts
    // in the middle of the bar, where there is nothing but flat colour, so the
    // caption is the leftmost writing in it.
    if (got.size() < 2) return false;
    if (got[1] <= 0 || got[0] < 0 || got[0] > got[1]) return false;
    if (got[1] > 99'999'999) return false;

    current = static_cast<int>(got[0]);
    maximum = static_cast<int>(got[1]);
    return true;
}

float VitalsReader::measure(const Frame& f, const BarSpec& spec) {
    if (!f.valid() || !spec.valid()) return -1.0f;

    // Sample a few rows through the middle of the bar and take the median
    // fill. A single row can be caught by a highlight or a border; three
    // disagreeing rows mean the region is not actually on the bar.
    const int rows[3] = {
        spec.y + spec.height / 2,
        spec.y + spec.height / 3,
        spec.y + (spec.height * 2) / 3,
    };

    float fills[3];
    int taken = 0;

    for (int yi = 0; yi < 3; ++yi) {
        const int y = rows[yi];
        if (y < 0 || y >= f.height) continue;

        // Walk the row once, recording which pixels are the fill colour.
        int width = 0;
        bool match[kMaxBarWidth];

        for (int i = 0; i < spec.width && i < kMaxBarWidth; ++i) {
            const int x = spec.x + i;
            if (x < 0 || x >= f.width) break;

            std::uint8_t r, g, b;
            f.get(x, y, r, g, b);

            match[width++] = near(r, spec.r, spec.tolerance) &&
                             near(g, spec.g, spec.tolerance) &&
                             near(b, spec.b, spec.tolerance);
        }
        if (width == 0) continue;

        // The fill ends at the rightmost run of fill-coloured pixels.
        //
        // Counted from the right rather than forward from the left, because
        // the game prints the numbers over the bar in white and a forward scan
        // stops dead at the first glyph -- reading a full bar as two thirds.
        // Coming back from the far end, the writing is just more of the stuff
        // that is not fill, and the first real fill met is the end of it.
        //
        // A run rather than a single pixel, so a stray matching pixel out in
        // the empty part cannot stretch the reading to the full width.
        int filled = 0;
        int run = 0;
        for (int i = width - 1; i >= 0; --i) {
            if (!match[i]) { run = 0; continue; }
            if (++run >= 3) { filled = i + run; break; }
        }

        // A row counts only when it positively looks like a bar. An empty bar
        // and a region that is not a bar at all both match nothing, and
        // reporting them the same way is the one mistake that matters here:
        // "0%" makes a potion key fire, and firing because the reader lost the
        // window is worse than not firing.
        if (filled > 0) {
            // A fill is drawn from the bar's left edge and only its caption is
            // printed over it, so up to where it ends it is mostly fill colour.
            // Three such pixels among others that are not -- a speck of red in
            // some picture -- are no fill.
            int lit = 0;
            for (int i = 0; i < filled; ++i)
                if (match[i]) ++lit;
            if (lit * 2 < filled) continue;
        } else {
            // No fill to speak of: empty, or nearly -- or no bar at all.
            // A bar with only a sliver left has no room for a run of three.
            for (int i = width - 1; i >= 0; --i)
                if (match[i] && i < 4) { filled = i + 1; break; }
            if (!trough_row(f, spec.x, y, width, filled > 0)) continue;
        }

        fills[taken++] = static_cast<float>(filled) / static_cast<float>(width);
    }

    if (taken == 0) return -1.0f;
    std::sort(fills, fills + taken);
    return std::clamp(fills[taken / 2] * 100.0f, 0.0f, 100.0f);
}

void VitalsReader::loop() {
    ScreenCapture capture;
    Frame frame;

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (!running_.load()) break;

        void* hwnd = hwnd_.load();
        if (!hwnd) continue;

        if (!capture.grab(hwnd, frame)) {
            // The values stay as they were read last, and last_read_ms()
            // with them: whoever acts on them can tell how old they are.
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.failures;
            failed_at_.push_back(now_ms());
            while (failed_at_.size() > kFailuresKept) failed_at_.pop_front();
            continue;
        }

        BarSpec hp, mp, pet_was;
        bool anchored = false;
        bool recoloured = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_ = frame;

            // Find the landmark the first time, and again whenever the figures
            // have stopped being readable -- which is the only evidence that
            // the HUD has moved. Not every frame: it does not move while the
            // HUD is up, and looking for it costs more than reading the
            // figures does.
            if (anchor_.valid()) {
                // And at once when HP or MP is set on a bar in a companion
                // slot's frame -- the pet's, which "Tự tìm" saved as HP on
                // 2026-10-26 and which a Save hands back to the reader: read
                // there until the figures had gone unread a dozen times, the
                // pet's HP was the character's.
                const bool on_slot = in_slot_frame(frame, hp_spec_.x, hp_spec_.y + hp_spec_.height / 2) ||
                                     in_slot_frame(frame, mp_spec_.x, mp_spec_.y + mp_spec_.height / 2);
                const bool lost = !anchor_found_ ||
                                  hp_misses_ >= kForgetAfterMisses ||
                                  mp_misses_ >= kForgetAfterMisses || on_slot;
                if (lost) {
                    const bool had = anchor_found_;
                    if (locate_anchor(frame, &recoloured) && !had)
                        GB_INFO("Vitals: HUD landmark at %d,%d -- bars at %d,%d and %d,%d",
                                anchor_at_.x, anchor_at_.y,
                                hp_spec_.x, hp_spec_.y, mp_spec_.x, mp_spec_.y);
                }
                anchored = anchor_found_;
            }

            hp = hp_spec_;
            mp = mp_spec_;
            // The pet's bar, moved with the landmark just now if it moved.
            pet_bar_ = pet_watch_.bar;
            pet_was = pet_bar_;
        }
        // A colour put back is counted as the bars found again: the GUI takes
        // them into its copy, so a save writes the game's colour and not the
        // one that was wrong.
        if (recoloured) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.relocations;
        }

        float hp_pct = measure(frame, hp);
        float mp_pct = measure(frame, mp);

        // The pet's, off its own bar under them: looked for, kept and read
        // by pet_watch_ (see PetWatch). Where it is goes to the window, and
        // into the log when it is found or has moved.
        const float pet_pct = pet_watch_.read(frame, hp, mp, now_ms());
        {
            const BarSpec& pet = pet_watch_.bar;
            if (pet.x != pet_was.x || pet.y != pet_was.y || pet.width != pet_was.width ||
                pet.height != pet_was.height) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pet_bar_ = pet;
                }
                if (pet.valid() && (!pet_was.valid() || bars_differ(pet, pet_was)))
                    GB_INFO("Vitals: the pet's HP bar at %d,%d, %d wide and %d high", pet.x, pet.y, pet.width,
                            pet.height);
            }
        }

        // The printed figures, where they can be had. They override the bar
        // measurement rather than sitting beside it: the measurement is known
        // to read a full bar as two-thirds full, because the caption is
        // painted over the end of the fill.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (font_.loaded()) {
                bool any = false;
                const struct {
                    const BarSpec& spec;
                    int& value; int& max;
                    int& pending; int& seen; int& misses;
                    float& percent;
                } kBars[] = {
                    { hp, values_.hp, values_.hp_max, hp_max_pending_, hp_max_seen_,
                      hp_misses_, hp_pct },
                    { mp, values_.mp, values_.mp_max, mp_max_pending_, mp_max_seen_,
                      mp_misses_, mp_pct },
                };

                for (const auto& bar : kBars) {
                    int cur = 0, max = 0;
                    if (!read_numbers(frame, bar.spec, font_, cur, max)) {
                        // Forget the figures rather than keep showing them.
                        //
                        // A reading that stops updating is the worst of the
                        // three possible states: no reading announces itself,
                        // a wrong reading usually looks wrong, but a stale one
                        // looks exactly like a working one. Given a few
                        // seconds of failure, say so instead.
                        if (++bar.misses >= kForgetAfterMisses) {
                            bar.value = -1;
                            bar.max = -1;
                            bar.pending = -1;
                            bar.seen = 0;
                        }
                        continue;
                    }
                    bar.misses = 0;
                    any = true;

                    // A maximum has to be seen twice before it is believed;
                    // the current value is taken as it comes.
                    if (max == bar.max) {
                        bar.pending = -1;
                        bar.seen = 0;
                    } else if (max == bar.pending) {
                        if (++bar.seen >= 2) { bar.max = max; bar.pending = -1; bar.seen = 0; }
                    } else {
                        bar.pending = max;
                        bar.seen = 1;
                    }

                    bar.value = cur;
                    const int against = bar.max > 0 ? bar.max : max;
                    bar.percent = std::clamp(
                        static_cast<float>(cur) * 100.0f / static_cast<float>(against),
                        0.0f, 100.0f);
                }

                if (any) {
                    std::lock_guard<std::mutex> slock(stats_mutex_);
                    ++stats_.number_reads;
                }
            } else if (!warned_no_font_) {
                warned_no_font_ = true;
                GB_WARN("Vitals: no bitmap font loaded, so HP and MP are measured "
                        "from the bars alone. The game prints its figures over "
                        "the bars, which makes a full bar measure short.");
            }
        }

        // One clock for all three: a reader of hp_seen_ms() next to
        // last_read_ms() sees the two agree. hp_seen_ms_ first, so that one who
        // reads last_read_ms() first never pairs a new read with an old sight.
        const std::uint64_t read_at = now_ms();
        hp_.store(hp_pct);
        mp_.store(mp_pct);
        pet_.store(pet_pct);
        hp_seen_ms_.store(hp_seen_after(hp_seen_ms_.load(), hp_pct, read_at));
        read_at_ms_.store(read_at);

        // The first read that makes out no HP after one that did: its frame is
        // kept, for what was on it -- the HUD covered, a frame off the screen
        // that was not the game, a bar that blinked. After the read is out,
        // and written off this thread: every potion and the in-world hold
        // wait on this thread's next read.
        if (hp_pct < 0.0f && hp_was_read_) keep_unreadable_frame(frame, capture.from_screen(), mp_pct);
        hp_was_read_ = hp_pct >= 0.0f;

        // Notice when the bars stop being where they were put.
        //
        // The game's HUD can be resized from its own settings, which moves the
        // bars and changes their length. Nothing about that makes the reading
        // fail -- it carries on reporting the wrong fraction of the wrong bar,
        // which is how a potion rule ends up firing at the wrong moment with
        // no sign that anything is wrong.
        // Working the bars out again by scanning for coloured runs. Only when
        // there is no landmark to go by: the landmark answers the same question
        // without the failure mode that scan has, where a bar whose fill ends
        // under the printed figures measures three quarters of its true width.
        if (auto_relocate_.load() && !anchored) {
            const std::uint64_t now = now_ms();
            if (now - last_check_ms_ > kCheckGapMs) {
                last_check_ms_ = now;

                // Compare against a fresh look rather than probing the pixels
                // just past the bar's edge. That probe lands in the numbers the
                // game prints across the bar, which are white with a black
                // outline and leave almost no fill colour showing on the rows
                // it samples -- so a bar that is plainly too short reads as
                // perfectly fine. Finding the bars again and seeing whether the
                // answer matches asks the question directly (relocated).
                BarSpec new_hp, new_mp;
                const bool differs = relocated(frame, hp, mp, new_hp, new_mp);

                disagreements_ = differs ? disagreements_ + 1 : 0;

                // HP or MP set on a bar in a companion slot's frame -- the
                // pet's, as "Tự tìm" once saved them -- is wrong on this very
                // frame, which shows the HUD: no three looks are waited for.
                const bool on_slot = in_slot_frame(frame, hp.x, hp.y + hp.height / 2) ||
                                     in_slot_frame(frame, mp.x, mp.y + mp.height / 2);
                if (disagreements_ >= kDisagreements || (differs && on_slot)) {
                    disagreements_ = 0;
                    GB_INFO("HP/MP bars moved -- found them again at "
                            "%d,%d %dx%d and %d,%d %dx%d",
                            new_hp.x, new_hp.y, new_hp.width, new_hp.height,
                            new_mp.x, new_mp.y, new_mp.width, new_mp.height);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        hp_spec_ = new_hp;
                        mp_spec_ = new_mp;
                    }
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    ++stats_.relocations;
                }
            }
        }

        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.reads;
        stats_.capture_from_screen = capture.from_screen();
    }
}

bool VitalsReader::bars_differ(const BarSpec& a, const BarSpec& b) {
    return std::abs(a.x - b.x) > 3 || std::abs(a.y - b.y) > 3 ||
           std::abs(a.height - b.height) > 3 ||
           std::abs(a.width - b.width) > std::max(6, b.width / 10);
}

bool VitalsReader::relocated(const Frame& f, const BarSpec& hp, const BarSpec& mp, BarSpec& new_hp,
                             BarSpec& new_mp) {
    // Two bars, and drawn as the game draws HP and MP (bars_paired). find_bars
    // falls back to the topmost of each colour when it finds no such pair --
    // an answer for the full screen's FIND THE BARS, which draws what it found
    // for the player to judge -- and on 2026-10-27 three looks at the server
    // list, before the HUD was up, took two strips of sky 323 px apart for the
    // bars. Their colours stayed with the bars when the landmark put them
    // back, and HP measured nothing from then on.
    new_hp = hp;
    new_mp = mp;
    if (find_bars(f, new_hp, new_mp) != 2 || !bars_paired(new_hp, new_mp)) return false;
    return bars_differ(new_hp, hp) || bars_differ(new_mp, mp);
}

bool VitalsReader::bars_paired(const BarSpec& hp, const BarSpec& mp) {
    if (!hp.valid() || !mp.valid()) return false;
    const int narrow = std::min(hp.width, mp.width), wide = std::max(hp.width, mp.width);
    return std::abs(hp.x - mp.x) <= 4 && mp.y > hp.y && mp.y - hp.bottom() <= 44 && wide <= narrow * 2;
}

bool VitalsReader::bar_is_stale(const Frame& f, const BarSpec& spec) {
    if (!f.valid() || !spec.valid()) return false;
    if (measure(f, spec) < 0.0f) return true;

    // The fill running on past the right edge means the bar is longer than the
    // spec claims. Started a couple of pixels in, so a border or an antialiased
    // edge does not read as more bar.
    //
    // Several rows and a proportion, not one row and a run: the game prints its
    // numbers across the bar in white with a black outline, and a single row
    // through the middle of that writing has almost no fill colour left on it.
    int best = 0, looked = 0;
    for (int dy = 1; dy < std::max(2, spec.height - 1); ++dy) {
        const int y = spec.y + dy;
        if (y < 0 || y >= f.height) continue;
        int beyond = 0;
        looked = 0;
        for (int i = 2; i < 20; ++i) {
            const int x = spec.right() + i;
            if (x >= f.width) break;
            ++looked;
            std::uint8_t r, g, b;
            f.get(x, y, r, g, b);
            if (near(r, spec.r, spec.tolerance) && near(g, spec.g, spec.tolerance) &&
                near(b, spec.b, spec.tolerance))
                ++beyond;
        }
        best = std::max(best, beyond);
    }
    return looked >= 12 && best * 5 >= looked * 2;
}

bool VitalsReader::latest_frame(Frame& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame_.valid()) return false;
    out = frame_;
    return true;
}

bool VitalsReader::sample_pixel(int x, int y, std::uint8_t& r, std::uint8_t& g,
                                std::uint8_t& b) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame_.valid()) return false;
    if (x < 0 || y < 0 || x >= frame_.width || y >= frame_.height) return false;
    frame_.get(x, y, r, g, b);
    return true;
}

bool VitalsReader::autodetect_colour(const BarSpec& region, std::uint8_t& out_r,
                                     std::uint8_t& out_g, std::uint8_t& out_b) const {
    Frame f;
    if (!latest_frame(f) || !region.valid()) return false;

    // The fill is the dominant saturated colour inside the region: the empty
    // part is dark, the border is grey, and neither survives a saturation
    // test. Quantising groups the gradient a bar is usually drawn with.
    std::map<std::uint32_t, int> votes;

    for (int y = region.y; y < region.y + region.height; ++y) {
        for (int x = region.x; x < region.x + region.width; ++x) {
            std::uint8_t r, g, b;
            f.get(x, y, r, g, b);

            const int mx = std::max({ r, g, b });
            const int mn = std::min({ r, g, b });
            if (mx < 70 || mx - mn < 40) continue;    // too dark, or too grey

            const std::uint32_t key = (static_cast<std::uint32_t>(r >> 4) << 8) |
                                      (static_cast<std::uint32_t>(g >> 4) << 4) |
                                       static_cast<std::uint32_t>(b >> 4);
            ++votes[key];
        }
    }

    if (votes.empty()) return false;

    auto best = std::max_element(votes.begin(), votes.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    // Average the actual pixels in the winning bucket, so the result is a real
    // colour rather than the centre of a quantisation cell.
    long long sr = 0, sg = 0, sb = 0;
    int n = 0;
    for (int y = region.y; y < region.y + region.height; ++y) {
        for (int x = region.x; x < region.x + region.width; ++x) {
            std::uint8_t r, g, b;
            f.get(x, y, r, g, b);
            const std::uint32_t key = (static_cast<std::uint32_t>(r >> 4) << 8) |
                                      (static_cast<std::uint32_t>(g >> 4) << 4) |
                                       static_cast<std::uint32_t>(b >> 4);
            if (key != best->first) continue;
            sr += r; sg += g; sb += b; ++n;
        }
    }
    if (n == 0) return false;

    out_r = static_cast<std::uint8_t>(sr / n);
    out_g = static_cast<std::uint8_t>(sg / n);
    out_b = static_cast<std::uint8_t>(sb / n);
    return true;
}

namespace {

/// One horizontal run of flat saturated colour, with however much dark trough
/// follows it.
struct Run {
    int y = 0, x = 0, len = 0, trough = 0;
    std::uint8_t r = 0, g = 0, b = 0;
};

/// Runs stacked vertically at the same place: a whole bar.
///
/// Rows are grouped by shape rather than by colour. These bars are steep
/// vertical gradients -- the mana bar runs 0,8,123 at its top edge through
/// 0,181,255 in the middle and back down -- so neighbouring rows differ by
/// more than any sane colour tolerance, while the geometry stays identical
/// row after row. What makes it one bar is that every row starts at the same
/// x, is the same length, and leans the same way.
struct Band {
    int x = 0, y0 = 0, y1 = 0;
    int len = 0, trough = 0;
    int rows = 0;
    int dominant = 0;

    /// Colour at the vertical middle, which is where the gradient is flattest
    /// and where `measure` takes its samples.
    std::uint8_t r = 0, g = 0, b = 0;

    int middle() const { return (y0 + y1) / 2; }
};

int redness(const Band& x)  { return x.r - (x.g + x.b) / 2; }
int blueness(const Band& x) { return x.b - (x.r + x.g) / 2; }

/// 0 red, 1 green, 2 blue. Used to stop a run where the hue changes, which a
/// per-pixel difference alone would walk straight through given enough small
/// steps.
int dominant_channel(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (r >= g && r >= b) return 0;
    if (g >= b) return 1;
    return 2;
}

} // namespace

int VitalsReader::find_bars(const Frame& f, BarSpec& hp, BarSpec& mp) {
    if (!f.valid()) return 0;

    // Bars live in a HUD panel at the top of the window. Searching the whole
    // frame would turn up every saturated horizontal edge in the artwork.
    const int maxy = std::min(f.height, 220);
    const int maxx = std::min(f.width, 600);

    // Short enough that a badly drained bar still registers, long enough that
    // the HUD's small decorations do not.
    constexpr int kMinLen = 20;

    // How much writing a run will step over. The numbers the game prints
    // across its bars are the only thing that interrupts one.
    constexpr int kGapBridge = 18;

    // A bar is never this long; the cap stops a trough scan from walking the
    // width of the HUD when the empty part and the panel behind it match.
    constexpr int kMaxTrough = 400;

    std::vector<Run> runs;
    for (int y = 0; y < maxy; ++y) {
        int x = 0;
        while (x < maxx) {
            std::uint8_t r, g, b;
            f.get(x, y, r, g, b);
            const int mx = std::max({ r, g, b });
            const int mn = std::min({ r, g, b });
            if (mx < 100 || mx - mn < 60) { ++x; continue; }   // not a bar colour

            const int dominant = dominant_channel(r, g, b);

            // Extend while the colour keeps the same character, comparing each
            // pixel against the one before it rather than against the start.
            //
            // These bars are not flat: this one runs 214,0,0 at its left edge
            // to 247,0,0 forty pixels along. Measuring against the start broke
            // every bar into pieces too short to recognise. A gradient moves a
            // little per pixel; an edge jumps, and the saturation and hue
            // tests below stop the run at the trough or the panel either way.
            const auto is_fill = [&](int px, std::uint8_t ar, std::uint8_t ag,
                                     std::uint8_t ab) {
                std::uint8_t r2, g2, b2;
                f.get(px, y, r2, g2, b2);
                const int mx2 = std::max({ r2, g2, b2 });
                const int mn2 = std::min({ r2, g2, b2 });
                if (mx2 < 100 || mx2 - mn2 < 60) return false;
                if (dominant_channel(r2, g2, b2) != dominant) return false;
                return near(r2, ar, 20) && near(g2, ag, 20) && near(b2, ab, 20);
            };

            int end = x + 1;
            std::uint8_t pr = r, pg = g, pb = b;
            for (;;) {
                while (end < maxx) {
                    std::uint8_t r2, g2, b2;
                    f.get(end, y, r2, g2, b2);
                    if (!is_fill(end, pr, pg, pb)) break;
                    pr = r2; pg = g2; pb = b2;
                    ++end;
                }
                // Step over writing. The game prints the numbers across the
                // bar, and a run that stops at the first glyph measures the
                // distance to the text rather than the length of the bar.
                int gap = 1;
                while (gap <= kGapBridge && end + gap < maxx &&
                       !is_fill(end + gap, pr, pg, pb)) ++gap;
                if (gap > kGapBridge || end + gap >= maxx) break;
                end += gap;
            }

            const int len = end - x;
            if (len >= kMinLen) {
                // However much of the bar is empty sits immediately to the
                // right as trough, and counts toward the full width.
                //
                // Matched against the trough's own colour rather than just
                // "dark", because the panel behind the bar is usually dark
                // too: a brightness test walks straight off the end of the
                // bar and across the whole HUD.
                int t = 0;
                std::uint8_t tr = 0, tg = 0, tb = 0;
                if (end < maxx) {
                    f.get(end, y, tr, tg, tb);
                    // Flat and not itself a bar colour. Not "dark": this game's
                    // empty bar is light grey at one HUD size and near black at
                    // another, and only flatness holds for both.
                    const int tmx = std::max({ tr, tg, tb });
                    const int tmn = std::min({ tr, tg, tb });
                    if (tmx - tmn < 60) {
                        const auto is_trough = [&](int px) {
                            std::uint8_t r3, g3, b3;
                            f.get(px, y, r3, g3, b3);
                            return near(r3, tr, 12) && near(g3, tg, 12) && near(b3, tb, 12);
                        };
                        for (;;) {
                            while (end + t < maxx && t < kMaxTrough && is_trough(end + t)) ++t;
                            int gap = 1;
                            while (gap <= kGapBridge && end + t + gap < maxx &&
                                   !is_trough(end + t + gap)) ++gap;
                            if (gap > kGapBridge || end + t + gap >= maxx ||
                                t + gap >= kMaxTrough) break;
                            t += gap;
                        }
                    }
                }
                runs.push_back(Run{ y, x, len, t, r, g, b });
            }
            x = end;
        }
    }

    // Stack runs that line up into bands. A one-row run is a highlight or an
    // edge in the artwork; a bar is several rows of the same thing.
    std::vector<Band> bands;
    for (const Run& run : runs) {
        const int run_dominant = dominant_channel(run.r, run.g, run.b);

        Band* into = nullptr;
        for (Band& band : bands) {
            if (band.y1 + 1 != run.y) continue;
            if (band.dominant != run_dominant) continue;
            if (std::abs(band.x - run.x) > 2) continue;
            // Comparable length. A row may come up short -- the game prints
            // the numbers over the bar, and that row's run stops at the text
            // -- but a row that runs materially longer than the rest is a
            // different thing that happens to start in the same place.
            if (run.len > band.len + band.len / 8 + 4) continue;
            if (run.len * 2 < band.len) continue;
            into = &band;
            break;
        }

        if (into) {
            into->y1 = run.y;
            ++into->rows;
            // Keep the longest run seen: a row crossed by the text drawn over
            // the bar reads short.
            if (run.len > into->len) { into->len = run.len; into->trough = run.trough; }
        } else {
            Band band;
            band.x = run.x;
            band.y0 = band.y1 = run.y;
            band.len = run.len;
            band.trough = run.trough;
            band.rows = 1;
            band.dominant = run_dominant;
            bands.push_back(band);
        }
    }

    // Take each band's colour from its middle row, now that the extent is
    // known. The edges of these bars are much darker than the body, and a
    // colour taken from an edge matches almost none of the fill.
    for (Band& band : bands) {
        for (const Run& run : runs) {
            if (run.y != band.middle() || std::abs(run.x - band.x) > 2) continue;
            band.r = run.r; band.g = run.g; band.b = run.b;
            break;
        }
    }

    // Pick the two as a pair, not independently.
    //
    // Picking the topmost red band and the topmost blue one separately finds
    // the sky: it is blue, it is saturated, and a strip of it above the HUD
    // panel beats the real MP bar on height alone. What is distinctive about
    // HP and MP is not either bar on its own but their arrangement -- one red
    // and one blue, the same width, starting at the same x, stacked a few
    // pixels apart. Nothing in the artwork looks like that.
    std::vector<const Band*> reds, blues;
    for (const Band& band : bands) {
        if (band.rows < 4 || band.len < kMinLen) continue;
        // Never a bar of the companion slots under the HUD: the pet's are a
        // red one over a purple one, the shape of HP over MP, and were taken
        // for them on 2026-10-26. They sit in a black border inside a light
        // grey frame; HP's and MP's edge is slate.
        if (in_slot_frame(f, band.x, band.middle())) continue;
        if (redness(band)  > 60) reds.push_back(&band);
        if (blueness(band) > 50) blues.push_back(&band);
    }

    const Band* best_hp = nullptr;
    const Band* best_mp = nullptr;
    for (const Band* red : reds) {
        for (const Band* blue : blues) {
            if (std::abs(red->x - blue->x) > 4) continue;

            // MP under HP, close enough to be the same widget.
            const int gap = blue->y0 - red->y1;
            if (gap < 0 || gap > 40) continue;

            // Same width. The fills differ, but fill plus trough should not.
            const int wr = red->len + red->trough;
            const int wb = blue->len + blue->trough;
            if (std::max(wr, wb) > std::min(wr, wb) * 2) continue;

            if (!best_hp || red->y0 < best_hp->y0) { best_hp = red; best_mp = blue; }
        }
    }

    // No pair: fall back to the topmost of each colour. Worse, but a wrong
    // answer the user can see and correct beats refusing to answer.
    if (!best_hp) {
        for (const Band* red : reds)
            if (!best_hp || red->y0 < best_hp->y0) best_hp = red;
        for (const Band* blue : blues)
            if (!best_mp || blue->y0 < best_mp->y0) best_mp = blue;
    }

    int found = 0;
    const struct { const Band* band; BarSpec* spec; } kOut[] = {
        { best_hp, &hp }, { best_mp, &mp },
    };
    for (const auto& o : kOut) {
        if (!o.band) continue;

        // Read only the middle of the bar, not the whole of it. The gradient
        // is flat there and steep at the edges, so a band of a few rows around
        // the centre matches one colour cleanly where the full height cannot.
        const int tall = o.band->y1 - o.band->y0 + 1;
        const int keep = std::min(5, tall);
        o.spec->y = std::clamp(o.band->middle() - keep / 2,
                               o.band->y0, o.band->y1 - keep + 1);
        o.spec->height = keep;

        o.spec->x = o.band->x;
        o.spec->width = o.band->len + o.band->trough;
        o.spec->r = o.band->r;
        o.spec->g = o.band->g;
        o.spec->b = o.band->b;
        // Wider than the hand-set default: even across the middle rows the
        // colour moves, and the trough is far enough away to stay excluded.
        o.spec->tolerance = 80;
        ++found;
    }

    // Work out the full width from both bars together.
    //
    // Neither bar alone settles it. The fill is a lower bound -- the bar is at
    // least as wide as the coloured part. Fill plus trough is an upper bound,
    // and a loose one: a bar that happens to be full has no trough, so the
    // scan runs on across the panel behind it.
    //
    // The two bars are drawn as a matched pair, so one answer serves both, and
    // the bounds from each constrain the other. When they agree the tighter
    // upper bound is a genuine refinement; when they are far apart one of the
    // troughs ran into the background and only the lower bound can be trusted.
    //
    // What this cannot recover is two bars drained by a similar amount, where
    // every bound is short -- hence the panel asking for full health, and
    // showing the measured percentage so a bad answer is obvious.
    if (found == 2 && hp.x == mp.x) {
        const int lo = std::max(best_hp->len, best_mp->len);
        const int hi = std::min(best_hp->len + best_hp->trough,
                                best_mp->len + best_mp->trough);
        hp.width = mp.width = (hi >= lo && hi <= lo + lo / 3) ? hi : lo;
    } else if (found == 1) {
        const Band* only = best_hp ? best_hp : best_mp;
        BarSpec* spec = best_hp ? &hp : &mp;
        // Alone, the trough is only believable when it is short enough to be
        // the rest of a bar rather than the whole HUD.
        if (only->trough > only->len / 3) spec->width = only->len;
    }

    // What was actually in the running, not just the tally. When this picks
    // the wrong thing the only useful question is what else it was looking at,
    // and that can only be asked after the fact.
    //
    // At DEBUG, though. Every candidate band at INFO came to 71,029 of the
    // 80,357 lines in one session's log -- 88% of it -- and a log that is
    // seven-eighths one routine's working notes is one nobody reads, so the
    // warnings that matter went unseen among them. Run with --verbose when a
    // bar is picked wrongly and this all comes back.
    for (const Band* band : reds)
        GB_DEBUG("  red   x=%d y=%d..%d len=%d trough=%d rgb=%u,%u,%u",
                 band->x, band->y0, band->y1, band->len, band->trough,
                 band->r, band->g, band->b);
    for (const Band* band : blues)
        GB_DEBUG("  blue  x=%d y=%d..%d len=%d trough=%d rgb=%u,%u,%u",
                 band->x, band->y0, band->y1, band->len, band->trough,
                 band->r, band->g, band->b);

    // The verdict stays at INFO: one line per attempt, and it is the line that
    // says whether the reader has anything to work with.
    GB_INFO("find_bars: %zu runs, %zu bands, %d bar(s) identified",
            runs.size(), bands.size(), found);
    return found;
}

ScreenRect VitalsReader::pet_band(const BarSpec& hp, const BarSpec& mp) {
    const int top = std::max(hp.bottom(), mp.bottom()) + 2;
    const int left = std::max(0, std::min(hp.x, mp.x) - kPetBandLeft);
    const int right = std::max(hp.right(), mp.right()) + kPetBandRight;
    return ScreenRect{ left, top, std::max(0, right - left), kPetBandDepth };
}

bool VitalsReader::find_pet_bar(const Frame& f, const ScreenRect& look, BarSpec& out) {
    if (!f.valid()) return false;
    const int x_from = std::max(0, look.x), x_to = std::min(f.width, look.right());
    const int y_from = std::max(0, look.y), y_to = std::min(f.height, look.bottom());
    if (x_to - x_from < kPetMinWidth + 4 || y_to <= y_from) return false;

    // Each row's left ends of a bar in a slot's frame: the frame, the
    // border, and red at the left end of what is inside.
    struct End { int y, x0; };
    std::vector<End> ends;
    for (int y = y_from; y < y_to; ++y)
        for (int x = x_from; x + 2 < x_to; ++x)
            if (slot_light(f, x, y) && slot_black(f, x + 1, y) && pet_red(f, x + 2, y))
                ends.push_back(End{ y, x + 2 });

    // Left ends that line up, one under the next, are one bar's.
    struct Bar { int x0, x1, y0, y1; };
    std::vector<Bar> bars;
    for (const End& e : ends) {
        Bar* into = nullptr;
        for (Bar& b : bars)
            if (b.x0 == e.x0 && b.y1 + 1 == e.y) { into = &b; break; }
        if (into) into->y1 = e.y;
        else      bars.push_back(Bar{ e.x0, -1, e.y, e.y });
    }

    // And where each ends on the right. Not along its own rows: past the
    // fill is the empty part, whose colour is not something to depend on --
    // no pet's bar has been seen part empty yet, and light grey like the
    // frame, as the HUD's own bars are at its other size, it would end the
    // bar where the fill ends, in the two black columns that end a fill on
    // the player's client. Along the border over the bar, or under it: black
    // from the left-hand border to the right-hand one whatever the fill, then
    // the frame. And on each of the bar's rows, there, the border and the
    // frame, the frame alike with the one on the left.
    const auto right_end = [&](const Bar& b, int y) {
        if (y < 0 || y >= f.height || !slot_black(f, b.x0 - 1, y)) return -1;
        for (int i = b.x0; i < x_to && i - b.x0 <= kMaxBarWidth; ++i)
            if (!slot_black(f, i, y)) return slot_light(f, i, y) ? i - 2 : -1;
        return -1;
    };
    for (Bar& b : bars) {
        if (b.y1 - b.y0 + 1 < kPetMinRows) continue;
        for (const int y : { b.y0 - 1, b.y1 + 1 }) {
            const int x1 = right_end(b, y);
            if (x1 - b.x0 + 1 < kPetMinWidth) continue;
            bool framed = true;
            for (int row = b.y0; framed && row <= b.y1; ++row)
                framed = slot_black(f, x1 + 1, row) && slot_light(f, x1 + 2, row) &&
                         frames_alike(f, b.x0 - 2, x1 + 2, row);
            if (framed) { b.x1 = x1; break; }
        }
    }

    // The widest -- the pet's slot is the wide one -- and of two as wide the
    // taller. A row or two of a picture that happens to fit is no bar.
    const Bar* best = nullptr;
    for (const Bar& b : bars) {
        if (b.x1 < 0) continue;
        if (!best) { best = &b; continue; }
        const int w = b.x1 - b.x0, bw = best->x1 - best->x0;
        if (w > bw || (w == bw && b.y1 - b.y0 > best->y1 - best->y0)) best = &b;
    }
    if (!best) return false;

    // Which rows to read: those the colour of the middle row at the left
    // end, where the fill is, around it -- not the paler highlight along the
    // top (ff948c on the player's client), which matches nothing else.
    const int mid = (best->y0 + best->y1) / 2;
    std::uint8_t mr, mg, mb;
    f.get(best->x0, mid, mr, mg, mb);
    const auto alike = [&](int y) {
        std::uint8_t r, g, b;
        f.get(best->x0, y, r, g, b);
        return near(r, mr, kPetTolerance) && near(g, mg, kPetTolerance) && near(b, mb, kPetTolerance);
    };
    int top = mid, bottom = mid;
    while (top > best->y0 && alike(top - 1)) --top;
    while (bottom < best->y1 && alike(bottom + 1)) ++bottom;

    out = BarSpec{};
    out.x = best->x0;
    out.y = top;
    out.width = best->x1 - best->x0 + 1;
    out.height = bottom - top + 1;
    out.r = mr;
    out.g = mg;
    out.b = mb;
    out.tolerance = kPetTolerance;
    return true;
}

bool VitalsReader::pet_frame_holds(const Frame& f, const BarSpec& bar) {
    if (!f.valid() || !bar.valid()) return false;
    if (bar.x < 2 || bar.right() + 1 >= f.width || bar.y < 0 || bar.bottom() > f.height) return false;
    for (int y = bar.y; y < bar.bottom(); ++y)
        if (!slot_black(f, bar.x - 1, y) || !slot_light(f, bar.x - 2, y) || !slot_black(f, bar.right(), y) ||
            !slot_light(f, bar.right() + 1, y) || !frames_alike(f, bar.x - 2, bar.right() + 1, y))
            return false;
    return true;
}

float VitalsReader::PetWatch::read(const Frame& f, const BarSpec& hp, const BarSpec& mp, std::uint64_t at_ms) {
    if (!f.valid()) return -1.0f;
    bool there = bar.valid() && pet_frame_holds(f, bar);
    // Not where it was, or never found: looked for, every kPetLookMs at
    // most. A bar found elsewhere replaces it; none found keeps it -- the
    // slot may be back where it was in a moment.
    if (!there && (looked_ms == 0 || at_ms >= looked_ms + kPetLookMs)) {
        looked_ms = std::max<std::uint64_t>(at_ms, 1);
        BarSpec found;
        if (find_pet_bar(f, pet_band(hp, mp), found)) {
            bar = found;
            there = pet_frame_holds(f, bar);
        }
    }
    return there ? measure(f, bar) : -1.0f;
}

VitalsReader::Stats VitalsReader::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

float VitalsReader::if_fresh(float pct, std::uint64_t read_at_ms, std::uint64_t at_ms, std::uint64_t within_ms) {
    if (read_at_ms == 0) return -1.0f;          // never read
    // A read stored after `at_ms` was taken is as fresh as can be.
    if (at_ms <= read_at_ms) return pct;
    return at_ms - read_at_ms <= within_ms ? pct : -1.0f;
}

bool VitalsReader::in_world(std::uint64_t hp_seen_ms, std::uint64_t at_ms, std::uint64_t within_ms) {
    if (hp_seen_ms == 0) return false;          // never read
    if (at_ms <= hp_seen_ms) return true;       // read after `at_ms` was taken
    return at_ms - hp_seen_ms <= within_ms;
}

std::uint64_t VitalsReader::hp_seen_after(std::uint64_t seen_ms, float hp_pct, std::uint64_t read_at_ms) {
    return hp_pct >= 0.0f ? read_at_ms : seen_ms;
}

void VitalsReader::keep_unreadable_frame(const Frame& f, bool from_screen, float mp_pct) {
    const std::uint64_t now = now_ms();
    if (kept_frame_ms_ != 0 && now - kept_frame_ms_ < kKeepFrameGapMs) return;
    // One still being written is left to finish, and this one goes unkept:
    // a std::async future assigned over before it is ready waits for it here.
    if (keeping_.valid() && keeping_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    kept_frame_ms_ = now;
    const char* how = from_screen ? "read off the screen" : "drawn by the window itself";
    const char* mp = mp_pct >= 0.0f ? "MP read" : "MP unreadable too";

    // On a copy, off this thread: the next grab writes into `f`.
    keeping_ = std::async(std::launch::async, [shot = f, how, mp] {
        // Named by when it was taken, so that they sort oldest first.
        const std::string dir = exe_relative("vitals_debug");
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char name[48];
        std::strftime(name, sizeof name, "vitals_%Y%m%d_%H%M%S.png", &tm);
        if (!shot.valid() || !make_folder(dir) || !save_png(shot, dir + "\\" + name)) {
            GB_INFO("Vitals: HP unreadable on a frame %s, %s; the frame could not be kept", how, mp);
            return;
        }
        std::vector<std::string> names = files_in(dir, "vitals_*.png");
        std::sort(names.begin(), names.end());
        for (std::size_t i = 0; i + kKeptFrames < names.size(); ++i) remove_file(dir + "\\" + names[i]);
        GB_INFO("Vitals: HP unreadable on a frame %s, %s -- kept it as vitals_debug\\%s", how, mp, name);
    });
}

std::size_t VitalsReader::failures_since(std::uint64_t since_ms) const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return static_cast<std::size_t>(
        std::count_if(failed_at_.begin(), failed_at_.end(), [since_ms](std::uint64_t t) { return t >= since_ms; }));
}

} // namespace gobot
