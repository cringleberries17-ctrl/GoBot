#pragma once

#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/template_matcher.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
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
    int scroll_down_dx = 236, scroll_down_dy = 163;

    /// The scroll bar's track between the arrows, down the same column, first
    /// and last row inclusive. A white thumb slides in it: where it sits says
    /// which row of the tab is at the top of the view, and how long it is says
    /// how many rows the tab has. Measured on the live client, where one click
    /// on an arrow scrolls two rows or three, so counting clicks says nothing.
    int track_top_dy = 39, track_bottom_dy = 155;

    /// The trash bin, the square in the window's bottom-right corner: from the
    /// strip's top-left to the can it shows when empty. Measured on the live
    /// client, and the same in the commercial client's picture of the window.
    ///
    /// Taken from here rather than searched for. The bin shows the can only
    /// while it is empty; with an item in it the nearest thing to a can is
    /// whatever grey item is in the bag, and a drag aimed there moves things
    /// about inside the bag.
    int bin_dx = 228, bin_dy = 178;
    int bin_w  = 16,  bin_h  = 22;

    /// The bag window's own close button, its top-left from the strip's
    /// top-left: where it is on every one of the 704 saved frames of the live
    /// client with the bag open. A close button anywhere else is another
    /// window's (bag::other_window_up).
    int close_dx = 228, close_dy = -224;

    /// Parts of a slot that say nothing about what is in it: the corner the
    /// stack count is printed in, and the edge, where the hover and selection
    /// highlights are drawn. Left out of every comparison, so that 67 potions
    /// and 3 of the same potion are one item, and so is the slot the pointer
    /// happens to be over.
    int count_w = 18;
    int count_h = 11;
    int edge    = 2;

    /// The opposite corner, from the slot's bottom-right, where the client
    /// draws a badge over some items -- a green ring with a red P -- left out
    /// the same way. Measured on the live client, on a buff and on a card:
    /// 11 by 11 pixels from the slot's 24th column and 23rd row, so 12 by 12
    /// holds it with a column to spare. Compared, it held the badged buff to
    /// 0.87 on its own icon, under catalog_score, and it was never taken for
    /// it; left out, 0.99.
    int badge_w = 12;
    int badge_h = 12;

    void load(const IniFile& ini);
};

/// The bag's tabs, in the order the strip shows them.
constexpr int kBagTabs = 5;
const char* bag_tab_name(int tab);
/// Tiêu Dùng, the tab of things that are used: the only one where anything is
/// right-clicked.
constexpr int kUseTab = 2;

/// The bag as found in one frame.
struct BagView {
    bool       found = false;
    float      score = 0.0f;
    ScreenRect strip;              ///< the tab strip
    ScreenRect trash;              ///< the trash bin, from the layout
    /// The can is showing. Not the same as nothing waiting: while the bin's
    /// own dialog is open the dialog holds the item and the can is back.
    bool       bin_empty = false;

    /// Rows on show that are rows of slots, from the top; -1 for all of them.
    /// The last row of a tab is the button for buying more room, once it is
    /// scrolled to, and that is not six slots.
    int slot_rows = -1;

    /// What the scroll bar's thumb says: the tab's row at the top of the
    /// view, and how many rows the tab has. -1 when the thumb cannot be read
    /// -- covered by a dialog, say.
    int first_row = -1;
    int total_rows = -1;

    /// Which tab the strip shows chosen, from 0: the client draws it darker
    /// than the rest -- 92 to 99 of 255 against 115 to 123 for the others,
    /// measured on the live client and on its own picture of the strip. -1
    /// when no tab stands out like that. Nothing is right-clicked unless this
    /// says Tiêu Dùng: on another tab a right-click equips what it lands on.
    int selected_tab = -1;
    bool scroll_known() const { return first_row >= 0 && total_rows > 0; }
    bool at_top() const { return scroll_known() && first_row == 0; }
    bool at_bottom(const BagLayout& l) const {
        return scroll_known() && first_row + l.rows >= total_rows;
    }

    ScreenRect slot(const BagLayout& l, int col, int row) const;
    /// The slots of the rows that are slots, row by row.
    std::vector<ScreenRect> slots(const BagLayout& l) const;
    void tab_point(const BagLayout& l, int tab, int& x, int& y) const;
};

namespace bag {

/// Find the bag by its tab strip, and say whether its trash bin shows the
/// can, which rows on show are slots, and where the scroll bar is.
BagView find(const Frame& f, const TemplateStore& store, const BagLayout& l);

/// Nothing of the bag's is waiting on an answer: no dialog with the two
/// buttons on screen, and the bin, when the bag is open, showing its can. What
/// a pass that could not give an item back waits for before the bot goes on.
bool nothing_held(const Frame& f, const TemplateStore& store, const BagLayout& l);

/// A slot cut out of a frame, fully opaque.
Frame cut(const Frame& f, const ScreenRect& r);

/// The same picture with the count corner, the badge corner and the edge
/// made see-through.
Frame masked(const Frame& picture, const BagLayout& l);

/// Nothing in it. An empty slot is the bag's dark gradient, whose brightest
/// pixels are around 33 of 255; the dimmest item icon in the client's whole
/// library peaks above 118.
bool is_empty(const Frame& f, const ScreenRect& r, const BagLayout& l);

/// A cheap fingerprint of what the grid shows.
std::uint64_t grid_hash(const Frame& f, const BagView& v, const BagLayout& l);

/// Whether two looks at the bag show the same items, slot for slot. Only for
/// when the scroll bar cannot be read: it is an exact comparison, near
/// enough, so a sparse page and the empty one below it are not taken for one
/// page -- which ended scrolling before the rows that had items in them.
bool same_page(const Frame& a, const BagView& va, const Frame& b, const BagView& vb,
               const BagLayout& l);

/// How alike two cuts of one slot are, in shape, -1 to 1, leaving out the
/// count corner and the edge. For telling whether an item is still where it
/// was; -1 when either is flat, which an empty slot nearly is.
float alike(const Frame& a, const Frame& b, const BagLayout& l);

} // namespace bag

