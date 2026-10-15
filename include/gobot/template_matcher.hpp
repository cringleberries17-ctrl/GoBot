#pragma once

#include "gobot/mob_detector.hpp"      // ScreenRect
#include "gobot/screen_capture.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace gobot {

/// Finds a known picture somewhere on the screen.
///
/// Most of what a bot has to do outside combat is press the right part of a
/// dialog: accept the party, confirm the purchase, click through the launcher,
/// pick the fish up. None of that is reachable by reading text or packets --
/// the game draws a button and waits, and the only way to know where the
/// button is, is to recognise it.
///
/// Matching is by zero-mean normalised correlation, so a template still
/// matches when the game draws it over a different background or at a
/// different brightness. Where a template has transparency, only its opaque
/// pixels count: a button with rounded corners is matched by its face and not
/// by whatever happens to show through the corners.
class TemplateMatcher {
public:
    struct Match {
        bool       found = false;
        ScreenRect at;              ///< where it sits, in frame coordinates
        float      score = 0.0f;    ///< -1..1; 1 is identical
    };

    /// Whether a match has to agree about colour.
    ///
    /// Grey correlates one channel instead of three and is enough for almost
    /// everything here: dialogs, buttons and panels are told apart by their
    /// shape. Colour is for the sets that are one drawing in several hues --
    /// the four crest tiers, the sixteen exchange items -- where shape alone
    /// matches all of them equally well and picks whichever comes first.
    enum class Tone { Grey, Colour };

    /// Search `area` of `frame` for `tpl`, which may carry a mask.
    ///
    /// Searching a whole screen for a small picture is a few billion
    /// comparisons done directly, so it is done twice instead: once over a
    /// quarter-size copy of both, which costs a few hundredths of that, and
    /// then properly in the handful of places that looked promising. The
    /// answer is the same and it arrives in about a millisecond.
    static Match find(const Frame& frame, const ScreenRect& area,
                      const Frame& tpl, float min_score = kDefaultScore,
                      Tone tone = Tone::Grey);

    /// Every place the template matches at or above `min_score`, strongest
    /// first, with overlapping hits merged. For counting things -- how many
    /// empty inventory slots, how many of an item in a list.
    static std::vector<Match> find_all(const Frame& frame, const ScreenRect& area,
                                       const Frame& tpl,
                                       float min_score = kDefaultScore,
                                       std::size_t limit = 64,
                                       Tone tone = Tone::Grey);

    /// How alike counts as the same picture.
    ///
    /// The commercial helper this is modelled on uses 0.55 for its launcher
    /// pages and fails below it, which is about right for a picture drawn over
    /// changing scenery. Anything matched against a fixed panel can be held to
    /// a much higher standard, so callers that know better should say so.
    static constexpr float kDefaultScore = 0.78f;

    /// A pixel counts as part of the template when it is at least this opaque.
    static constexpr int kOpaque = 128;

    /// What is doing the matching, in words, with its version.
    ///
    /// Reported at startup and shown in the front-end because otherwise there
    /// is no way to tell. OpenCV is linked statically -- it lives inside the
    /// executable rather than in a DLL beside it -- so "there are no DLLs in
    /// the build folder" looks exactly like "OpenCV is not being used", and
    /// the only honest answer to that is for the program to say which it is.
    static std::string backend();

    /// Whether matching is done by OpenCV.
    ///
    /// Worth asking, because the two implementations differ in what they can
    /// tell apart. The hand-rolled one reduces everything to grey, so one icon
    /// in red and the same icon in blue are the same picture to it -- which
    /// matters here, where four crest tiers and sixteen exchange items are
    /// exactly that. OpenCV correlates the colour channels and separates them.
    static bool uses_opencv();

private:
    /// A picture reduced to one grey value per pixel, with the see-through
    /// parts marked, at a given reduction.
    struct Reduced {
        std::vector<int>          grey;
        std::vector<std::uint8_t> solid;
        int w = 0, h = 0;
        int count = 0;            ///< solid pixels
    };

    static Reduced reduce(const Frame& f, const ScreenRect& area, int step);
    static double  score_at(const Reduced& hay, const Reduced& needle, int px, int py);
};

