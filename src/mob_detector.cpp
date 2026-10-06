#include "gobot/mob_detector.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/camera_tracker.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/minimap_tracker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace gobot {
namespace {

/// How far above a health bar the name can start, and how far up one line of
/// it reaches. A name sits directly on its bar; white any further up belongs
/// to something else — a flower, a highlight on the waterfall, somebody else's
/// name — and accepting it lets almost any red pixel in the artwork pass for a
/// monster.
constexpr int kNameGap = 14;
constexpr int kNameHeight = 14;

/// How sure the reader has to be before a patch counts as a name.
///
/// Not high. A name plate is drawn over whatever the creature is standing in,
/// so a letter or two is routinely eaten by a flower or a casting effect, and
/// a part-read name is still a name. What this has to reject is scenery, which
/// does not come close.
constexpr float kMinNameConfidence = 0.40f;

/// How far above the health bar the name can start, and how much ink has to be
/// in that window before it is worth reading.
///
/// How far above the health bar a name can start, and how wide a patch is
/// worth reading. Reading costs time in proportion to width, and no name in
/// this game is a hundred and eighty pixels across.
constexpr int kNameWindowTop = 18;
constexpr int kMaxNameHalfWidth = 90;

struct Px { std::uint8_t r, g, b; };

inline Px at(const Frame& f, int x, int y) {
    Px p{};
    f.get(x, y, p.r, p.g, p.b);
    return p;
}

bool is_health_red(const Px& p, const DetectorTuning& t) {
    return p.r >= t.red_min && p.g <= t.other_max && p.b <= t.other_max;
}

bool is_mana_blue(const Px& p, const DetectorTuning& t) {
    return p.b >= t.blue_min && p.r <= t.blue_red_max;
}

/// The empty part of a health bar: near black.
bool is_trough(const Px& p) {
    return std::max({ p.r, p.g, p.b }) <= 60;
}

/// Whether a mana bar sits under a health bar — the one thing that is only
/// ever true of a player.
///
/// Searched a little wider than the health bar and over several rows: mana is
/// often low, so the blue bar is a fraction of the width, and scenery in front
/// of the character breaks what is left into pieces.
bool has_mana_bar(const Frame& f, int x, int end, int y, int bar_h, int y1,
                  const DetectorTuning& t) {
    for (int dy = bar_h; dy <= bar_h + 6; ++dy) {
        if (y + dy >= y1) break;
        int run = 0;
        for (int xx = x; xx < end; ++xx) {
            Px p{};
            f.get(xx, y + dy, p.r, p.g, p.b);
            if (p.b >= t.blue_min && p.r <= t.blue_red_max) {
                if (++run >= 3) return true;
            } else {
                run = 0;
            }
        }
    }
    return false;
}

/// The name itself: bright and colourless.
bool is_name_text(const Px& p) {
    const int mn = std::min({ p.r, p.g, p.b });
    const int mx = std::max({ p.r, p.g, p.b });
    return mn >= 170 && mx - mn <= 45;
}

} // namespace

DetectorTuning DetectorTuning::defaults() {
    DetectorTuning t;
    // Measured against a 1024x768 client. The band between the top HUD and the
    // chat and skill bars is where the play field is; the game's own panels are
    // full of red text and buttons and would otherwise dominate the results.
    t.area = ScreenRect{ 0, 104, 1024, 566 };
    // The minimap and the quest log stack down the right-hand side.
    t.exclude = ScreenRect{ 812, 0, 212, 360 };
    return t;
}

MobDetector::MobDetector() { tuning_ = DetectorTuning::defaults(); }
MobDetector::~MobDetector() { stop(); }

void MobDetector::set_target_window(void* hwnd) { hwnd_.store(hwnd); }

void MobDetector::set_tuning(const DetectorTuning& t) {
    std::lock_guard<std::mutex> lock(mutex_);
    tuning_ = t;
}

DetectorTuning MobDetector::tuning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tuning_;
}

void MobDetector::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&MobDetector::loop, this);
    GB_INFO("Mob detector started");
}

void MobDetector::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        plates_.clear();
    }
    GB_INFO("Mob detector stopped");
}