/// Reads numbers in the game's 5 by 8 pixel digits: the stack count in a
/// slot's corner, and the number in a dialog's quantity box.
///
/// The bin's quantity dialog is confirmed only when the number the game put
/// in its box reads as the count in the slot's corner, and a deletion cannot
/// be undone, so both have to be right. The client prints both in the same
/// digits as everything else, white with a dark outline -- the commercial
/// client's glyphs for those digits match them pixel for pixel. Measured
/// against real slots reading 2, 7, 97, 100 and 67, and boxes reading 1 and
/// 2, the 2 with the box's caret over its last column.
class CountReader {
public:
    /// Something white is there that does not read as a number.
    static constexpr int kUnreadable = -1;

    /// `<font_dir>/lower/0.png` to `9.png`.
    bool load(const std::string& font_dir);
    bool loaded() const { return loaded_; }

    /// The stack count in the slot's corner: the number printed there; 1 when
    /// nothing is, since the client prints counts from 2 up and a single item
    /// of a kind that stacks still gets the quantity box, with 1 in it; or
    /// kUnreadable when something white is in the corner but no number reads,
    /// or when the digits are not loaded.
    int read(const Frame& f, const ScreenRect& slot) const;

    /// A number written in `area`: 0 when there is nothing white in it at all,
    /// kUnreadable when there is but it does not read, or the digits are not
    /// loaded.
    ///
    /// This is for a quantity box. Its caret -- a yellow bar two pixels wide,
    /// right after the number -- blinks about half a second on and half off,
    /// and while it is on it stands over the last column of a digit five
    /// pixels wide. What is under it cannot be seen, so its pixels are left
    /// out of the comparison, neither stroke nor ground; a digit with too much
    /// of itself under it does not read, and then neither does the number.
    /// White that the digits read leave over, or a digit read where another,
    /// too much of it under the caret to read, would fit what can be seen as
    /// well, makes it kUnreadable -- never the digits that did read, as if
    /// they were the whole number.
    int read_field(const Frame& f, const ScreenRect& area) const;

    /// Give a digit its shape directly. For tests.
    void set_digit(int d, int w, int h, const std::vector<std::uint8_t>& on);

private:
    /// Digits in `area`, left to right, the line starting no lower than
    /// `max_top`: the number, or -1 when none reads. `white` gets how many
    /// pixels there were white enough to be part of a digit, and `stray` how
    /// many of those lie outside every digit read. With `caret`, the caret's
    /// yellow pixels are unknown rather than ground.
    int scan(const Frame& f, const ScreenRect& area, int max_top, bool caret, int& white,
             int& stray) const;

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
/// For choosing what to throw away before it is ever in the bag. The client
/// draws most of them exactly as they are here -- potions match their slots at
/// 1.00 -- but not always in their colours: the fragments in the bag cycle
/// from gold to orange to red and back, and GoC's own pictures of them in the
/// bag are red. So a slot is matched by the drawing alone. Several icons are
/// one drawing in two or three colours, and a few pairs are the same picture
/// under two ids; by the player's rule those are one thing, and choosing one
/// throws all of them away -- same_drawing says which, and the trash tab
/// shows it before anything is deleted.
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

    /// Other ids whose icon is the same drawing as this one's, in any colour:
    /// each found in the other at TrashList::kSameDrawing or better. Choosing
    /// this one throws those away too. Icon against icon, the badge corner
    /// included: an icon never has a badge, and two that differ only there
    /// are two things.
    std::vector<std::string> same_drawing(const std::string& id) const;

    /// The staged copy beside this program, else the client's own.
    static std::string default_dir();
};

/// configs/trash.ini.
struct CleanSettings {
    bool          enabled = false;
    int           interval_minutes = 10;
    std::uint32_t bag_key = 'I';
    bool          tabs[kBagTabs] = { true, true, true, true, false };

    /// How alike a slot's drawing has to be to a listed picture's, colour
    /// left out. High on purpose: a near miss here deletes something that was
    /// not on the list. Measured on the clean, uncovered slots of every real
    /// frame, a picture cut from the bag scores 0.93 or more on the same item
    /// in another colour, and 0.81 at most on anything else.
    ///
    /// Not on its own a bar against every other item, though. With the badge
    /// corner left out, two potions that differ mostly there -- the plain
    /// gourd and the decorated one, drawn into slots as the client draws them
    /// -- score up to 0.92 on each other's slots. What keeps them apart is the
    /// other gourd's icon, which matches its own slot better and is a rival
    /// to the picture: see TrashList::add_rivals.
    float min_score = 0.90f;

    /// The same, for icons chosen from the catalog. The right icon scores
    /// 0.947 or more on its item whatever colour the bag draws it in -- the
    /// least on a red piece like the ones GoC photographs -- and 0.99 on the
    /// badged buff; another drawing 0.77 at most.
    float catalog_score = 0.90f;

    /// Catalog ids chosen beforehand, from [Items].
    std::vector<std::string> delete_ids;
    std::vector<std::string> drop_ids;
    /// Fragments to register in Yêu Quái Bách Khoa, from [Items] use=. An id
    /// that is on `delete_ids` as well is registered first, and what is left
    /// of the stack registered deleted after: in that pass only, from that
    /// one slot only, and only once the registration has been seen through.
    std::vector<std::string> use_ids;

    /// [Trash] use_fragments: whether a pass registers anything at all. Off
    /// unless the file says so, and it ships off: a registration cannot be
    /// undone, and the picture of the question being there is no say-so --
    /// with it shipped, every build has it. Off, the pieces on the Dùng list
    /// are only said, as Thử tìm says them, and never right-clicked.
    bool use_fragments = false;

