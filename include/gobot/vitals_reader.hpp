#pragma once

#include "gobot/bar_spec.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/screen_capture.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace gobot {

/// Reads HP and MP straight off the health bars.
///
/// The one piece of live game state obtainable without fighting the protocol
/// or the anticheat: it is already drawn on screen, at a fixed place.
///
/// Read two ways, because the game offers two and they fail differently.
///
/// The game prints the exact figures across its bars -- "6744/9232" -- and
/// that writing is in the game's own bitmap font, which this project can read
/// back exactly. So the first answer is the true numbers, and everything that
/// wants a maximum, a party member's health or a rule in absolute terms gets
/// one.
///
/// Measuring the coloured part of the bar is the fallback. It needs no font
/// and survives writing this reader has no glyphs for, but it cannot be exact:
/// the figures are printed *over* the bar, in white on a black outline, so the
/// right-hand third of the fill is painted out by its own caption. A bar that
/// is three-quarters full and one that is completely full measure much the
/// same. That is the bug this class had -- both readings were wrong, and
/// disagreed with each other, which was the only reason anyone noticed.
class VitalsReader {
public:
    VitalsReader();
    ~VitalsReader();

    VitalsReader(const VitalsReader&) = delete;
    VitalsReader& operator=(const VitalsReader&) = delete;

    void set_target_window(void* hwnd);
    void set_hp_bar(const BarSpec& s);
    void set_mp_bar(const BarSpec& s);
    BarSpec hp_bar() const;
    BarSpec mp_bar() const;

    void start();
    void stop();
    bool running() const { return running_.load(); }

    /// 0..100, or -1 when the bar has not been set up or cannot be read.
    ///
    /// Worked out from the figures printed on the bar when those are legible,
    /// and from the length of the coloured part when they are not.
    float hp_percent() const { return hp_.load(); }
    float mp_percent() const { return mp_.load(); }

    /// When the bars were last read, on now_ms()'s clock; 0 before the first
    /// read. A grab that fails, or no game window, leaves it where it was --
    /// and hp_percent() and mp_percent() with it, at what they read then.
    std::uint64_t last_read_ms() const { return read_at_ms_.load(); }

    /// `pct`, read at `read_at_ms`, as it is to be acted on at `at_ms`:
    /// itself while it is `within_ms` old or less, otherwise -1, no reading.
    /// A reader whose grabs have stopped working keeps its last values; pushed
    /// on to the macro as fresh, they hold the HP potion back as above its
    /// mark on a number nobody read, and the black box shows a steady HP that
    /// was not read at all (R8R-3).
    static float if_fresh(float pct, std::uint64_t read_at_ms, std::uint64_t at_ms, std::uint64_t within_ms);

    /// Grabs that failed at `since_ms` or after, on now_ms()'s clock -- of the
    /// last kFailuresKept -- for the log after a death.
    std::size_t failures_since(std::uint64_t since_ms) const;
    static constexpr std::size_t kFailuresKept = 256;

    /// The figures the game prints across its bars. -1 for any it could not
    /// read, which is also what they are before the first successful read.
    struct Vitals {
        int hp = -1, hp_max = -1;
        int mp = -1, mp_max = -1;

        bool have_hp() const { return hp >= 0 && hp_max > 0; }
        bool have_mp() const { return mp >= 0 && mp_max > 0; }
    };
    Vitals values() const;

    /// Load the game's bitmap font so the figures can be read. Without it the
    /// reader still works, from the bars alone, and says so once.
    ///
    /// An empty path means the set the commercial client ships.
    bool load_font(const std::string& dir = {});
    bool has_font() const;

    /// Read "current/maximum" off one bar. Exposed for tests and for the
    /// calibration view, which shows what it got.
    ///
    /// The caption is right-aligned inside the bar and sits a little below its
    /// middle, so where to look follows from the bar's own geometry and needs
    /// no separate calibration.
    static bool read_numbers(const Frame& f, const BarSpec& spec,
                             const GlyphFont& font, int& current, int& maximum);

    /// Where the caption is, given the bar. Exposed so the calibration view
    /// can draw the box it is reading.
    static ScreenRect caption_box(const BarSpec& spec);

    /// Most recent frame, for the calibration view.
    bool latest_frame(Frame& out) const;