std::vector<Nameplate> MobDetector::detect(const Frame& f, const DetectorTuning& t,
                                           const GlyphFont* font, Cost* cost) {
    Cost spent;
    std::vector<Nameplate> out;
    if (!f.valid()) return out;

    const int x0 = t.area.valid() ? std::max(0, t.area.x) : 0;
    const int y0 = t.area.valid() ? std::max(0, t.area.y) : 0;
    const int x1 = t.area.valid() ? std::min(f.width, t.area.right()) : f.width;
    const int y1 = t.area.valid() ? std::min(f.height, t.area.bottom()) : f.height;

    for (int y = y0; y < y1; ++y) {
        int x = x0;
        while (x < x1) {
            if (!is_health_red(at(f, x, y), t)) { ++x; continue; }

            int end = x + 1;
            while (end < x1 && is_health_red(at(f, end, y), t)) ++end;

            // Bridge small gaps. Grass and other scenery are drawn in front of
            // the bar and cut it into pieces, each too short to recognise on
            // its own; the character's own bar is routinely split this way.
            for (;;) {
                int gap = 0;
                while (end + gap < x1 && gap <= 6 && !is_health_red(at(f, end + gap, y), t))
                    ++gap;
                if (gap == 0 || gap > 6 || end + gap >= x1) break;
                if (!is_health_red(at(f, end + gap, y), t)) break;
                end += gap;
                while (end < x1 && is_health_red(at(f, end, y), t)) ++end;
            }

            const int len = end - x;

            if (len < t.min_bar || len > t.max_bar) { x = end; continue; }
            if (t.exclude.valid() && t.exclude.contains(x, y)) { x = end; continue; }

            // A bar is a few pixels tall, so the same one is met again on the
            // next few rows. Skipping anything that overlaps what is already
            // found keeps one detection per creature.
            const ScreenRect probe{ x, y, len, 1 };
            bool seen = false;
            for (const Nameplate& n : out) {
                ScreenRect grown = n.bar;
                grown.y -= 6; grown.h += 12;
                grown.x -= 4; grown.w += 8;
                if (grown.overlaps(probe)) { seen = true; break; }
            }
            if (seen) { x = end; continue; }

            Nameplate n;
            n.bar = ScreenRect{ x, y, len, 1 };

            // How tall the red block is.
            while (y + n.bar.h < y1) {
                int hits = 0;
                for (int xx = x; xx < end; ++xx)
                    if (is_health_red(at(f, xx, y + n.bar.h), t)) ++hits;
                if (hits * 10 < len * 7) break;
                ++n.bar.h;
            }
            if (n.bar.h < t.min_bar_height) { x = end; continue; }

            // The name written above the bar.
            //
            // The name box used to be what was looked for -- a dark, nearly
            // grey rectangle -- and that worked until the character walked onto
            // bright grass. The box is translucent: over grey rock it is dark
            // grey, over yellow-green grass it is yellow-green and nothing like
            // "dark and colourless". Everything on screen stopped being
            // detected at once, which is a good sign the test was wrong rather
            // than the threshold.
            //
            // The writing inside it does not change. It is white, it is
            // colourless, and no patch of scenery above a red sprite has any --
            // which was the whole job the box test was doing.
            int text_rows = 0, text_top = 0, text_px = 0;
            int text_left = x1, text_right = x0;


            ++spent.bars;
            {
                const int sx0 = std::max(x0, x - 24);
                const int sx1 = std::min(x1, end + 24);

                // A row of writing, as opposed to a bright expanse.
                //
                // Writing is sparse: a few strokes of white against the dark
                // box behind it. Sky and falling water are just as bright and
                // just as colourless, and fill the whole row -- which is how
                // the red roof of a temple with sky above it came to be read as
                // nine monsters. So a row qualifies only if it is partly
                // bright and partly dark, which no expanse of anything is.
                const int span = sx1 - sx0;
                const auto hits_on = [&](int yy) {
                    int bright = 0, dark = 0;
                    for (int xx = sx0; xx < sx1; ++xx) {
                        const Px p = at(f, xx, yy);
                        if (is_name_text(p)) ++bright;
                        else if (std::max({ p.r, p.g, p.b }) < 110) ++dark;
                    }
                    if (bright * 10 > span * 6) return 0;    // a bright expanse
                    if (dark * 10 < span * 2) return 0;      // nothing to write on
                    return bright;
                };

                // Where the writing starts. A name sits directly on top of the
                // bar; white further up is a flower, a highlight on the water,
                // or somebody else's name, and accepting it lets almost any red
                // pixel in the artwork pass for a monster.
                int first = 0;
                for (int dy = 2; dy <= kNameGap; ++dy) {
                    if (y - dy < 0) break;
                    if (hits_on(y - dy) >= 3) { first = dy; break; }
                }
                if (first == 0 && !has_mana_bar(f, x, end, y, n.bar.h, y1, t)) {
                    x = end;
                    continue;
                }
                if (first == 0) first = 2;      // a mana bar is signature enough

                // And how far up it goes. One line of text, not a column of
                // scattered white.
                for (int dy = first; dy <= first + kNameHeight; ++dy) {
                    const int yy = y - dy;
                    if (yy < 0) break;
                    const int hits = hits_on(yy);
                    if (hits < 3) continue;
                    ++text_rows;
                    text_top = dy;
                    text_px += hits;
                    for (int xx = sx0; xx < sx1; ++xx) {
                        if (!is_name_text(at(f, xx, yy))) continue;
                        text_left = std::min(text_left, xx);
                        text_right = std::max(text_right, xx);
                    }
                }
            }
            // A mana bar under the health bar means a player.
            //
            // Calling a player a monster is the one mistake worth going out of
            // the way to avoid, so this is deliberately easy to trip.
            n.is_player = has_mana_bar(f, x, end, y, n.bar.h, y1, t);

            // The name has to be legible — unless there is a mana bar, which is
            // signature enough on its own.
            //
            // Nothing in the artwork is a red bar with a blue bar directly
            // under it. Insisting on a readable name as well cost the bot sight
            // of its own character for seconds at a time: the character stands
            // in its own casting glow with a pet flapping over its head, and
            // the writing is the first thing to disappear under both. The bars
            // stay put.
            if (!n.is_player && (text_rows < t.min_text_rows || text_px < t.min_name_px)) {
                x = end;
                continue;
            }

            // The plate is the writing's own extent, and nothing else.
            //
            // Not unioned with the health bar: the bar is anchored on its left
            // and shrinks rightward as the creature is hurt, so including it
            // drags the apparent position left exactly as the creature starts
            // to matter. The name stays centred on the creature however badly
            // it is doing. With no name to go on, the bar is all there is.
            if (text_rows >= t.min_text_rows && text_px >= t.min_name_px) {
                n.plate = ScreenRect{ text_left, y - text_top,
                                      text_right - text_left + 1, text_top };
                // A name is a line of text, so it is wider than it is tall. A
                // tall narrow patch of white is a highlight on scenery.
                if (!n.is_player && n.plate.w * 2 < n.plate.h * 3) { x = end; continue; }

                // Now read it, if there is a font to read it with.
                //
                // The statistics above decide where to look and the reader
                // decides what is there. Neither does the other's job well:
                // reading every reddish run on a busy map costs most of a
                // second a frame, and noticing that a patch is mottled is not
                // the same as finding letters on it -- which is why a temple
                // roof against the sky once read as nine monsters.
                //
                // So the cheap test narrows, and the reader has the last word.
                // A patch the statistics liked but no glyph fits is scenery.
                if (font && font->loaded()) {
                    ++spent.read;

                    // Generously around the writing the statistics found,
                    // not exactly on it. That extent comes from a test for
                    // bright colourless pixels, so for a name drawn in yellow
                    // or green it marks only the few pixels that happened to
                    // pass -- and a box drawn tightly around those cuts the
                    // name in half. Padding costs a fraction of a millisecond
                    // and there are only a handful of these a frame.
                    // Around the writing the statistics found, with room to
                    // spare, and no wider than a name can be. The extent they
                    // report comes from a test for bright colourless pixels,
                    // so for a name drawn in yellow or green it marks only the
                    // few pixels that happened to pass and a box drawn tightly
                    // round them cuts the name in half. The cap is there
                    // because reading costs time in proportion to width and a
                    // long health bar is no reason to read scenery.
                    int left = std::max(x0, std::min(text_left, x) - 28);
                    int right = std::min(x1, std::max(text_right, end) + 28);
                    if (right - left > kMaxNameHalfWidth * 2) {
                        const int mid = (left + right) / 2;
                        left = mid - kMaxNameHalfWidth;
                        right = mid + kMaxNameHalfWidth;
                    }
                    const ScreenRect box{ left, std::max(0, y - kNameWindowTop),
                                          right - left, kNameWindowTop - 1 };
                    n.name = font->read_line(f, box);
                    n.name_confidence = font->last_confidence();

                    // The letters are the plate: these are the pixels of the
                    // name, exactly, which is a better ground position than an
                    // extent guessed from pixel statistics.
                    const auto& placed = font->last_placed();

                    // Two different questions, and only the first is a veto.
                    //
                    // Whether anything here is writing at all decides if this
                    // is a creature; whether the writing was read well enough
                    // to be trusted decides if the name is worth keeping. A
                    // name plate drawn over flowers loses letters all the
                    // time, and refusing to see the monster because its name
                    // is half-eaten is worse than not knowing what it is.
                    const bool any_letters = placed.size() >= 2;
                    const bool legible = any_letters &&
                                         n.name_confidence >= kMinNameConfidence;
                    if (legible) {
                        ScreenRect got = placed.front().box;
                        for (const GlyphFont::Placed& g : placed) {
                            const int rx = std::max(got.right(), g.box.right());
                            const int by = std::max(got.bottom(), g.box.bottom());
                            got.x = std::min(got.x, g.box.x);
                            got.y = std::min(got.y, g.box.y);
                            got.w = rx - got.x;
                            got.h = by - got.y;
                        }
                        n.plate = got;
                    }
                    if (!legible) {
                        n.name.clear();
                        n.name_confidence = 0.0f;
                    }
                    // A mana bar is signature enough on its own: nothing in the
                    // artwork is a red bar with a blue one under it, and a
                    // character's own name is routinely buried under its
                    // casting glow.
                    if (!any_letters && !n.is_player) { x = end; continue; }
                }
            } else {
                n.plate = ScreenRect{ x, y - 2, len, 2 };
            }

            // Health. The empty part of the bar is near-black and runs to the
            // right, bounded by the name box — which is why the box has to be
            // measured first. Approximate: a bar that is almost full has no
            // trough to find, and nothing on screen says how wide it would be.
            {
                int trough = 0;
                const int limit = std::min(x1, n.plate.right());
                while (end + trough < limit && trough < t.max_bar &&
                       is_trough(at(f, end + trough, y + n.bar.h / 2)))
                    ++trough;
                n.hp = static_cast<float>(len) / static_cast<float>(len + trough);
            }

            out.push_back(n);
            x = end;
        }
    }

    if (cost) *cost = spent;
    return out;
}