    /// A bound on one pass, so a mistake costs a handful of items at most.
    int max_per_run = 40;
    /// The same for registering, [Trash] max_uses_per_run: a registration
    /// cannot be undone either.
    int max_uses_per_run = 20;

    /// Click the item and then the target, instead of dragging it there.
    bool click_to_move = false;

    BagLayout layout;

    /// False, and nothing changed, when the file cannot be read or has no
    /// [Trash] section -- which is also what a file caught half way through
    /// being rewritten looks like. Keys it does not know are passed over,
    /// fragments_loose among them: colour no longer counts for anything.
    bool load(const std::string& path);

    /// Write use_ids, delete_ids and drop_ids to [Items] use=, delete= and
    /// drop= of the file at `path`, each line changed in place and the rest
    /// of the file left as it is.
    ///
    /// A pass reads the file every second and again just before it starts,
    /// and may read it between any two of the writes this takes. No such read
    /// may find an id on delete= and not on use= that was on use= before or
    /// will be after: a fragment to be registered first would be deleted
    /// outright, for good. So use= is written first holding every id it held
    /// and every id it will hold, then delete= and drop=, and use= as it now
    /// is last. `between`, when set, is called between two writes: for tests.
    bool save_items(const std::string& path, const std::function<void()>& between = {}) const;
};

/// What to get rid of: pictures of items, cut from the player's own bag and
/// kept as PNGs in one folder for each way of getting rid of them, and icons
/// chosen from the catalog by id.
///
/// Cut from the bag because no library has everything: the commercial
/// client's covers potions, pets and seals, and nothing that actually drops --
/// its equipment folder is empty. Whatever is in the bag can be pointed at.
struct TrashList {
    enum class Action {
        Delete,     ///< into the trash can: gone
        Drop,       ///< onto the ground, where anyone can pick it up
        /// Right-clicked on Tiêu Dùng and its question confirmed: a fragment
        /// registered in Yêu Quái Bách Khoa, which cannot be undone -- "một
        /// khi đã đăng ký, bạn sẽ không thể lấy lại mảnh ghép đó", as GoC's
        /// own page of the book says. GoC's "Sử dụng mảnh ghép".
        Use,
    };
    static const char* folder(Action a);

    /// How alike two pictures have to be, each found in the other, to be one
    /// drawing in whatever colours. Measured on the client's icons in grey:
    /// one drawing in two colours scores 0.89 to 1.00 both ways, two drawings
    /// 0.81 at most -- and one that holds the other, a bag with a potion
    /// beside it and the same bag alone, scores high one way only.
    static constexpr float kSameDrawing = 0.85f;
    /// How much better a listed item has to match than anything in the catalog
    /// that is drawn differently and not listed. Measured on every real frame,
    /// the right icon beats the best other drawing by 0.14 or more, so this
    /// only ever comes into it when two drawings are a near tie. Not wider: a
    /// drawing that holds another -- the bag with a potion beside it holds
    /// the bag alone, found in it at 0.91 -- would then never be thrown away.
    static constexpr float kRivalMargin = 0.05f;

    struct Item {
        std::string file;       ///< a picture cut from the bag, or empty
        std::string id;         ///< a catalog icon's id, or empty
        Action      action = Action::Delete;
        Frame       shown;      ///< for the screen
        Frame       picture;    ///< masked, for matching
        /// For a picture cut from the bag: masked with the badge corner kept,
        /// for add_rivals to compare with the catalog's icons. Empty for an
        /// icon.
        Frame       drawing;
        float       min_score = 0.0f;   ///< 0: the caller's

        /// Catalog ids drawn like what was chosen: its own and the same
        /// drawing in other colours. By the player's rule they are one thing,
        /// so none of them is a rival to this item.
        std::vector<std::string> kin;

        /// For an icon on the list only because it is drawn like one that was
        /// chosen: the one that was. Empty for what was chosen itself.
        std::string via;

        /// A Use item that is on the delete list as well: what is left of a
        /// stack a pass has just registered is deleted in that pass -- a piece
        /// is registered once, and its copies are no more use. That stack
        /// only, from the very slot it was registered from. A fragment's icon
        /// says only which of a monster's nine pieces it is, so another slot
        /// drawn the same is most likely another monster's piece: it is never
        /// deleted before a registration of its own.
        bool then_delete = false;

        /// A fragment, and so something a pass may right-click when it is on
        /// the Dùng list: one of the client's fragment icons, or a picture cut
        /// from the bag that add_rivals found drawn like fragment icons and
        /// like nothing else in the catalog. A right-click on anything else
        /// uses it or equips it there and then -- a potion drunk, a box
        /// opened, a scroll read -- so a picture of that on the Dùng list is
        /// only ever said, never right-clicked.
        bool fragment() const;
    };
    std::vector<Item> items;

    /// Catalog icons not on the list -- everything a listed item has to look
    /// more like the slot than, unless it is drawn the same. Filled by
    /// add_rivals.
    std::vector<Item> rivals;

    /// Add catalog icons by id, and every icon drawn the same, as items that
    /// say which one brought them. The count corner and the badge corner of
    /// each are left out, as for a picture cut from the bag; ids the catalog
    /// does not have are skipped.
    void add_catalog(const ItemCatalog& catalog, const std::vector<std::string>& ids,
                     Action a, float min_score);

    /// Every catalog icon that is not on the list becomes a rival, and each
    /// picture cut from the bag learns which icons are drawn like it: the
    /// icons it is one drawing with, the badge corner compared too, as
    /// same_drawing compares two icons -- and those icons' own colours. Call
    /// after everything is added.
    void add_rivals(const ItemCatalog& catalog);