/// A named set of templates, loaded from a directory of PNGs.
///
/// Kept apart from the matcher so that what to look for is data rather than
/// code: adding a dialog the bot should recognise is adding a file, not a
/// rebuild. Each file's stem is its name.
class TemplateStore {
public:
    /// Load every PNG under `dir`, recursing. A file at `buttons\ok.png`
    /// becomes "buttons/ok".
    bool load(const std::string& dir);

    /// Load the pictures embedded in a .NET assembly, in place.
    ///
    /// The helper this borrows from carries its template library inside its
    /// own executable rather than as files. They are the game's artwork --
    /// its dialogs, its buttons, its item icons -- extracted once so that a
    /// program can recognise them; capturing the same pictures again off a
    /// running client would take days and come out worse.
    ///
    /// So they are read from wherever that program is installed and are never
    /// copied here, exactly as the glyph set is. Nothing of theirs is
    /// redistributed, and a machine without their installer simply has no
    /// templates and the features that need them stay off.
    ///
    /// They arrive unnamed, since the assembly does not name them. Each is
    /// called "t" and its ordinal -- t000, t001 -- and `name_map` gives the
    /// useful ones a name. Working out which picture is which is this
    /// project's own doing and lives in this project's own config.
    bool load_from_assembly(const std::string& dll_path,
                            const std::string& name_map_ini = {});

    /// Where that helper is installed, if it is.
    static std::string helper_assembly();

    /// Load from the best source available, and say which was used.
    ///
    /// The pictures are the game's own UI artwork, and there is more than one
    /// place to get it from. In order of preference:
    ///
    ///   1. `templates\` beside this executable -- staged at build time, so a
    ///      built bot carries what it needs and starts without hunting.
    ///   2. The commercial client's extracted set. A thousand pictures, named
    ///      after what they are, and -- the part that matters -- holding both
    ///      of the game's two interface skins. Guessing the skin wrong is what
    ///      made the first attempt at this fail silently.
    ///   3. The helper's embedded set. Fewer, unnamed, and from whichever skin
    ///      its author happened to use, so last.
    ///
    /// None of it is copied into this project's source. It is the game's art,
    /// and a machine that has the game has it already.
    bool load_best(const std::string& name_map_ini = {});

    /// Which source the last `load_best` used, for saying so.
    const std::string& source() const { return source_; }

    /// One picture by its short name, from the first of the places
    /// `load_best` looks in that has it -- the staged `templates\`, then the
    /// commercial client's set, then the helper's assembly -- without loading
    /// the thousand others. `name_map_ini` says which file the short name is.
    /// False when none has it; `from` gets where it came from, for saying so.
    static bool load_one(const std::string& name, const std::string& name_map_ini,
                         Frame& out, std::string* from = nullptr);

    /// The commercial client's extracted pictures, if that client is here.
    static std::string client_images();

    std::size_t size() const { return by_name_.size(); }
    bool        has(const std::string& name) const;

    /// The picture, or null if nothing of that name was loaded.
    const Frame* get(const std::string& name) const;

    /// Every name held, sorted. For saying what is available when a lookup
    /// fails, which is otherwise a silent no-op that looks like a bug in
    /// whatever asked.
    std::vector<std::string> names() const;

    /// Convenience: find a named template in a frame.
    TemplateMatcher::Match find(const std::string& name, const Frame& frame,
                                const ScreenRect& area,
                                float min_score = TemplateMatcher::kDefaultScore) const;

    /// Where this project keeps them, next to the executable.
    static std::string default_dir();

    /// Put a picture in under a name, replacing anything already there.
    ///
    /// For tests, which need a store of known pictures without a game or a file
    /// on disk, and for anything that grabs a picture from the live client and
    /// wants to use it immediately.
    void put(const std::string& name, const Frame& picture);

    /// Give loaded pictures the short names this project knows them by.
    /// Returns how many were named.
    ///
    /// Done for you by the loads that take a name map. Public because a
    /// caller loading from a directory of its own choosing wants the names
    /// too, and should not have to go through `load_best` to get them.
    int apply_names(const std::string& name_map_ini);

private:
    std::unordered_map<std::string, Frame> by_name_;
    std::string                            source_;
};

} // namespace gobot
