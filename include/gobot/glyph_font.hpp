#pragma once

#include "gobot/mob_detector.hpp"
#include "gobot/screen_capture.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gobot {

/// Reads the game's own text off the screen, one glyph at a time.
///
/// Everything else here guesses at text from pixel statistics — is this patch
/// bright and sparse enough to be writing? — and that guessing is behind most
/// of what still goes wrong: name plates missed on bright grass, sky mistaken
/// for a name, the map's title bar given up on as "that would need OCR".
///
/// It does not need to be a guess. The game draws text with a bitmap font: a
/// fixed, tiny picture per character, blitted unchanged every time. Given those
/// pictures, reading the screen is matching them, and the answer is exact.
///
/// The pictures come from the game's own font, extracted into one small PNG per
/// character and named after it. The commercial client ships a set in
/// `data\bitmap_font`, which is where this looks by default — read in place,
/// never copied, so nothing of theirs is redistributed. Point it anywhere else
/// with a directory of the same shape.
class GlyphFont {
public:
    /// Load every glyph under `dir`, which holds `lower/` and `upper/`
    /// sub-directories of PNGs named after the character they draw.
    bool load(const std::string& dir);

    bool        loaded() const { return !glyphs_.empty(); }
    std::size_t size() const { return glyphs_.size(); }
    int         tallest() const { return tallest_; }

    /// Where the commercial client keeps its set, if it is installed.
    static std::string default_dir();

    /// Draw a line of text with these glyphs: white on black, one pixel
    /// between letters, every glyph standing on a common baseline.
    ///
    /// The point is to be able to read it back. A reader that cannot recover a
    /// string from a picture built out of its own glyphs is broken in a way
    /// that has nothing to do with the game, and finding that out needs no
    /// screenshot, no window, and nobody logged in. Letters that hang below
    /// the baseline come out sitting on it, so keep test strings free of them.
    bool render_line(const std::string& utf8, Frame& out) const;

    /// Find a known string on screen.
    ///
    /// Reading asks "what does this say?" and needs to be pointed at something
    /// first. This asks "where does it say this?" -- anywhere, inside a longer
    /// line too. Your own character is found by its name with the stricter
    /// find_whole below: a plate with bars under it may be yours or another
    /// player's -- the bars say a player stands there, not which one -- so
    /// only the name, drawn under its feet like everyone else's, says which.
    ///
    /// Fast enough to run on a whole frame: the first letter is tried at every
    /// position, which the probe test rejects in a few lookups almost
    /// everywhere, and the rest of the string is only matched where that first
    /// letter actually fits.
    ///
    /// Returns the best place it was found, and how well, or false.
    bool find_text(const Frame& f, const ScreenRect& area, const std::string& utf8,
                   ScreenRect& where, float* confidence = nullptr) const;

    /// One place a sought string was found.
    struct Spot {
        ScreenRect where;          ///< its letters, in frame coordinates
        float      sure = 0.0f;    ///< how well they were read, 0..1
    };

    /// Every place a string is written as the whole of its line, as a name is
    /// on its plate: not inside a longer name, and not a name a letter away.
    ///
    /// find_text is satisfied by any line that says the string somewhere, and
    /// for a name that is the wrong question: "Anh" is written in "TuanAnh",
    /// "KhiCon" in "KhiCon2", and six of the seven letters of "ThanhAm" in
    /// "ThanhAn". Looking for your own character that way took another player
    /// for you. Here a place counts only when the first and the last letters
    /// are found and every stroke in their colour on the line, from a space's
    /// width before the name to a space's width after it, is one of the
    /// letters found -- the marks over and under each letter included, so
    /// "Ánh" is not "Anh". A letter in between may still be missing -- grass
    /// or a casting effect eats one, and writes nothing in the name's colour
    /// where it was -- but not replaced by another.
    ///
    /// A spot found from several starting points is given once, the best read.
    std::vector<Spot> find_whole(const Frame& f, const ScreenRect& area,
                                 const std::string& utf8) const;

    /// How much of a sought string has to be legible. A floating name is drawn
    /// over whatever the character is standing in and a letter is routinely
    /// eaten by grass or a casting effect.
    static constexpr double kMinFoundFraction = 0.7;

    /// How well the first letter has to fit before the rest is looked for.
    /// Only a little above the general bar: a floating character name is drawn
    /// half-transparent over whatever it is standing in, so demanding a clean
    /// first letter is demanding that the character stand somewhere plain.
    static constexpr double kMinLeadQuality = 0.50;

    /// Read one line of text out of a region of a captured frame.
    ///
    /// The region should be a little taller than the text and start at or
    /// before its left edge; everything in it is assumed to be one line.
    std::string read_line(const Frame& f, const ScreenRect& r) const;