void MobDetector::loop() {
    ScreenCapture capture;
    Frame frame;

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(140));
        if (!running_.load()) break;

        void* hwnd = hwnd_.load();
        if (!hwnd) continue;

        if (!capture.grab(hwnd, frame)) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.failures;
            continue;
        }

        DetectorTuning t;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            t = tuning_;
        }

        const GlyphFont* font = font_.load();
        std::vector<Nameplate> found = detect(frame, t, font);

        // Your own character, found by reading its name.
        //
        // Nothing else will find it. Other players are known by the mana bar
        // under their health bar, and the local character has neither -- its
        // health is in the HUD, not over its head. All it has is its name,
        // written under its feet like everyone else's, so the way to see your
        // character is to go and read the screen for it.
        if (font && font->loaded() && !name_known(found)) {
            std::string want;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                want = character_name_;
            }
            if (!want.empty()) find_character(frame, t, *font, want, found);
        }

        // Fed from the same frame rather than capturing its own: the screen
        // grab is by far the expensive part and there is no reason to pay for
        // it twice.
        if (MinimapTracker* mm = minimap_.load()) mm->update(frame);
        if (CameraTracker* cam = camera_.load()) cam->update(frame);

        std::size_t mobs = 0;
        bool saw_player = false;
        for (const Nameplate& n : found) {
            if (n.is_player) saw_player = true; else ++mobs;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            plates_ = std::move(found);
            frame_ = frame;

            Nameplate me;
            if (pick_player(plates_, frame_.width, me)) {
                last_player_ = me;
                last_player_ms_ = now_ms();
            }
        }
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.frames;
            stats_.last_mobs = mobs;
            stats_.player_seen = saw_player;
            stats_.capture_from_screen = capture.from_screen();
        }
    }
}

