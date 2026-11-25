#pragma once

#include "gobot/mob_detector.hpp"      // ScreenRect
#include "gobot/screen_capture.hpp"

#include <memory>
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

    /// find_all with every buffer made afresh, as every search was made before
    /// 2026-11-16, when the buffers began to be kept from one search to the
    /// next and each picture's mask made once. For the test that holds the
    /// kept ones to the same answers, to the bit; nothing else should need it.
    static std::vector<Match> find_all_fresh(const Frame& frame, const ScreenRect& area,
                                             const Frame& tpl, float min_score,
                                             std::size_t limit, Tone tone);

    /// Several searches of one frame that turn it into grey, or colour, once.
    ///
    /// Each search starts by turning the part of the frame it looks at into
    /// grey (or colour without the alpha): for a whole 1024x768 frame that is
    /// 0.8 MB written before any matching starts, again for every picture
    /// looked for. One turn of the popups looks for four pictures in the same
    /// frame, and a look for windows in the way about nine; on a PC with one
    /// core it is time the game does not get.
    ///
    /// While one of these is alive, the searches its own thread makes of
    /// `frame` -- by find, find_all or TemplateStore::find -- cut their part
    /// from one conversion of the whole frame, made by the first of them that
    /// needs it, grey and colour once each. The answers are the same to the
    /// bit. Worth it only when the searches cover most of the frame: the whole
    /// of it is converted, however small the part looked at.
    ///
    /// The frame must stay as it is while this lives -- not moved, resized or
    /// grabbed into again -- or the searches see the picture as it was. Other
    /// frames, and other threads, are not touched. One may be made inside
    /// another, each for its own frame.
    class OneFrame {
    public:
        explicit OneFrame(const Frame& frame);
        ~OneFrame();
        OneFrame(const OneFrame&) = delete;
        OneFrame& operator=(const OneFrame&) = delete;

        /// The frame and its conversions: defined beside the matching, which
        /// alone looks inside.
        struct State;

    private:
        std::unique_ptr<State> state_;
    };

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
    /// becomes "buttons/ok". How many were loaded, and from where, is logged
    /// -- at debug only when `quiet`: for a folder read again and again, as
    /// TrashList::load reads the trash list's every half minute.
    bool load(const std::string& dir, bool quiet = false);

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

    /// load_best's set, one for the whole program.
    ///
    /// Every job used to load a set of its own: about 18 MB of pictures each,
    /// 3 to 9 of them at once, and on a PC with one core a second of reading
    /// PNGs at each job's start (2026-11-16). Now the first job to ask loads
    /// it, and every job that asks while someone still holds it gets that
    /// same one -- so nobody may change it: a job that wants pictures of its
    /// own puts them in a store of its own. It is loaded again when nobody
    /// holds it any more, and when a picture in the places load_best reads,
    /// or the name map, was added, removed or written since: a job starting
    /// then sees the pictures as they are, as each job's own load did.
    ///
    /// Never null. When load_best would fail, an empty store, and the next
    /// ask tries again. Safe from any thread; two jobs asking at once wait
    /// for one load rather than making two.
    static std::shared_ptr<const TemplateStore> shared_best(const std::string& name_map_ini = {});

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

    /// How many names it answers to, short names included.
    std::size_t size() const;
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
    /// The short names back into copies, as apply_names made them before
    /// 2026-11-16: before a put, which would otherwise change what a short
    /// name shows when it replaces the picture the name is for.
    void unalias();

    std::unordered_map<std::string, Frame> by_name_;
    /// Short name -> the name in by_name_ it shows (apply_names). A name,
    /// not a copy of the picture as before: there are some hundred of them.
    /// Looked at before by_name_, as a copy put over a picture of the same
    /// name used to win.
    std::unordered_map<std::string, std::string> aliases_;
    std::string                            source_;
};

} // namespace gobot
