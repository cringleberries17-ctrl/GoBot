#pragma once

#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/template_matcher.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gobot {

/// Where things are in the game's bag window, measured from the strip of tabs
/// across its middle.
///
/// The strip is the one part of the window that looks the same whatever the
/// bag holds -- Trang Bị 1, Trang Bị 2, Tiêu Dùng, Khác, Linh Thú, then + and
/// ≡ -- and the window can be dragged anywhere, so everything else is an
/// offset from it. The numbers are measured off the commercial client's own
/// picture of the window: slots 34 pixels square on a 37 pixel pitch, six
/// across and four on show, starting 25 pixels below the top of the strip.
/// They live in [Bag] in configs/trash.ini, so another skin is a file edit
/// rather than a rebuild.
struct BagLayout {
    int tab_w   = 37;   ///< each tab's width, from the strip's left edge
    int cell    = 34;   ///< one slot, its frame included
    int pitch   = 37;   ///< slot to slot
    int grid_dx = 1;    ///< the first slot, from the strip's top-left
    int grid_dy = 25;
    int cols    = 6;
    int rows    = 4;    ///< rows on show at once; the rest are scrolled to

    /// The scroll bar's two arrows, from the strip's top-left.
    int scroll_up_dx   = 236, scroll_up_dy   = 32;
    int scroll_down_dx = 236, scroll_down_dy = 159;

    /// Parts of a slot that say nothing about what is in it: the corner the
    /// stack count is printed in, and the edge, where the hover and selection
    /// highlights are drawn. Left out of every comparison, so that 67 potions
    /// and 3 of the same potion are one item, and so is the slot the pointer
    /// happens to be over.
    int count_w = 18;
    int count_h = 11;
    int edge    = 2;

    void load(const IniFile& ini);
};

/// The bag's tabs, in the order the strip shows them.
constexpr int kBagTabs = 5;
const char* bag_tab_name(int tab);

/// The bag as found in one frame.
struct BagView {
    bool       found = false;
    float      score = 0.0f;
    ScreenRect strip;              ///< the tab strip
    bool       has_trash = false;
    ScreenRect trash;              ///< the trash can

    ScreenRect slot(const BagLayout& l, int col, int row) const;
    std::vector<ScreenRect> slots(const BagLayout& l) const;   ///< row by row
    void tab_point(const BagLayout& l, int tab, int& x, int& y) const;
};

namespace bag {

/// Find the bag: its tab strip, then the trash can in the window's corner.
BagView find(const Frame& f, const TemplateStore& store);

/// A slot cut out of a frame, fully opaque.
Frame cut(const Frame& f, const ScreenRect& r);

/// The same picture with the count corner and the edge made see-through.
Frame masked(const Frame& picture, const BagLayout& l);

/// Nothing in it. An empty slot is the bag's dark gradient, whose brightest
/// pixels are around 33 of 255; the dimmest item icon in the client's whole
/// library peaks above 118.
bool is_empty(const Frame& f, const ScreenRect& r, const BagLayout& l);

/// A cheap fingerprint of what the grid shows, for telling whether a scroll
/// moved anything -- which is how the bottom of the bag is found.
std::uint64_t grid_hash(const Frame& f, const BagView& v, const BagLayout& l);

/// Whether two looks at the bag show the same items -- most slots alike --
/// which is how scrolling is found to have reached the bottom. Not an exact
/// comparison: a slot under the pointer lights up, and an item can glint.
bool same_page(const Frame& a, const BagView& va, const Frame& b, const BagView& vb,
               const BagLayout& l);

} // namespace bag

/// Reads the stack count printed in a slot's corner.
///
/// The trash dialog for a stack takes the exact number in it and nothing
/// else, so this has to be right. The client prints it in the same 5 by 8
/// pixel digits as everything else, white with a dark outline, a couple of
/// pixels in from the corner -- the commercial client's glyphs for those digits
/// match it pixel for pixel. Measured against real slots reading 7, 97, 100
/// and 67.
class CountReader {
public:
    /// `<font_dir>/lower/0.png` to `9.png`.
    bool load(const std::string& font_dir);
    bool loaded() const { return loaded_; }

    /// The number in the slot's corner, or 0 for none or unreadable.
    int read(const Frame& f, const ScreenRect& slot) const;

    /// Give a digit its shape directly. For tests.
    void set_digit(int d, int w, int h, const std::vector<std::uint8_t>& on);

private:
    struct Glyph {
        int w = 0, h = 0;
        std::vector<std::uint8_t> on;   ///< 1 where the digit is drawn
        int lit = 0;
    };
    Glyph digits_[10];
    bool  loaded_ = false;
};

/// Item icons the commercial client ships, named by item id: fragments,
/// materials, potions, buffs, seals.
///
/// For choosing what to throw away before it is ever in the bag. Matched
/// against a slot they score 0.91 to 1.00 for the right item and never above
/// 0.68 for a wrong one, once the corner with the count in it is left out.
struct ItemCatalog {
    struct Entry {
        std::string id;         ///< "spend/fragment/1103063"
        Frame       icon;       ///< with its own transparency
        Frame       tile;       ///< the icon on a slot's dark ground, for the screen
    };
    struct Group {
        std::string        folder;     ///< "spend/fragment"
        const char*        name;       ///< in Vietnamese, for the screen
        std::vector<Entry> entries;
    };
    std::vector<Group> groups;

    /// Load from a folder laid out like the client's `data\images`.
    bool load(const std::string& images_dir);
    const Entry* find(const std::string& id) const;
    std::size_t size() const;

    /// The staged copy beside this program, else the client's own.
    static std::string default_dir();
};