    /// Sample a pixel from the last frame, for picking a bar's colour.
    bool sample_pixel(int x, int y, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const;

    /// Work out a bar's fill colour from the pixels inside it: the most common
    /// strongly-saturated colour there. Saves the user eyedropping it.
    bool autodetect_colour(const BarSpec& region, std::uint8_t& r,
                           std::uint8_t& g, std::uint8_t& b) const;

    /// Where the bars sit relative to the HUD's fixed landmark.
    ///
    /// Measured once against this client: the landmark lands at 44,4 and the
    /// bars at 83,8 and 83,22, so +39,+4 and +39,+18. The bar is 159 wide; the
    /// middle five rows are the ones worth sampling, the rest being the steep
    /// part of the gradient.
    struct HudLayout {
        int hp_dx = 39, hp_dy = 4;
        int mp_dx = 39, mp_dy = 18;
        int bar_w = 159, bar_h = 5;
    };

    /// Give the reader the HUD's landmark, and find the bars by it.
    ///
    /// This is how the other helper does it and it is better than what was
    /// here. Working the bars out by scanning for runs of saturated colour
    /// sounds robust and is not: the game prints the figures across the bar, so
    /// when the coloured part happens to run out underneath the writing, the
    /// scan that measures the empty part starts on white text, takes that for
    /// the background and stops dead. The bar then measures 115 wide instead of
    /// 159 -- and every reading taken from it is wrong by a quarter, with
    /// nothing to say so.
    ///
    /// A landmark has none of that trouble. The helper uses the "Level" plate
    /// beside the bars: it is high contrast, it never changes, and crucially it
    /// does not depend on the very quantity being measured. Find it, add the
    /// offsets, and the geometry is right at every value of HP and MP.
    void set_anchor(const Frame& anchor);
    bool has_anchor() const;
    void set_layout(const HudLayout& l);
    HudLayout layout() const;

    /// Where the landmark was last seen, for the calibration view.
    ScreenRect anchor_at() const;

    /// Locate both bars in a frame, geometry and colour, without being told
    /// where to look. Returns how many it found (0, 1 or 2) and only writes
    /// the ones it found.
    ///
    /// Typing four coordinates per bar is the step that makes this feature
    /// hard to use, and it is the step a machine is better at: a health bar is
    /// a long flat run of one saturated colour with a dark trough after it,
    /// which is a shape nothing else in the UI has.
    static int find_bars(const Frame& f, BarSpec& hp, BarSpec& mp);

    /// Whether a bar's geometry still matches what is on screen.
    ///
    /// Two ways it stops matching: the region reads as unreadable, or the fill
    /// colour carries on past the right-hand edge, which means the bar is
    /// wider than the spec says. The second is the one that bites, because it
    /// still produces a number -- just the wrong fraction of the wrong bar.
    static bool bar_is_stale(const Frame& f, const BarSpec& spec);

    /// Whether two geometries are different enough to be different bars.
    /// Tolerant by a few pixels, so the reader does not churn on a detector
    /// that lands one pixel left of where it did last time.
    static bool bars_differ(const BarSpec& a, const BarSpec& b);

    /// Work the bars out again when they stop matching. On by default: the
    /// game's HUD can be resized from its own settings, and the alternative is
    /// a reading that is quietly wrong until somebody notices.
    void set_auto_relocate(bool on) { auto_relocate_.store(on); }
    bool auto_relocate() const { return auto_relocate_.load(); }

    struct Stats {
        std::uint64_t reads = 0;
        std::uint64_t failures = 0;
        bool capture_from_screen = false;

        /// How many times the bars had to be found again.
        std::uint64_t relocations = 0;

        /// Reads where the printed figures were legible. Far below `reads`
        /// means the font is missing or the caption box is in the wrong place,
        /// and the numbers on show are stale.
        std::uint64_t number_reads = 0;
    };
    Stats stats() const;

    /// Measure one bar against a frame. Exposed for tests.
    static float measure(const Frame& f, const BarSpec& spec);

    /// Longest bar this will read across. Wider specs are measured over this
    /// much of themselves rather than refused.
    static constexpr int kMaxBarWidth = 1024;

private:
    void loop();

    std::atomic<void*> hwnd_{nullptr};
    std::atomic<bool>  running_{false};
    std::atomic<bool>  auto_relocate_{true};
    std::atomic<float> hp_{-1.0f};
    std::atomic<float> mp_{-1.0f};
    std::atomic<std::uint64_t> read_at_ms_{0};
    std::thread        thread_;

    int           disagreements_ = 0;
    std::uint64_t last_check_ms_ = 0;

    /// How often to look again, and how many looks in a row have to disagree
    /// before the answer changes. Slow on purpose: a loading screen looks
    /// exactly like a HUD that has moved, and adopting bars found against one
    /// would learn nonsense.
    static constexpr std::uint64_t kCheckGapMs = 2000;
    static constexpr int           kDisagreements = 3;

    /// Believe a new maximum only when it is read twice running.
    ///
    /// The current value moves constantly and a misread of it costs one tick.
    /// The maximum moves on levelling and on changing gear, and a misread of
    /// it poisons every percentage after it -- so it has to be seen twice
    /// before it is adopted.
    int hp_max_pending_ = -1, hp_max_seen_ = 0;
    int mp_max_pending_ = -1, mp_max_seen_ = 0;

    /// Reads in a row that could not make out the figures.
    ///
    /// A few in a row is ordinary -- a floating damage number crosses the
    /// caption, the client redraws mid-capture. Enough of them means something
    /// has moved, and the figures on show stop being true. They are cleared at
    /// that point rather than left standing: a reading that quietly stops
    /// updating looks exactly like one that is working, which is how a bar
    /// measured at the wrong width sat there reading 87% of a figure that had
    /// not changed in a minute.
    int hp_misses_ = 0, mp_misses_ = 0;
    static constexpr int kForgetAfterMisses = 12;   ///< about two seconds

    /// Find the landmark again when it has been lost, but not every frame:
    /// it does not move while the HUD is up, so re-finding it constantly is
    /// work for nothing. Checked afresh when a read fails, which is the only
    /// time the answer can have changed.
    bool locate_anchor(const Frame& f);

    mutable std::mutex mutex_;
    BarSpec            hp_spec_;
    BarSpec            mp_spec_;
    Frame              frame_;
    GlyphFont          font_;
    Vitals             values_;
    bool               warned_no_font_ = false;

    Frame              anchor_;
    HudLayout          layout_;
    ScreenRect         anchor_at_{};
    bool               anchor_found_ = false;

    mutable std::mutex stats_mutex_;
    Stats              stats_;
    /// When each of the last kFailuresKept grabs that failed did, under
    /// stats_mutex_.
    std::deque<std::uint64_t> failed_at_;
};

} // namespace gobot