    /// The whole list a pass goes by: the pictures under `dir`, the ids in the
    /// settings, and the rest of the catalog to tell them from. An id chosen
    /// both to use and to delete is one Use item with then_delete.
    void build(const std::string& dir, const CleanSettings& s, const ItemCatalog& catalog);

    /// Load `dir`/use/*.png, `dir`/delete/*.png and `dir`/drop/*.png, in that
    /// order. A folder that is not there yet is an empty list.
    bool load(const std::string& dir, const BagLayout& l);

    /// Save a slot picture under `dir` and add it. Returns the file, or empty.
    std::string add(const std::string& dir, const Frame& cut, Action a, const BagLayout& l);

    /// Add without writing a file. For tests.
    void add_picture(const Frame& cut, Action a, const BagLayout& l);

    /// Delete an item's file and take it off the list.
    bool remove(std::size_t index);

    /// What a slot holds, as far as the list is concerned.
    struct Verdict {
        int   item = -1;            ///< the listed item it is, or -1
        float score = 0.0f;         ///< how alike it is to that item

        /// When the slot was taken for an item on the Dùng list though an item
        /// on the delete or drop list matched it better: that item, and how
        /// alike it was -- kept when the Dùng item is then refused for a
        /// rival, since the slot is left alone rather than deleted. For saying
        /// why it is not deleted; see judge.
        int   passed_over = -1;
        float passed_over_score = 0.0f;

        /// When it looks like a listed item and is still not taken for it,
        /// why: something in the catalog that is not listed, and is drawn
        /// differently, looks as much like it. For saying so on a look-only
        /// pass.
        enum class Doubt { None, Rival };
        Doubt       doubt = Doubt::None;
        int         like = -1;      ///< the listed item it looks like
        float       like_score = 0.0f;
        std::string rival;          ///< the unlisted look-alike
        float       rival_score = 0.0f;
    };

    /// Which listed item the slot holds, if any.
    ///
    /// By its drawing, whatever its colour: the bag draws fragments in a
    /// colour that cycles, and the player's rule is that the drawing decides.
    /// Compared in grey, with the count corner, the badge corner and the edge
    /// left out. What keeps a look-alike from being thrown away is the rest
    /// of the catalog: no unlisted icon of another drawing may match the slot
    /// within kRivalMargin of the listed one. Searched a few pixels either
    /// side of the slot, since the grid is measured rather than given.
    ///
    /// A slot that an item on the Dùng list matches, at that item's own
    /// threshold, is that item's however much better an item on the delete or
    /// drop list matches it: the piece is registered first, and what is left
    /// of it deleted only when the Dùng item says so (then_delete) -- never
    /// deleted whole, which cannot be had back. Whatever the order of the
    /// list: a picture cut from the bag scores 1.00 on its own slot and a
    /// catalog icon 0.947 or more, so an older cut on the delete list would
    /// otherwise take every slot of a fragment chosen later to use.
    Verdict judge(const Frame& f, const ScreenRect& slot, float min_score) const;

    /// The items on the delete or drop list drawn like an item on the Dùng
    /// list, by the catalog icons each was found drawn like (kin), as indexes
    /// into `items`: a slot holding one is the Dùng item's (judge), so it is
    /// registered rather than deleted. For the tab to say so. After
    /// add_rivals, which is what tells a picture cut from the bag its kin.
    std::vector<std::size_t> yielding_to_use() const;

    /// judge, reduced to the item, or -1. `score` gets how alike it is -- or,
    /// for a slot that is refused, how alike it was to what it resembles.
    int match(const Frame& f, const ScreenRect& slot, float min_score,
              float* score = nullptr) const;
};

namespace bag {

/// A dialog with [Xác Nhận][Hủy Bỏ] under it, and whether it is one of the
/// bin's own.
///
/// Told apart by its words, not by its frame: every dialog in the game has the
/// same frame and the same two buttons, and the revive question, a party
/// invite and a shop's quantity box all pass for the trash dialog when only
/// the frame is compared. So the panel of words above the buttons is matched,
/// at the one place it sits relative to them.
struct Dialog {
    ScreenRect pair;                ///< the two buttons
    /// Which of the bin's dialogs it is, or -1 for one that is not the bin's.
    int        kind = -1;
    bool       quantity = false;    ///< it has a box for how many
    ScreenRect box;                 ///< that box, when it has one
    float      words = 0.0f;        ///< how alike its words are to the picture's

    /// The question a fragment's right-click brings up, to register it in
    /// Yêu Quái Bách Khoa: its words match the picture of it (kRegisterDialog)
    /// at 0.99, where that picture has them for these buttons -- the same box
    /// with a line or a word of other words is not it. Never one of the bin's
    /// -- ours() is false for it -- and only true when that picture is there.
    bool       registers = false;
    /// How alike its words are to that picture's: from 0.90, else 0.
    float      register_words = 0.0f;