void MobDetector::set_character_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    character_name_ = name;
}

std::string MobDetector::character_name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return character_name_;
}

bool MobDetector::name_known(const std::vector<Nameplate>& found) {
    for (const Nameplate& n : found) if (n.is_player) return true;
    return false;
}

void MobDetector::find_character(const Frame& f, const DetectorTuning& t,
                                 const GlyphFont& font, const std::string& want,
                                 std::vector<Nameplate>& out) {
    ScreenRect area = t.area.valid() ? t.area : ScreenRect{ 0, 0, f.width, f.height };

    // Look where it was last, before looking everywhere. A whole frame takes
    // long enough to matter at ten frames a second, and a character does not
    // teleport -- so the search only widens when it has actually been lost.
    ScreenRect near{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_player_.plate.valid() && now_ms() - last_player_ms_ <= kPlayerMemoryMs)
            near = last_player_.plate;
    }
    if (near.valid()) {
        ScreenRect box{ near.x - 120, near.y - 40, near.w + 240, near.h + 80 };
        box.x = std::max(area.x, box.x);
        box.y = std::max(area.y, box.y);
        box.w = std::min(area.right(), box.right()) - box.x;
        box.h = std::min(area.bottom(), box.bottom()) - box.y;
        near = box;
    }

    // The search itself is the slow part and reads nothing that is shared, so
    // it happens with no lock held.
    ScreenRect at{};
    float sure = 0.0f;
    bool got = near.valid() && font.find_text(f, near, want, at, &sure);
    if (!got) got = font.find_text(f, area, want, at, &sure);

    // Said once each way round, so a character that cannot be found says so
    // instead of the bot silently standing still.
    if (got != said_player_) {
        said_player_ = got;
        if (got) GB_INFO("Found your character \"%s\" at %d,%d", want.c_str(),
                         at.cx(), at.y);
        else     GB_WARN("Cannot find \"%s\" on screen -- check it against the "
                         "name the game writes under your feet", want.c_str());
    }
    if (!got) return;

    // The name sits under the character's feet, so where the name is, is where
    // the character is standing.
    Nameplate me;
    me.plate = at;
    me.bar = ScreenRect{ at.x, at.y, at.w, 1 };
    me.is_player = true;
    me.name = want;
    me.name_confidence = sure;
    out.push_back(me);
}