    /// One character as it was found on screen.
    struct Placed {
        ScreenRect  box;        ///< where it sits, in frame coordinates
        std::string ch;
        float       quality = 0.0f;    ///< how well the shape correlated
        float       evidence = 0.0f;   ///< pixels it accounted for, net
    };

    /// Where each character of the last read was found.
    ///
    /// Useful beyond debugging: knowing which pixels spell a monster's name is
    /// what turns a name into a thing on the screen you can walk to.
    const std::vector<Placed>& last_placed() const { return last_placed_; }

    /// Every glyph that could begin at the region's left edge, best first.
    ///
    /// For asking why a letter came back wrong. A reader that answers only
    /// with its conclusion cannot be argued with; this shows the runners-up
    /// and what they scored, which is usually the whole explanation.
    std::vector<Placed> candidates(const Frame& f, const ScreenRect& r,
                                   std::size_t count) const;

    /// How certain the last read was, from how cleanly the glyphs stood out
    /// and how much of the line they accounted for. Low means the region was
    /// not text at all, which is the useful half: it is a reliable way to ask
    /// "is there a name here?" rather than guessing from pixel statistics.
    float last_confidence() const { return last_confidence_; }

    /// How bright a pixel is, for the purpose of finding text.
    ///
    /// The strongest channel, not the luminance. The game writes in white, in
    /// yellow, in green and in red, and a red stroke is dark by luminance
    /// while being as vivid as any other. What its text has in common is not
    /// a colour -- it is that the strokes are bright and the pixels hemming
    /// them in are not.
    static int level(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return r > g ? (r > b ? r : b) : (g > b ? g : b);
    }

    /// A stroke is at least this bright. Floating character names are drawn
    /// half-transparent and come out dimmer than the rest, which sets the bar.
    static constexpr int kMinInkLevel = 140;

    /// How much darker the sleeve around a stroke is than the stroke itself.
    ///
    /// This game outlines its text in black, so a letter is a thin bright
    /// thing in a dark sleeve. That, and not brightness, is what tells writing
    /// from scenery -- sunlit grass is as bright as any name and is lit in
    /// broad patches, so a reader that only asks "is this bright?" goes
    /// looking for letters on every hillside and takes most of a second a
    /// frame doing it.
    static constexpr int kOutlineDrop = 50;

    /// Whether a pixel could be part of a letter: bright, with the dark that
    /// outlines text above it or below it.
    static bool looks_like_stroke(const Frame& f, int x, int y);

    /// And at least this much brighter than what surrounds it. Every piece of
    /// text in this game is outlined in black, so the frame around a glyph is
    /// the evidence that it is one -- and for a glyph that is solid ink, an
    /// 'l' being a single column, it is the only evidence there is.
    static constexpr int kMinSeparation = 50;

    /// How closely a glyph's shape has to correlate with the pixels before it
    /// is accepted. 1.0 would be an exact copy; real text on a busy background
    /// lands well short of that, and noise lands well short of this.
    static constexpr double kMinQuality = 0.45;

    /// What each pixel of a glyph is worth once it is admissible.
    ///
    /// Inventing a stroke costs twice what finding one pays, which is the
    /// whole of the difference between reading an accent and imagining one: a
    /// tilde that is really there earns five pixels, and one that is not loses
    /// ten.
    ///
    /// A correct outline earns nothing. It is tempting to pay for it -- the
    /// black frame really is evidence -- but a bigger glyph has a bigger
    /// outline, so paying by the pixel pays for size, and an 'A' comes back as
    /// an 'Ạ' because the dot it invented cost less than the extra frame
    /// around it earned. An outline only ever speaks against: where the glyph
    /// says the page should be clear and it is not, that is a stroke belonging
    /// to some other letter.
    static constexpr double kStrokeFound    =  1.0;
    static constexpr double kStrokeInvented = -2.0;
    static constexpr double kOutlineRight   =  0.0;
    static constexpr double kOutlineWrong   = -1.0;

    /// Blank columns between glyphs before it counts as a space rather than
    /// ordinary letter spacing.
    static constexpr int kSpaceGap = 3;

    /// How far a pixel's colour may stray from a name's own strokes, in each
    /// channel, and still be the same writing (find_whole). Generous: the
    /// writing is drawn half-transparent and takes some of the colour behind
    /// it, while a glow or an icon beside it is another colour altogether.
    static constexpr int kSameInk = 60;

    /// How many strokes in a name's colour that are not its own letters make
    /// it part of some other writing (find_whole). One is a speck of
    /// something; the edge of a letter, or an accent, is several. The one
    /// mark this cannot tell from a speck is the single pixel under an "ị".
    static constexpr int kStrayInk = 2;

    /// How far below the line's foot a glyph may still sit. The files are
    /// cropped to their own ink, so a 'j' or a dot-below vowel ends lower than
    /// the letters beside it while sharing their baseline.
    static constexpr int kMaxDescent = 3;

private:
    struct Glyph {
        std::string               ch;       ///< UTF-8; Vietnamese needs several bytes
        int                       w = 0, h = 0;
        std::vector<std::uint8_t> ink;      ///< w*h, 1 where the glyph is drawn
        int                       ink_count = 0;