    bool ours() const { return kind >= 0; }
    int confirm_x() const { return pair.x + pair.w / 4; }    ///< Xác Nhận
    int cancel_x() const { return pair.x + pair.w * 3 / 4; } ///< Hủy Bỏ
    int y() const { return pair.cy(); }
};

/// The kinds of the bin's dialogs, for Dialog::kind.
constexpr int kAskDialog = 0;       ///< "Bạn có thật sự muốn bỏ vào thùng rác không?"
constexpr int kCountDialog = 1;     ///< "Vui lòng nhập số lượng cần xóa vào thùng rác."
const char* dialog_name(int kind);

/// The picture of the question a fragment's right-click brings up, under its
/// name in templates\: the whole box, its words and its two buttons, cut from
/// the live client on 2026-10-19 (live\shots\fragq_A_085: 204x86 from 538,317,
/// see-through outside its rounded outline) and named in templates.ini.
///
/// "Bạn có muốn đăng ký mảnh quái vật vào Bách Khoa Quái Vật không?", on three
/// lines: the same words for every piece -- the question does not name the
/// piece or its monster -- so one picture tells them all. The box is the
/// game's dialog frame a line taller than the bin's question, so its words and
/// its two buttons are looked for where this picture has them, not where the
/// bin's has them; a picture whose buttons are not there is not used.
///
/// A part of the picture can still be made see-through -- only what is left
/// is compared -- so long as enough words are left and those words are none of
/// another question's (see can_check).
///
/// The picture is no switch: a pass right-clicks nothing unless [Trash]
/// use_fragments is on as well (CleanSettings::use_fragments), and it ships
/// off.
inline const char* const kRegisterDialog = "grabbed/fragment_register";

/// Every dialog with the two buttons on screen, the bin's own marked.
std::vector<Dialog> find_dialogs(const Frame& f, const TemplateStore& store);

/// How alike the words over `d`'s buttons are to each of the bin's pictures,
/// indexed by kind: the best within two pixels of where that picture puts
/// them, however low, or -1 where there is no picture with words. For saying
/// how near a dialog came to being taken for the bin's -- find_dialogs takes
/// it from 0.90 -- not for deciding anything.
std::vector<float> words_scores(const Frame& f, const TemplateStore& store, const Dialog& d);

/// One of the bin's dialogs is on screen. For anything else that presses
/// Xác Nhận on a dialog -- a party invitation's footer picture matches the
/// bin's quantity dialog at 0.90 -- to keep its hands off.
bool bin_asking(const Frame& f, const TemplateStore& store);

/// Whether the pictures to tell the dialogs for `a` apart are there. There is
/// no picture of the drop question with its words, so nothing is dropped:
/// a dialog that cannot be told from the others is not answered. For Use, the
/// registration question's picture (kRegisterDialog), with words in it and its
/// two buttons where the bin's question has them -- and words that are none
/// of the other questions GoBot has a picture of: the bin's, the revive
/// question, dropping and selling. Words cut down to their fixed part, the
/// piece's name see-through, could otherwise be no more than the "Bạn có
/// muốn" every question opens with.
bool can_check(const TemplateStore& store, TrashList::Action a);

/// A picture cut from the bag is a fragment, as a pass decides it for one on
/// the Dùng list (TrashList::Item::fragment): drawn like the catalog's
/// fragment icons, and like nothing else in it. What the bag picture's "Dùng"
/// is offered for.
bool fragment_cut(const Frame& cut, const BagLayout& l, const ItemCatalog& catalog);

/// A message with only Xác Nhận under it is up, and no dialog with two buttons:
/// what the client shows when it refuses something. Never answered.
bool notice_up(const Frame& f, const TemplateStore& store);

/// The client's close buttons, every drawing it has for one -- the bag's own X
/// among them, and those of Kho Cash, a shop and the monster book -- by the
/// names the template store gives them.
inline const char* const kCloseButtons[] = { "inven/1/btn_CloseDialog.spr/0", "inven/1/btn_CloseBox.spr/0",
                                             "inven/1/monster-close" };

/// Every picture other_window_up looks with is in `store`.
bool can_see_windows(const TemplateStore& store);

/// Another of the game's windows is open on `f` besides the bag `v`: a close
/// button anywhere that is not the bag's own (BagLayout::close_dx, close_dy).
/// Kho Cash, a shop, a storage or a trade window open beside the bag may take
/// whatever is right-clicked in it, with no question asked -- and on the live
/// client Kho Cash was seen open beside Tiêu Dùng with every other check a use
/// sweep makes passing. `at` gets where the first such button is. True as
/// well without every one of the pictures in `store`: what cannot be ruled
/// out is taken to be there. `also_own`, when given, is one more close button
/// that is no other window's: the monster book's own X where find_book put
/// it, for the moment its question is up, which the right-click opened with
/// it.
bool other_window_up(const Frame& f, const TemplateStore& store, const BagView& v, const BagLayout& l,
                     ScreenRect* at = nullptr, const ScreenRect* also_own = nullptr);

/// Yêu Quái Bách Khoa, the monster book, as the live client draws it: the
/// window a fragment's right-click opens together with its question, and
/// leaves open once the question is answered, Xác Nhận or Hủy Bỏ.
///
/// Found by its first tab, "Yêu Quái Bách Khoa", with the rounded corner of
/// the window's frame round it -- drawn the same on every page of the book --
/// and its X, the client's own drawing of one, where it is from the tab on
/// every frame of the live client that shows the book. Both are cut from the
/// live client (live\shots\fragq_A_085): the tab 50x45 from 306,156, inside
/// the frame's outer line, and the X 18x18 from 807,160, drawn only where
/// GoC's inven/1/monster-close is drawn, so whatever page is behind it does
/// not count. On the 1,870 saved frames, the tab scores 1.00 on all 299 that
/// show the book and 0.58 at most anywhere on the rest; the X 1.00 where the
/// tab says, 0.94 with the pointer over it, and nothing like it while the
/// mouse button is held down on it, which draws it dark.
inline const char* const kBookTitle = "grabbed/monster_book_title";
inline const char* const kBookClose = "grabbed/monster_book_close";

/// The book's X from the top-left of its tab, and its whole window round the
/// tab: measured on the live client, the window's outer edge included.
constexpr int kBookXDx = 501, kBookXDy = 4;
constexpr ScreenRect kBookWindow{ -3, -3, 534, 414 };

/// The book on one frame.
struct Book {
    bool       found = false;       ///< its tab, at 0.90
    float      score = 0.0f;        ///< how alike the tab is
    ScreenRect title;               ///< the tab as found
    ScreenRect window;              ///< the whole window, from the tab
    /// Its X where the tab says, give or take three pixels, at 0.85: what a
    /// click closes the book with.
    bool       has_x = false;
    float      x_score = -1.0f;
    ScreenRect x;
};

/// Both of the book's pictures are in `store`.
bool can_see_book(const TemplateStore& store);

/// The book on `f`, by its tab -- anywhere, or with `window` only where a
/// book with that window would have its tab, give or take a few pixels: a
/// book looked at again where it was a moment ago -- and its X where the tab
/// says.
Book find_book(const Frame& f, const TemplateStore& store, const ScreenRect* window = nullptr);

} // namespace bag