std::vector<Nameplate> MobDetector::plates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return plates_;
}

std::vector<Nameplate> MobDetector::mobs() const {
    std::vector<Nameplate> all;
    Nameplate me;
    bool have_me = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        all = plates_;
        have_me = pick_player(plates_, frame_.width, me);
    }

    std::vector<Nameplate> out;
    out.reserve(all.size());
    for (const Nameplate& n : all)
        if (!n.is_player) out.push_back(n);

    if (have_me) {
        // Nearest first, with height counting for more than distance along the
        // ground: something level with you is reachable, something the same
        // distance away but two platforms up is not.
        const int px = me.x(), py = me.y();
        std::sort(out.begin(), out.end(), [px, py](const Nameplate& a, const Nameplate& b) {
            const auto cost = [px, py](const Nameplate& n) {
                const int dx = n.x() - px;
                const int dy = (n.y() - py) * 3;
                return dx * dx + dy * dy;
            };
            return cost(a) < cost(b);
        });
    }
    return out;
}

bool MobDetector::pick_player(const std::vector<Nameplate>& plates, int frame_width,
                              Nameplate& out) {
    // Party members show health and mana bars too, so "has a mana bar" narrows
    // it to the players but does not say which one is you. The camera follows
    // your character, so of the candidates you are the one nearest the middle
    // of the screen. It is only wrong at the edges of a map, where the camera
    // stops scrolling and you walk away from the centre -- and there the next
    // frame with nobody else on it corrects it.
    const int centre = frame_width > 0 ? frame_width / 2 : 0;
    const Nameplate* best = nullptr;
    int best_d = 0;
    for (const Nameplate& n : plates) {
        if (!n.is_player) continue;
        const int d = std::abs(n.x() - centre);
        if (!best || d < best_d) { best = &n; best_d = d; }
    }
    if (!best) return false;
    out = *best;
    return true;
}

bool MobDetector::player(Nameplate& out, bool* fresh) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pick_player(plates_, frame_.width, out)) {
        if (fresh) *fresh = true;
        return true;
    }
    // Not visible this instant. The pet, a casting effect or the grass covers
    // the plate for a frame or two several times a fight, and stopping dead
    // each time leaves the bot standing still for most of it.
    if (last_player_ms_ != 0 && now_ms() - last_player_ms_ <= kPlayerMemoryMs) {
        out = last_player_;
        if (fresh) *fresh = false;
        return true;
    }
    if (fresh) *fresh = false;
    return false;
}

bool MobDetector::latest_frame(Frame& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame_.valid()) return false;
    out = frame_;
    return true;
}

MobDetector::Stats MobDetector::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

} // namespace gobot