        /// What is actually matched against the screen: the strokes and the
        /// pixels hemming them in, over a box one pixel larger all round.
        /// 1 = stroke, 2 = its surround, 0 = ignored.
        ///
        /// Open space inside the box is left out on purpose. Counting it makes
        /// a tall glyph's score depend mostly on how uniform its background
        /// happens to be, so an 'Â' loses to an 'A' that explains less -- the
        /// taller box simply has more emptiness to dilute it. Matching the
        /// letter and its outline, which is all the game actually draws,
        /// compares glyphs of different sizes on even terms.
        std::vector<std::uint8_t> role;     ///< (w+2)*(h+2)
        int                       role_count = 0;

        /// A handful of stroke pixels spread across the glyph, checked before
        /// anything else. Almost every position a glyph is tried at is not
        /// that glyph, and four lookups reject it for the cost of one.
        int probe[4][2] = {};
        int probes = 0;
    };

    /// How bright a probe pixel has to be. Below the real threshold on
    /// purpose: a single pixel of dim or half-transparent text may fall short
    /// of it while the stroke as a whole does not, and this is a sieve, not a
    /// verdict.
    static constexpr int kProbeLevel = 110;


    /// A region of a frame reduced to one brightness per pixel, plus a mark
    /// for the pixels that could be part of a letter.
    struct Patch {
        std::vector<int>          level;
        std::vector<std::uint8_t> stroke;
        int x0 = 0, y0 = 0;     ///< where it came from, in frame coordinates
        int w = 0, h = 0;

        int at(int x, int y) const { return level[static_cast<std::size_t>(y) * w + x]; }
        bool is_stroke(int x, int y) const {
            return stroke[static_cast<std::size_t>(y) * w + x] != 0;
        }
        bool valid() const { return w > 1 && h > 4; }
    };

    static Patch patch_of(const Frame& f, const ScreenRect& r);

    /// The glyphs spelling a string, nulls for spaces. False if some character
    /// is not in the set.
    bool spell(const std::string& utf8, std::vector<const Glyph*>& out) const;

    /// What a glyph is worth at one position, measured two ways.
    struct Fit {
        /// How well the shape correlates with the pixels, in -1..1. Scale-free,
        /// so it answers "is this the shape?" the same way for white text,
        /// yellow text and half-transparent text. Decides whether a glyph is
        /// admissible at all.
        double correlation = -1.0;

        /// How many pixels the glyph actually accounts for, strokes it claims
        /// that are not there counting double against it. Decides which of the
        /// admissible glyphs to believe.
        ///
        /// Correlation cannot make that second decision. It is an average, so
        /// a glyph that explains more of the writing does not thereby score
        /// higher -- an 'ã' reads the tilde an 'a' ignores and is still the
        /// worse average fit, while an 'ã' guessed over a plain 'a' is barely
        /// penalised for the two strokes it invented. Counting pixels puts
        /// explaining and inventing on the same scale, where they belong.
        double evidence = 0.0;
    };

    Fit fit(const Glyph& g, const Patch& p, int px, int py) const;

    /// One way a sought string lines up with the screen: where its first
    /// letter fitted, and what walking the rest of the string from there
    /// found. In the patch's coordinates.
    struct Run {
        struct Letter {
            const Glyph* g = nullptr;
            int          x = 0, y = 0;     ///< where its box starts
        };
        int                 left = 0, right = 0;   ///< the letters found; right is one past them
        int                 top = 0, foot = 0;     ///< the first letter's top, and the baseline
        std::size_t         hits = 0;              ///< how many letters were found
        double              score = 0.0;           ///< their correlations, summed
        bool                last_found = false;    ///< whether the string's last letter was
        std::vector<Letter> letters;               ///< each letter found, and where
    };

    /// Every run of `want` across the patch that finds at least `need` of its
    /// letters, handed to `each` in the order the first letter was met. What
    /// find_text and find_whole both search with.
    void runs(const Patch& p, const std::vector<const Glyph*>& want, std::size_t need,
              const std::function<void(const Run&)>& each) const;

    /// Whether a run is the whole of its line, as find_whole means it. `line_h`
    /// is how far above the baseline the line's letters reach.
    bool alone(const Frame& f, const Patch& p, const Run& r, int line_h) const;

    bool load_dir(const std::wstring& dir);

    std::vector<Glyph>          glyphs_;
    int                         tallest_ = 0;
    /// The tallest plain capital, A to Z: how high a line of names reaches
    /// even where the name sought has no capital in it.
    int                         cap_height_ = 0;
    mutable float               last_confidence_ = 0.0f;
    mutable std::vector<Placed> last_placed_;
};

} // namespace gobot
