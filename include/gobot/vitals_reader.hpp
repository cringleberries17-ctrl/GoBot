#pragma once

#include "gobot/bar_spec.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/screen_capture.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

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
///
/// And the pet's HP, off the red bar in the pet's slot on the HUD's row of
/// companion slots, under the HP and MP bars (pet_percent): found there by
/// itself, and measured from its fill, as the game prints no figures on it.
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

    /// Frames come from the window, through ScreenCapture, unless this is
    /// set; a test sets its own. Set it before start(), never while running.
    void set_grab(std::function<bool(void* window, Frame& out)> grab) { grab_ = std::move(grab); }

    /// 0..100, or -1 when the bar has not been set up or cannot be read.
    ///
    /// Worked out from the figures printed on the bar when those are legible,
    /// and from the length of the coloured part when they are not.
    float hp_percent() const { return hp_.load(); }
    float mp_percent() const { return mp_.load(); }

    /// The pet's HP, 0..100, or -1: no pet's bar found on the HUD, or what is
    /// where it was found is not its bar now -- the pet put away, the slot
    /// covered. Measured from the length of the bar's red fill, the only way:
    /// the game prints no figures on it. Read off the same frames as HP and
    /// MP, so last_read_ms() is its age too. See PetWatch for how the bar is
    /// found and kept.
    float pet_percent() const { return pet_.load(); }
    /// Where the pet's bar was last found, in the window's client area; not
    /// valid() before it has been.
    BarSpec pet_bar() const;

    /// When the bars were last read, on now_ms()'s clock; 0 before the first
    /// read. A grab that fails, or no game window, leaves it where it was --
    /// and hp_percent() and mp_percent() with it, at what they read then.
    std::uint64_t last_read_ms() const { return read_at_ms_.load(); }

    /// How far apart the reads are: the reader's wait between one and the
    /// next. Said in the Load line (gobot/load_meter.hpp). kReadGapMs until
    /// set_period_ms says otherwise, as it always was.
    static constexpr int kReadGapMs = 150;
    int period_ms() const { return period_ms_.load(); }

    /// The wait between reads from now on, held to kMinPeriodMs..kMaxPeriodMs.
    /// GoBot's window asks for the run's pace (150 ms, 250 in light mode)
    /// while anything that acts on HP and MP is alive and a second otherwise,
    /// on every pass of its loop: the same period asked again changes
    /// nothing. A shorter one than the reader waits on takes effect at once --
    /// the next read is taken now, not when the longer wait would have ended
    /// -- so a job started reads fresh figures before its first key; a longer
    /// one from the next wait. Any thread.
    void set_period_ms(int ms);
    static constexpr int kMinPeriodMs = 50;
    static constexpr int kMaxPeriodMs = 10000;

    /// When the pace was last cut from an idle one to a run's -- to half of
    /// what it was or less (set_period_ms), a job starting -- on now_ms()'s
    /// clock; 0 never. A read before it was taken at the idle pace and may be
    /// a second old: HP read low a second ago, a potion drunk by hand since,
    /// and the macro's first look would drink again on it. GoBot's window
    /// hands the macro no reading until a read lands after this, tens of ms
    /// on, as the shorter pace wakes the reader at once. A smaller cut --
    /// 250 to 150, light mode unticked in a fight -- leaves it where it was.
    std::uint64_t paced_from_ms() const { return paced_from_ms_.load(); }

    /// How long the printed figures may go unread before they are forgotten,
    /// and the landmark looked for again: twelve reads at kReadGapMs, as it
    /// always was -- about two seconds.
    static constexpr int kForgetAfterMs = 1800;
    /// Reads in a row without the figures that make kForgetAfterMs, at reads
    /// `period_ms` apart: the fewest whose gaps add up to it, never fewer
    /// than two -- one unreadable read is ordinary, a damage number across
    /// the caption. 12 at 150 ms, exactly as before; 8 at 250 (2.0 s); 2 at
    /// a second. Counted in reads, not measured on the clock, so that 150
    /// gives the same 12 however long each read's own work takes.
    static int forget_after_misses(int period_ms);

    /// A bar's measure on a read that missed its figures, as it is handed on:
    /// `measured` itself under kCaptionShadowPct, or with no figures kept
    /// (`value` -1, `max` 0 or less); from there on the higher of it and the
    /// kept figures' percentage. The caption, printed right-aligned over the
    /// bar, paints out the fill under it, and a fill that reaches it measures
    /// where it begins: "12643/15280" begins 56.7% along, so MP at 66-90% read
    /// 56%, under the MP mark of 59 (2026-11-03: four such reads seen, and
    /// three MP potions most likely pressed on one). There the measure says
    /// "this much or more"; the figures, kept forget_after_misses reads, say
    /// how much more.
    static float shadowed_percent(float measured, int value, int max);
    /// Left of where any caption begins -- an 11-character one at 56-58% --
    /// the fill measures true (BarCheck::kBarTrueBelowPct likewise).
    static constexpr float kCaptionShadowPct = 54.0f;

    /// When HP last read as a figure -- 0..100, not -1 -- on now_ms()'s clock;
    /// 0 before the first. A read that made out neither the figures nor the
    /// bar leaves it where it was, as does a grab that fails.
    std::uint64_t hp_seen_ms() const { return hp_seen_ms_.load(); }

    /// `pct`, read at `read_at_ms`, as it is to be acted on at `at_ms`:
    /// itself while it is `within_ms` old or less, otherwise -1, no reading.
    /// A reader whose grabs have stopped working keeps its last values; pushed
    /// on to the macro as fresh, they hold the HP potion back as above its
    /// mark on a number nobody read, and the black box shows a steady HP that
    /// was not read at all (R8R-3).
    static float if_fresh(float pct, std::uint64_t read_at_ms, std::uint64_t at_ms, std::uint64_t within_ms);

    /// Whether the character counts as in the world at `at_ms`: HP read
    /// within `within_ms` of it (hp_seen_ms). Not one unreadable frame --
    /// those come every few seconds off the screen, and every other read as a
    /// character dies -- but HP unread that long: a loading screen, the login
    /// pages, a game gone. Never read is not in the world.
    ///
    /// Asked at the reader's last read (last_read_ms), not at the clock: grabs
    /// that fail -- the game minimised -- leave it as the last frame had it,
    /// as hp_percent() always did. Out of the world there was a game merely
    /// minimised, and buffs cast again on its return as after a death.
    static bool in_world(std::uint64_t hp_seen_ms, std::uint64_t at_ms, std::uint64_t within_ms);

    /// hp_seen_ms as a read at `read_at_ms` leaves it: that read's time when
    /// it made out HP (0..100), as it was when it did not (-1).
    static std::uint64_t hp_seen_after(std::uint64_t seen_ms, float hp_pct, std::uint64_t read_at_ms);

    /// Grabs that failed at `since_ms` or after, on now_ms()'s clock -- of the
    /// last kFailuresKept -- for the log after a death.
    std::size_t failures_since(std::uint64_t since_ms) const;
    static constexpr std::size_t kFailuresKept = 256;

    /// The figures the game prints across its bars. -1 for any it could not
    /// read, which is also what they are before the first successful read.
    ///
    /// Kept through a read that misses them, for forget_after_misses reads:
    /// `hp_now` and `mp_now` say whether the last read made them out itself.
    /// Only those are the bar's at that read -- what the packets' check
    /// (packets_vitals.cpp) may hold against 0x0051.
    struct Vitals {
        int hp = -1, hp_max = -1;
        int mp = -1, mp_max = -1;
        bool hp_now = false, mp_now = false;   ///< the figures made out on the last read itself

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
    ///
    /// Nothing else but the companion slots under the HUD, whose bars sit in
    /// a black border inside a light grey frame -- the pet's a red one over a
    /// purple one, stacked like HP over MP. On 2026-10-26 "Tự tìm", on frames
    /// without the top of the HUD in them, took those two for HP and MP, and
    /// saved HP at 137,86, 134 wide: the pet's bar. A bar in such a frame is
    /// never HP or MP here. HP's and MP's own edge is slate (5a6373), in a
    /// pale blue-grey frame.
    static int find_bars(const Frame& f, BarSpec& hp, BarSpec& mp);

    /// The pet's HP bar in a frame, looked for in `look` (pet_band): the
    /// widest bar there -- kPetMinWidth or more -- that sits in a companion
    /// slot's frame, a light grey frame and a black border inside it on
    /// either side, alike on both, and has the red of a fill at its left end.
    /// On the player's client, 1366x768, it is the slot with two portraits
    /// and the pet's food ("Thức Ăn") beside them: its upper bar, red, 112
    /// wide, at 137,86..91 under the landmark at 44,4. The slot's lower bar,
    /// purple with tick marks, is not the pet's HP -- nor is the grabbed
    /// picture GoC calls pet_hp_indicator, which looks like it -- and it is
    /// not red; the HP bar, red and wider, has no such frame round it; and
    /// the other slots' bars are a third as wide.
    ///
    /// `out` is the bar's full width -- along the black border over it, or
    /// under it, which runs to the right-hand border whatever the fill; not
    /// along its own rows, where past the fill is its empty part, a colour
    /// not to depend on -- and the rows of it that are the colour of its
    /// middle row, within kPetTolerance -- not the paler highlight along its
    /// top -- in that colour. False, and `out` untouched, when there is none:
    /// no pet out, or one with no HP left, which leaves no red to know the
    /// bar by.
    static bool find_pet_bar(const Frame& f, const ScreenRect& look, BarSpec& out);

    /// Where the pet's bar is looked for, given the HP and MP bars: under
    /// them, from just below the lower of the two down through the row of
    /// companion slots -- kPetBandDepth rows, which stops short of the
    /// party's panels further down -- and from kPetBandLeft left of them to
    /// kPetBandRight right of them. It follows the bars, and so the landmark.
    static ScreenRect pet_band(const BarSpec& hp, const BarSpec& mp);

    /// Whether the pet's bar is still where `bar` says: on each of its rows
    /// the black border and the light grey frame either side of it, alike on
    /// both. Not its fill -- a pet with no HP left has none -- so a bar found
    /// once is kept through 0, while a slot gone, or a window over it, is no
    /// bar: no reading, never an empty bar's 0%.
    static bool pet_frame_holds(const Frame& f, const BarSpec& bar);

    /// The pet's bar from frame to frame, as the reader follows it: looked
    /// for (find_pet_bar, in pet_band of the HP and MP bars) until it is
    /// found, and again whenever it is not where it was (pet_frame_holds) --
    /// the HUD moved, the pet put away -- at most every kPetLookMs; and kept
    /// meanwhile, so a pet at 0 HP, with no red left to find its bar by,
    /// still reads 0 where its bar was. Exposed for tests; the reader's
    /// thread has one.
    struct PetWatch {
        BarSpec       bar;              ///< where it was last found; not valid() before
        std::uint64_t looked_ms = 0;    ///< when it was last looked for; 0 never

        /// The pet's HP in `f` at `at_ms`: 0..100, or -1 for no bar, or none
        /// where it was. `hp` and `mp` are where the HP and MP bars are.
        float read(const Frame& f, const BarSpec& hp, const BarSpec& mp, std::uint64_t at_ms);
    };
    static constexpr std::uint64_t kPetLookMs = 2000;
    static constexpr int kPetMinWidth = 60;
    static constexpr int kPetTolerance = 80;
    static constexpr int kPetBandLeft = 100;
    static constexpr int kPetBandRight = 200;
    static constexpr int kPetBandDepth = 76;

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

    /// Whether two bars sit as the game draws HP and MP: the same left edge,
    /// give or take a few pixels, MP under HP and close by, and about as wide
    /// -- the arrangement find_bars looks for first. Only such a pair moves
    /// the bars by itself (relocated), or is taken by the simple screen's "Tự
    /// tìm"; the fallback find_bars answers with when it finds none is for the
    /// full screen's FIND THE BARS, which draws it for the player to judge.
    static bool bars_paired(const BarSpec& hp, const BarSpec& mp);

    /// Whether a look at `f` finds the bars somewhere else than `hp` and `mp`:
    /// find_bars' HP and MP there, drawn as a pair (bars_paired), into
    /// `new_hp` and `new_mp`, and different enough (bars_differ). What the
    /// reader's auto-relocation goes by, a look at a time.
    static bool relocated(const Frame& f, const BarSpec& hp, const BarSpec& mp, BarSpec& new_hp,
                          BarSpec& new_mp);

    /// Whether a bar's colour is a fill colour of its bar at all: HP's red
    /// (`hp`), MP's blue, its own channel strong and well over the others.
    /// One that is not -- learned off something else -- is put back to the
    /// game's own (kHpFill, kMpFill) when the landmark places the bars.
    static bool reads_as_fill(const BarSpec& s, bool hp);

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

    /// Measure one bar against a frame: 0..100, or -1 when what is there is
    /// not a bar at all. Exposed for tests.
    ///
    /// -1 rather than 0 for anything that is neither a fill nor an empty
    /// trough -- the black of a capture with nothing in it, a picture that
    /// changes from pixel to pixel, anything light with none of the fill
    /// colour in it (a bright bar of another colour, say) -- because 0% is
    /// what fires the potion, and a screen with no character on it (loading,
    /// the login pages, a window in front) must not read as an empty bar.
    static float measure(const Frame& f, const BarSpec& spec);

    /// Longest bar this will read across. Wider specs are measured over this
    /// much of themselves rather than refused.
    static constexpr int kMaxBarWidth = 1024;

private:
    void loop();

    std::atomic<void*> hwnd_{nullptr};
    std::atomic<bool>  running_{false};
    std::function<bool(void*, Frame&)> grab_;   ///< see set_grab; empty for the real window

    /// The wait between reads (set_period_ms), and what cuts it short: stop(),
    /// and a shorter period asked for (wake_now_, under wake_mutex_).
    std::atomic<int>        period_ms_{kReadGapMs};
    std::atomic<std::uint64_t> paced_from_ms_{0};   ///< see paced_from_ms
    std::mutex              wake_mutex_;
    std::condition_variable wake_;
    bool                    wake_now_ = false;
    std::atomic<bool>  auto_relocate_{true};
    std::atomic<float> hp_{-1.0f};
    std::atomic<float> mp_{-1.0f};
    std::atomic<float> pet_{-1.0f};
    std::atomic<std::uint64_t> read_at_ms_{0};
    std::atomic<std::uint64_t> hp_seen_ms_{0};
    std::thread        thread_;

    /// The frame of the first read that makes out no HP after one that did,
    /// kept in vitals_debug for what was on it -- at most one every
    /// kKeepFrameGapMs, the newest kKeptFrames -- written off the reader's
    /// thread (keeping_). The reader thread's alone.
    void keep_unreadable_frame(const Frame& f, bool from_screen, float mp_pct);
    bool          hp_was_read_ = false;
    std::uint64_t kept_frame_ms_ = 0;
    static constexpr std::uint64_t kKeepFrameGapMs = 10000;
    static constexpr std::size_t   kKeptFrames = 20;
    /// A kept frame being written; its destructor waits for the write, which
    /// touches nothing of the reader's.
    std::future<void> keeping_;

    /// The pet's bar, followed: the reader thread's alone -- locate_anchor,
    /// on that thread, moves it with the landmark. pet_bar_, under mutex_,
    /// is the copy the window reads.
    PetWatch      pet_watch_;

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
    /// not changed in a minute. "Enough" is forget_after_misses at the pace
    /// the reads are asked at: about two seconds, whatever the pace.
    int hp_misses_ = 0, mp_misses_ = 0;

    /// Find the landmark again when it has been lost, but not every frame:
    /// it does not move while the HUD is up, so re-finding it constantly is
    /// work for nothing. Checked afresh when a read fails, which is the only
    /// time the answer can have changed. `recoloured` is set when a bar's
    /// colour was no fill colour of it and was put back to the game's own.
    bool locate_anchor(const Frame& f, bool* recoloured = nullptr);

    mutable std::mutex mutex_;
    BarSpec            hp_spec_;
    BarSpec            mp_spec_;
    BarSpec            pet_bar_;
    /// The newest frame. Written by the reader's thread alone, under mutex_,
    /// by swapping the frame just grabbed in; that thread reads it without
    /// the lock, every other one with it.
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