/// One pass over the bag: open it, find what is on the list, get rid of it,
/// close it again.
///
/// Every step is checked against the screen before the next: the bag has to be
/// found before anything is touched, a dialog has to be the bin's own -- by
/// its words -- before its button is pressed, the number the game puts in a
/// quantity box has to read as the stack's count, look after look, before it
/// is confirmed -- nothing is ever typed there -- and an item has to be gone
/// before it counts. Anything unexpected and it backs out, answering Hủy Bỏ on
/// the bin's dialog so the item goes back to the bag, rather than pressing on:
/// this is the one feature that destroys things.
///
/// On Tiêu Dùng the items on the Dùng list -- fragments -- are registered in
/// Yêu Quái Bách Khoa first, before anything there is deleted, as GoC does it,
/// when [Trash] use_fragments is on: a right-click on the slot, and Xác Nhận on
/// the question that brings up only when its words are the registration
/// question's and nothing else is up; any other dialog gets Hủy Bỏ, a message
/// with only Xác Nhận stops the pass with nothing pressed, and nothing is ever
/// typed. A registration cannot be undone either, so it is held to the same
/// rules as a deletion. It counts only when the slot is seen to come out of it
/// as a registration does -- empty, or one less in place -- and anything else
/// stops the pass; what is left of a stack to be deleted after is deleted from
/// that very slot and from no other. The right-click opens the monster book
/// with its question, and the book stays open whatever the answer: it is
/// closed with its own X, and seen closed, before anything else is
/// right-clicked -- or the pass stops there.
class BagCleaner {
public:
    using Grab = std::function<bool(Frame&)>;
    using Report = std::function<void(const std::string&)>;

    BagCleaner(InputManager& input, const TemplateStore& store, Grab grab);

    void set_settings(const CleanSettings& s) { settings_ = s; }
    void set_trash(const TrashList* t) { trash_ = t; }
    void set_counts(const CountReader* c) { counts_ = c; }

    /// Where to keep a picture of the screen whenever a step goes wrong, so
    /// the next attempt can be fixed from what happened rather than guessed --
    /// the newest few dozen -- and a probe's frames, apart from those. Empty
    /// keeps none.
    void set_debug_dir(const std::string& dir) { debug_dir_ = dir; }
    void on_report(Report r) { report_ = std::move(r); }

    /// Look only: open the bag and go through it as usual, say what is on the
    /// list, and throw nothing away.
    void set_dry_run(bool on) { dry_run_ = on; }

    /// Stop at the next pause, giving back whatever is in the bin first --
    /// with the game brought to the front for it, since a stop is mostly
    /// pressed in GoBot's window or a console. A cleaner that is cancelled
    /// stays cancelled: a stop that arrives before the pass gets going still
    /// stops it.
    void cancel() { cancelled_ = true; }

    /// Only for tests, which have no game window: do not ask whether the game
    /// is in front before using the mouse.
    void set_skip_focus_check(bool on) { skip_focus_check_ = on; }

    /// What the cleaner does to the game, one function each. The game's own
    /// input unless set; a test sets them to a pretend game, which is how the
    /// whole of an item's way into the bin is tried without one. Setting them
    /// also skips the check that the game is in front.
    ///
    /// Nothing is ever typed into the bin's quantity box, and it is never
    /// clicked: the number the game fills it with is the only one confirmed.
    /// So `key` is only ever the key that opens and closes the bag, pressed
    /// only on a look at the screen that shows no dialog up. `right_click` is
    /// only ever on a slot of Tiêu Dùng judged an item on the Dùng list, on a
    /// clean screen.
    struct Hands {
        std::function<bool(int, int)>                click;
        std::function<bool(int, int, int, int)>      drag;
        std::function<bool(std::uint32_t)>           key;
        std::function<bool(int, int)>                right_click;
    };
    void set_hands(Hands h) { hands_ = std::move(h); skip_focus_check_ = true; }

    /// The time as the cleaner reads it, and its waits: the steady clock and
    /// real sleeps unless set. Every decision it makes by how long something
    /// took goes through these -- how long the bin has shown an item with
    /// nothing up, how long a dialog has been watched, the box's blink. A test
    /// sets them to a clock of its own that moves only on a wait and on each
    /// look at its pretend game, so those decisions come out the same however
    /// busy the machine is.
    struct Clock {
        std::function<std::chrono::steady_clock::time_point()> now;
        std::function<void(int)>                                 sleep;     ///< milliseconds
    };
    void set_clock(Clock c) { clock_ = std::move(c); }

    struct Result {
        bool ran = false;           ///< the bag was found and searched
        int  matched = 0;           ///< listed items to throw away found, each slot once
        int  deleted = 0;
        int  dropped = 0;
        int  failed = 0;            ///< tried and not got rid of, or not registered
        int  doubtful = 0;          ///< looked like a listed item, not taken for one
        /// Slots of items on the Dùng list found and not registered this pass
        /// -- all of them on a look-only pass -- each slot once.
        int  use_matched = 0;
        int  used = 0;              ///< registered in Yêu Quái Bách Khoa, and seen to be
        /// Right-clicked, and what came of it could not be seen through: its
        /// question confirmed and the slot not seen empty or one less in place,
        /// or no question at all and the stack gone down anyway. Never counted
        /// as used, nothing of it deleted, and the pass stops at the first.
        int  use_unsure = 0;
        std::string trouble;        ///< why it stopped early, in Vietnamese