/// What to get rid of: pictures of items, cut from the player's own bag, kept
/// as PNGs in one folder for each way of getting rid of them.
///
/// Cut from the bag rather than picked from a library because no library has
/// them: the commercial client's covers potions, pets and seals, and nothing
/// that actually drops -- its equipment folder is empty. Whatever is in the
/// bag can be pointed at.
struct TrashList {
    enum class Action {
        Delete,     ///< into the trash can: gone
        Drop,       ///< onto the ground, where anyone can pick it up
    };
    static const char* folder(Action a);

    struct Item {
        std::string file;       ///< a picture cut from the bag, or empty
        std::string id;         ///< a catalog icon's id, or empty
        Action      action = Action::Delete;
        Frame       shown;      ///< for the screen
        Frame       picture;    ///< masked, for matching
        float       min_score = 0.0f;   ///< 0: the caller's
    };
    std::vector<Item> items;

    /// Add catalog icons by id. The count corner of each is left out, as for
    /// a picture cut from the bag; ids the catalog does not have are skipped.
    void add_catalog(const ItemCatalog& catalog, const std::vector<std::string>& ids,
                     Action a, float min_score);

    /// Load `dir`/delete/*.png and `dir`/drop/*.png. A folder that is not
    /// there yet is an empty list.
    bool load(const std::string& dir, const BagLayout& l);

    /// Save a slot picture under `dir` and add it. Returns the file, or empty.
    std::string add(const std::string& dir, const Frame& cut, Action a, const BagLayout& l);

    /// Add without writing a file. For tests.
    void add_picture(const Frame& cut, Action a, const BagLayout& l);

    /// Delete an item's file and take it off the list.
    bool remove(std::size_t index);

    /// Which item the slot holds, or -1.
    ///
    /// Colour has to agree as well as shape: three strengths of one potion are
    /// one drawing in three colours, and throwing away the wrong one is not
    /// undone. Searched a few pixels either side of the slot, since the grid
    /// is measured rather than given.
    int match(const Frame& f, const ScreenRect& slot, float min_score,
              float* score = nullptr) const;
};

/// configs/trash.ini.
struct CleanSettings {
    bool          enabled = false;
    int           interval_minutes = 10;
    std::uint32_t bag_key = 'I';
    bool          tabs[kBagTabs] = { true, true, true, true, false };

    /// How alike a slot has to be to a listed picture. High on purpose: a near
    /// miss here deletes something that was not on the list.
    float min_score = 0.90f;

    /// The same, for icons chosen from the catalog. Lower because those are
    /// the client's sprite rather than a photo of the slot -- the right one
    /// scores 0.91 and up, a wrong one at most 0.68.
    float catalog_score = 0.85f;

    /// Catalog ids chosen beforehand, from [Items].
    std::vector<std::string> delete_ids;
    std::vector<std::string> drop_ids;

    /// A bound on one pass, so a mistake costs a handful of items at most.
    int max_per_run = 40;

    /// Click the item and then the target, instead of dragging it there.
    bool click_to_move = false;

    BagLayout layout;

    bool load(const std::string& path);
};

/// One pass over the bag: open it, find what is on the list, get rid of it,
/// close it again.
///
/// Every step is checked against the screen before the next: the bag has to be
/// found before anything is touched, a dialog has to be one of the trash or
/// drop dialogs before its button is pressed, and an item has to be gone
/// before it counts. Anything unexpected and it backs out with Esc rather
/// than pressing on -- this is the one feature that destroys things.
class BagCleaner {
public:
    using Grab = std::function<bool(Frame&)>;
    using Report = std::function<void(const std::string&)>;

    BagCleaner(InputManager& input, const TemplateStore& store, Grab grab);

    void set_settings(const CleanSettings& s) { settings_ = s; }
    void set_trash(const TrashList* t) { trash_ = t; }
    void set_counts(const CountReader* c) { counts_ = c; }

    /// Where to keep a picture of the screen whenever a step goes wrong, so
    /// the next attempt can be fixed from what happened rather than guessed.
    /// Empty keeps none.
    void set_debug_dir(const std::string& dir) { debug_dir_ = dir; }
    void on_report(Report r) { report_ = std::move(r); }

    /// Look only: open the bag and go through it as usual, say what is on the
    /// list, and throw nothing away.
    void set_dry_run(bool on) { dry_run_ = on; }
    void cancel() { cancelled_ = true; }

    struct Result {
        bool ran = false;           ///< the bag was found and searched
        int  matched = 0;
        int  deleted = 0;
        int  dropped = 0;
        int  failed = 0;
        std::string trouble;        ///< why it stopped early, in Vietnamese
    };
    Result run();

private:
    bool grab_view(Frame& f, BagView& v);
    bool rid(Frame& f, BagView& v, int index, const TrashList::Item& item, int count);
    /// Answer the dialog the drag brought up. What came of it: done, nothing
    /// asked at all, or a step that did not go and was backed out of.
    enum { kFailed = 0, kDone = 1, kNoDialog = 2 };
    int  answer_dialog(TrashList::Action action, int count);
    void keep(const Frame& f, const char* what);
    void scroll_to_top(BagView& v);
    void say(const std::string& line);
    bool wait_ms(int ms);           ///< false when cancelled

    InputManager&        input_;
    const TemplateStore& store_;
    Grab                 grab_;
    CleanSettings        settings_;
    const TrashList*     trash_ = nullptr;
    const CountReader*   counts_ = nullptr;
    std::string          debug_dir_;
    Report               report_;
    bool                 dry_run_ = false;
    std::atomic<bool>    cancelled_{false};
};

} // namespace gobot