        /// An item went towards the bin and was never seen given back or
        /// gone: it may still be in the bin, or in a dialog whose box takes
        /// whatever is typed. Whoever ran the pass keeps everything that
        /// presses keys held off until bag::nothing_held says otherwise.
        bool stuck = false;

        /// Whether everything that presses keys stays held off after this
        /// pass, given whether it was before. Taken while an item may be
        /// out; and left as it was by a pass that never got to a clean
        /// screen -- a look-only one, or one that met a dialog it would not
        /// answer -- since that pass has seen nothing of whether an earlier
        /// item came back. Only a pass that ran, or bag::nothing_held on a
        /// fresh look, ends it.
        bool keeps_hold(bool held_before) const { return stuck || (held_before && !ran); }
    };
    Result run();

    /// What a probe of the bin saw: see probe.
    struct Probe {
        /// What came up: "none"; "ask", the bin's question; "quantity", its
        /// quantity dialog; or "foreign", a dialog with the two buttons whose
        /// words are neither.
        std::string dialog = "none";
        /// It came up as the item landed in the bin, before any click on the
        /// bin, which was then never clicked. Otherwise it came up once the
        /// bin was clicked.
        bool  at_drop = false;
        /// How alike its words were to the picture it was taken for -- for a
        /// foreign one, to the nearer of the two -- at best over the frames
        /// watched. 0 when nothing came up.
        float words = 0.0f;
        int   frames = 0;           ///< frames looked at, from the drag on
        bool  moved = false;        ///< the item left its slot for the bin
        bool  back = false;         ///< the item is in its slot again, the bin clean
        std::string trouble;        ///< why not, in Vietnamese
    };

    /// See what the bin asks for one item, and give the item back: nothing
    /// is deleted. For the live client's question for an item that does not
    /// stack, which no pass has seen yet.
    ///
    /// Opens the bag if it is shut, clicks tab `tab`, scrolls to the top and
    /// -- on a clean screen only -- drags the item at `row`, `col` of the page
    /// on show (all from 0) onto the bin. From then on it looks at every
    /// frame, keeps each in the debug folder as probe_<time>_NN.png and says
    /// what every dialog on it is. When a dialog comes up as the item lands,
    /// that is what the bin asks: it is watched for about a second and a
    /// half, and the bin is not clicked. Otherwise, once the bin has shown the
    /// item for a moment with nothing up, the bin is clicked and what that
    /// brings up is watched for as long. Then it answers Hủy Bỏ on whatever
    /// came up, the bin's or not -- Hủy Bỏ never destroys anything -- or, when
    /// nothing came up, gives the item back the way a pass does; and last it
    /// looks for the item in its slot again.
    ///
    /// Xác Nhận is never pressed here, and no key at all once the item has
    /// moved: the one key is the bag's, pressed only on a look with no
    /// dialog up.
    Probe probe(int tab, int row, int col);

private:
    /// Hủy Bỏ on every dialog with the two buttons, the bin's first, until
    /// none is left or `tries` run out -- whoever's they are, since Hủy Bỏ
    /// never destroys anything. What a probe answers with; true when none is
    /// left.
    bool cancel_all(int tries);

    bool grab_view(Frame& f, BagView& v);

    /// The screen is in a state to start on an item: the bag found, the can
    /// in the bin and no dialog with the two buttons anywhere.
    bool clean(const Frame& f, const BagView& v);

    /// Give back whatever the bin holds -- answer Hủy Bỏ on its dialog, or
    /// click the bin to bring the dialog up and then answer it -- and leave
    /// any other dialog alone. True when the screen is clean afterwards.
    /// Runs to the end even when cancelled: it is what a stop needs. Brings
    /// the game to the front first, and clicks wherever the game is on top.
    bool back_out();

    /// The mouse goes where it is aimed: the window under that point is the
    /// game's, and -- for everything but giving an item back -- the game is
    /// in front. Says why not when not.
    bool mouse_ok(int x, int y, bool need_front = true);

    /// The game to the front, unless it is there already or this is a test.
    void bring_forward();

    /// Put the item in slot `index` of `v` into the trash: onto the bin, which
    /// takes it and waits; then a click on the bin, which brings up the bin's
    /// dialog; then the answer. `count` is the stack count read off the slot.
    bool trash_one(const Frame& f, const BagView& v, int index, int count);

    /// How a right-click on an item on the Dùng list came out. Only Used,
    /// Nothing and Refused let the pass go on: anything else is a surprise,
    /// and after a surprise nothing more is right-clicked or deleted.
    enum class UseOutcome {
        Used,           ///< its question confirmed, and the slot seen empty, or one less in place
        NotTaken,       ///< its question confirmed, and the stack as it was: not registered
        Unsure,         ///< its question confirmed, or none came up, and the slot not seen either way
        UsedUnasked,    ///< no question, and its stack went down: taken -- by a shop, perhaps
        Nothing,        ///< no question, and the stack as it was: it cannot be used
        Refused,        ///< another dialog came up, answered Hủy Bỏ
        Notice,         ///< a message with only Xác Nhận came up: left for the player
        Lost,           ///< the bag went out of sight, a dialog would not close, a stop
        Untouched,      ///< nothing was pressed: the mouse could not reach the slot
    };
    struct UseResult {
        UseOutcome  outcome = UseOutcome::Untouched;
        std::string why;            ///< in Vietnamese, when it is not Used
        /// When Used with some of the stack still in the slot: how many, and
        /// the slot as it was seen then -- what deleting the rest goes by.
        int         left = 0;
        Frame       after;
    };

    /// Register the item in slot `index` of `v`: one right-click on it; then
    /// its question, Xác Nhận only when its words are the registration's, on a
    /// fresh look, with nothing else up -- no other dialog, and no window but
    /// the bag and the book the right-click opened; and whatever else comes up
    /// Hủy Bỏ. Only gone, or one less in the stack, in that very slot, counts.
    /// `count` is the stack count read off the slot. The book is left as it
    /// is: use_sweep shuts it.
    UseResult use_one(const Frame& f, const BagView& v, int index, int count);

    /// What slot `index` of the page `v` holds now, against `before`, its look
    /// when it held `count`: looked at twice, a moment apart, and only what
    /// both looks agree on -- a count misread once, or a slot caught being
    /// redrawn, is no answer. The page has to be the same by the scroll bar on
    /// both looks: a slot on a page that cannot be told is no slot to go by.
    enum class SlotNow {
        Gone,       ///< empty
        Less,       ///< the same drawing, one less than `count`
        Same,       ///< the same drawing, still `count`
        Asking,     ///< a dialog with two buttons is up, which nobody here asked for
        Unknown,    ///< anything else; `why` says what, in Vietnamese
    };
    SlotNow slot_now(const BagView& v, int index, const Frame& before, int count, Frame& look,
                     Frame& shot, std::string& why);

    /// A slot this pass registered from, with some of its stack still in it,
    /// by tab, row of the tab and column -- kept only where the scroll bar
    /// said which row: the list item it was taken for, how many were left and
    /// the slot as seen right then. What is left of the stack of an item to
    /// be used and then deleted is deleted from these slots and no others, and
    /// only while each still shows just that: another slot drawn the same is
    /// most likely another monster's piece, which no registration of this one
    /// says anything about.
    struct Registered {
        int   item = -1;
        int   left = 0;
        Frame look;
    };
    using Registrations = std::map<long long, Registered>;

    /// The use sweep of tab `tab` -- Tiêu Dùng -- before its delete sweep, as
    /// GoC orders its pass: every slot judged a fragment on the Dùng list, one
    /// at a time with a fresh look after each, at most max_uses_per_run.
    /// `used` gets the list items registered -- one slot of a drawing a pass,
    /// the others left for the next -- `no_use` those that came to nothing,
    /// and `registered` the slots registered from that still hold some.
    /// Nothing is right-clicked with another of the game's windows open
    /// (bag::other_window_up); and a dialog up after one of the sweep's own
    /// right-clicks -- its question come late -- is answered Hủy Bỏ, never
    /// Xác Nhận, and the pass stops. After a right-click the pass goes on
    /// from -- registered, answered Hủy Bỏ, or nothing come of it -- the book
    /// it opened is shut (shut_book) before the next one, or the pass stops.
    void use_sweep(int tab, BagView& v, Result& res, bool& stop, std::set<int>& used,
                   std::set<int>& no_use, int& uses, Registrations& registered);

    /// Close Yêu Quái Bách Khoa when it is open: one click on its own X, on a
    /// look with no dialog up, and then looks until it is seen gone. True
    /// when it is not open, or seen gone after the click; false, with `why` in
    /// Vietnamese and `shot` the last look, when it is open and was not seen
    /// to close -- its X not seen, the mouse not to be had there, a dialog up,
    /// or the click did nothing. ESC is never pressed for it: with the bag
    /// open as well, which of the two an ESC closes is the game's to choose.
    bool shut_book(std::string& why, Frame& shot);

    /// Answer the bin's dialog that is up -- the question, or the quantity,
    /// confirmed only when the number the game put in the box is the stack's
    /// count -- and then whatever the bin asks next, up to one more. kDone
    /// when the last one has closed, kFailed on anything else, and then
    /// trash_one backs out.
    enum { kFailed = 0, kDone = 1 };
    int  answer(const bag::Dialog& first, int count);

    /// What the box of the bin's quantity dialog `d` holds, looked at again
    /// and again for a whole blink of its caret: the number every look that
    /// read one agrees on, two looks at least. kUnreadable when fewer read
    /// one, kBoxMixed when two read different numbers, kBoxGone when the
    /// dialog was no longer the bin's, where it was. `last` gets the last
    /// frame looked at.
    static constexpr int kBoxMixed = -2;
    static constexpr int kBoxGone = -3;
    int  box_holds(const bag::Dialog& d, Frame& last);

    /// A dialog of the bin's on screen within `ms`, or one with kind -1 and
    /// an empty pair. `foreign` is set when another dialog came up instead,
    /// and `last` gets the last frame looked at, to keep when that is wrong.
    bag::Dialog wait_for_dialog(int ms, bool& foreign, Frame* last = nullptr);

    void keep(const Frame& f, const char* what);
    void scroll_to_top(BagView& v);
    void say(const std::string& line);
    bool wait_ms(int ms);           ///< false when cancelled
    void pause_ms(int ms);          ///< whether cancelled or not
    // Through the clock when it is set, else the steady clock and a sleep.
    std::chrono::steady_clock::time_point now() const;
    void sleep(int ms);

    // Through the hands when they are set, else the game's input.
    bool click(int x, int y);
    bool drag(int fx, int fy, int tx, int ty);
    bool press(std::uint32_t vk);
    bool right_click(int x, int y);

    InputManager&        input_;
    const TemplateStore& store_;
    Grab                 grab_;
    CleanSettings        settings_;
    const TrashList*     trash_ = nullptr;
    const CountReader*   counts_ = nullptr;
    std::string          debug_dir_;
    Report               report_;
    bool                 dry_run_ = false;
    bool                 skip_focus_check_ = false;
    Hands                hands_;
    Clock                clock_;
    std::string          stuck_;        ///< why the last back_out left the screen unclean
    /// An item has gone towards the bin and the screen has not been seen
    /// clean since: see Result::stuck.
    bool                 item_out_ = false;
    /// A right-click of this pass brought nothing up while it was watched:
    /// its question may come up later still, and one up when the pass ends is
    /// taken for it -- see run().
    bool                 late_ask_ = false;
    std::atomic<bool>    cancelled_{false};
};

} // namespace gobot
