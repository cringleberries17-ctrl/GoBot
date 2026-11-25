#pragma once

#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/react_wait.hpp"
#include "gobot/template_matcher.hpp"

#include <algorithm>
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
/// Khác, where the Người Giữ Sân item lies: right-clicked there by the shop
/// run, and by nothing else.
constexpr int kOtherTab = 3;

/// The bag as found in one frame.
struct BagView {
    bool       found = false;
    float      score = 0.0f;
    ScreenRect strip;              ///< the tab strip
    ScreenRect trash;              ///< the trash bin, from the layout
    /// The can is showing. Not the same as nothing waiting: while the bin's
    /// own dialog is open the dialog holds the item and the can is back.
    bool       bin_empty = false;
    /// The bin lies inside the frame. A bag dragged low -- easy in a small game
    /// window -- puts it out of sight: no can is seen there, and that says
    /// nothing of the bin holding anything, nor can GoBot click it.
    bool       bin_on_frame = true;

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
    /// measured on the live client and on its own picture of the strip; in
    /// the band under the labels 63 to 80 against 110 to 131. -1 when no tab
    /// stands out like that -- by the band, or where it says nothing by the
    /// inside -- or the two name different tabs. Nothing is right-clicked
    /// unless this says Tiêu Dùng: on another tab a right-click equips what
    /// it lands on.
    int selected_tab = -1;
    /// Each tab's inside as selected_tab weighs it, its mean grey, from 0; -1
    /// for a tab not read. For the log when the strip reads no tab chosen, or
    /// not the one wanted: the live client's Tiêu Dùng chosen reads 105 against
    /// 123 for Khác next to it, 18 where kChosenTabDarker asks for 10.
    int tab_mean[kBagTabs] = { -1, -1, -1, -1, -1 };
    /// The same for the band under the labels, the strip's bottom rows
    /// (2026.11.04d): the shop's tab on 2026-11-04, Tiêu Dùng chosen and lit
    /// by the game's red glow, read 127 128 129 123 125 inside -- no tab -- and
    /// 121 121 68 122 122 here.
    int tab_band[kBagTabs] = { -1, -1, -1, -1, -1 };
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

/// The corner a stack's count is read in, from a slot's top-left, as
/// CountReader::read reads it: the client prints the count from 2 across and
/// 3 down, each digit 6 on.
constexpr int kCountCornerW = 28, kCountCornerH = 16;

/// How bare_corner tells a charm's slot with nothing printed over its icon --
/// a stack of one, which the client prints no count on. Measured on the live
/// client's bag (2026-11-03 19:54) against all eight charm icons: each slot
/// is that colour's catalog icon pixel for pixel, a pixel right of the
/// middle -- fit 0.00, worst channel 0, the badge corner left out. Another
/// colour fits it at 46.4 at best, the right icon a place off at 45.6; the
/// same slots in 16-bit colour at 5.8 at worst, with noise of 12 at 9.1.
constexpr int   kBareSlack    = 3;      ///< where the icon may lie, either way, as judge searches
constexpr int   kBareFitLeast = 200;    ///< the icon's pixels outside the count corner, at least
constexpr float kBareFitMean  = 12.0f;  ///< their mean largest-channel difference, at most
/// In the count corner: an icon pixel this near the frame's, each channel;
/// where the icon is see-through, the ground no brighter than this grey --
/// is_empty's 70. A count printed there puts strokes of 187 and more over
/// it, and an outline of 136 and less. Every number from 2 to 99, and 100,
/// 101, 110, 111, 199, 999, 1000, 1111 and 9999, drawn over the seven live
/// stacks of one, a pixel off each way, in white, dim or a grey read takes
/// for no white, breaks these in 11 pixels at least (33 with the client's
/// own outline); 2 to 99 over 8871081, in grey, red or yellow, 12; the live
/// x62 to x64 over 8871081, 81 to 88.
constexpr int   kBareOff      = 24;
constexpr int   kBareGround   = 70;

/// Where a catalog icon fits a slot best: the top-left of its own pixels in
/// the slot, within kBareSlack of the middle, and how well -- the mean of the
/// largest of the three channels' differences over its opaque pixels inside
/// the slot's edge, the count corner and the badge corner left out.
struct IconPlace {
    bool  found = false;        ///< kBareFitLeast pixels compared somewhere
    int   x = 0, y = 0;
    float mean = 255.0f;
};
IconPlace place_icon(const Frame& f, const ScreenRect& slot, const Frame& icon, const BagLayout& l);

/// The slot shows `icon`, a catalog icon with its own transparency, outside
/// its count corner: place_icon found it, at kBareFitMean or better. `fit`
/// gets the mean, 255 when it was not found.
bool icon_fits(const Frame& f, const ScreenRect& slot, const Frame& icon, const BagLayout& l,
               float* fit = nullptr);

/// The slot's count corner is `icon` with nothing printed over it: the icon
/// fits (icon_fits), and every pixel of the corner inside the edge is the
/// icon's own within kBareOff, or -- where the icon is see-through -- dark
/// ground. What a charm's stack of one is told by: the client prints no
/// count on it, and the light rim of a card or a diamond is white in that
/// corner, so "no white there" does not tell it (CountReader::read). Not the
/// digits it goes by, so a count in any colour fails it as one in white does.
/// `unlike` gets the corner's pixels that fail -- -1 when the icon does not
/// fit -- and `fit` the icon's fit.
bool bare_corner(const Frame& f, const ScreenRect& slot, const Frame& icon, const BagLayout& l,
                 int* unlike = nullptr, float* fit = nullptr);

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
    ///
    /// Read the way the client prints it: each digit in a cell 5 pixels wide
    /// and 8 high, the next cell 6 pixels on -- after a 1 too, which is 3
    /// pixels of ink at the left of its cell. In a cell only the digit's own
    /// strokes and the ring of pixels round them are compared; the rest of
    /// the cell is the item's icon showing through, white or not. A 1 judged
    /// over its 3-pixel box alone, as it was, lost to three white pixels of an
    /// icon's rim under it: on the live hotbar 61 read as 6 over the boiled
    /// egg, slot after slot.
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

    /// A charm's stack count, for the charm sweep and for its look at the
    /// slot after the use (BagCleaner::slot_now), in the same corner as read.
    /// With `icon` -- the catalog icon of the slot's colour, its own
    /// transparency, corner and all: the number printed there, from 2 up; 1
    /// only when no number reads and bag::bare_corner finds the corner is
    /// that icon with nothing printed over it -- the client prints no count
    /// on a stack of one, and a charm card's light rim is white in that
    /// corner, which read takes for a count it cannot read (on the live
    /// client, 2026-11-03 19:54, all seven charms of one); kUnreadable
    /// otherwise, no white in the corner included: a count drawn in another
    /// colour is still a count. Without one -- `icon` null or empty -- what
    /// read says, but never a lone 1 read in digits: the client prints counts
    /// from 2 up. `bare` says the 1 came from bare_corner.
    int read_charm(const Frame& f, const ScreenRect& slot, const Frame* icon, const BagLayout& l,
                   bool* bare = nullptr) const;

    /// Give a digit its shape directly. For tests.
    void set_digit(int d, int w, int h, const std::vector<std::uint8_t>& on);

    /// The client's digit cell: 5 pixels wide, a 1 included, and the next
    /// digit kCellW + 1 pixels on.
    static constexpr int kCellW = 5;

private:
    /// Digits in `area`, left to right, the line starting no lower than
    /// `max_top`: the number, or -1 when none reads. `white` gets how many
    /// pixels there were white enough to be part of a digit, and `stray` how
    /// many of those lie outside every digit read. With `caret`, the caret's
    /// yellow pixels are unknown rather than ground. With `cells`, digits are
    /// read in the client's fixed cells, by their strokes and the ring round
    /// them (read); without, each over its own box, a column at a time
    /// (read_field, the quantity box and its caret).
    int scan(const Frame& f, const ScreenRect& area, int max_top, bool caret, bool cells, int& white,
             int& stray) const;

    struct Glyph {
        int w = 0, h = 0;
        std::vector<std::uint8_t> on;   ///< 1 where the digit is drawn
        int lit = 0;
        /// Over the digit's cell, max(w, kCellW) wide and h high: 1 where a
        /// pixel the digit leaves unlit touches one it lights, side or
        /// corner -- the outline the client draws round its digits.
        std::vector<std::uint8_t> ring;
        int ring_n = 0;
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

    /// [Trash] use_charms: whether the charms (Bùa) on the Dùng list are used:
    /// a right-click on the slot in Tiêu Dùng, and Xác Nhận on the question
    /// that brings up. One charm of each kind lasts an hour and costs one of
    /// the stack, so each is used once and not again until
    /// charm_every_minutes are over. Off unless the file says so, and it
    /// ships off. Charms are used by a pass of their own (BagCleaner::Mode::
    /// CharmsOnly), quick and with nothing else pressed: never inside the slow
    /// pass that clears the bag.
    bool use_charms = false;
    /// [Trash] charm_every_minutes: how long a charm lasts, and so how soon
    /// after one is used another of the same kind may be. A charm used with
    /// no sign of whether it took waits as long: one used again within the
    /// hour adds to it, up to the game's 2 hours, and is spent early.
    int  charm_every_minutes = 60;
    static constexpr int kCharmEveryLeast = 5;
    static constexpr int kCharmEveryMost = 1440;
    /// [Trash] charm_wait_ms: how long a right-click on a charm is still
    /// watched for its question once the run has been stopped (F12, DỪNG, the
    /// guard's no), so that a question coming up late is not left open.
    /// Without a stop the right-click is given kReactMs, 30 s, for its
    /// question (2026.11.15d, the owner: "sẽ có thêm 30 giây chờ phản ứng"),
    /// whatever this says: it was how long the question was waited for at
    /// all before.
    int  charm_wait_ms = 1000;
    static constexpr int kCharmWaitLeast = 300;
    static constexpr int kCharmWaitMost = 3000;
    /// [Trash] charm_frame_score: how much of the registration question's box,
    /// under its words, a dialog has to show to be taken for the question of a
    /// charm (bag::charm_question). The questions GoBot knows score up to 0.89
    /// against it; the game's own box in the same frame near 1. Lower it only
    /// with the frames the run saves in trash_debug (charm_question).
    static constexpr float kCharmFrameDefault = 0.92f;
    float charm_frame_score = kCharmFrameDefault;
    static constexpr float kCharmFrameLeast = 0.85f;
    static constexpr float kCharmFrameMost = 0.99f;

    /// [Trash] buy_potions: whether potions are bought every
    /// buy_every_minutes from the shop the Người Giữ Sân item in Khác opens: a
    /// right-click on the item, a double-click on the potion, 100 into the
    /// box, Xác Nhận (BagCleaner::Mode::ShopBuy) -- buy_hp_slots purchases of
    /// Thịt bò khô and buy_mp_slots of Rượu lá tre, taking turns. Nothing in
    /// the bag is counted (the owner, 2026-11-09): the game's message that the
    /// bag is full ends a round. Off unless the file says so, and it ships
    /// off: it spends the character's money, and types into the game.
    bool buy_potions = false;
    int  buy_every_minutes = 60;
    static constexpr int kBuyEveryLeast = 5;
    static constexpr int kBuyEveryMost = 1440;
    /// [Trash] buy_after_trash (2026.11.13b, users' feedback via the owner:
    /// "Mua ngay sau khi dọn rác ... không cần thiết định thời gian mua HP/MP
    /// nữa"): with Tự dọn rác on too, a plan is bought right after each timed
    /// pass, riding it under the same hold, and buy_every_minutes begins none
    /// -- what is still owed of a plan is bought on as before. With Tự dọn
    /// rác off it changes nothing: the clock buys. Off unless the file says
    /// so (bag::buys_after_pass).
    bool buy_after_trash = false;
    /// [Trash] buy_hp_slots, buy_mp_slots: how many purchases of 100 of Thịt
    /// bò khô (HP) and of Rượu lá tre (MP) each time -- a slot of the bag each
    /// -- 0 leaving that potion alone. No cap of GoBot's own since 2026.11.10
    /// (the owner: "Không giới hạn"; 20 in 2026.11.09): the game's message
    /// that the bag is full ends a round, and a hold buys
    /// BagCleaner::kShopMaxBuys at most ("Mua ngay") or kShopBuysPerHold (on
    /// the clock), the rest a part's gap later. kBuySlotsPerRunMost bounds
    /// the number, no more. A file from before 2026.11.09 has neither: its
    /// buy_hp_to and buy_mp_to, the slots it bought up to, stand for them
    /// until the card saves the new keys -- kBuySlotsFromOldMost at most, as
    /// 2026.11.09 read them, so that only a number set on the card buys more
    /// -- and buy_*_below is passed over. Neither there: kBuySlotsDefault.
    static constexpr int kBuySlotsDefault = 3;
    static constexpr int kBuySlotsPerRunMost = 999;
    static constexpr int kBuySlotsFromOldMost = 20;
    int  buy_hp_slots = kBuySlotsDefault;
    int  buy_mp_slots = kBuySlotsDefault;
    /// [Trash] buy_pet, buy_pet_slots (2026.11.24, the owner: "Khi ra tới bản
    /// đồ TổCổLong thì cần mua lại HP cho pet. Cái này ở trong tab Linh Thú
    /// của người giữ sân và là item "Thịtbò" => cần có lựa chọn để mua và mua
    /// bao nhiêu slot tương tự như mua HP MP"): whether the pet's HP food is
    /// bought each time the character comes into TổCổLong (t60_s19, Cổ Long's
    /// lobby, where a run leaves you) -- one round of buy_pet_slots purchases
    /// of 100 of Thịt bò, the pet's, from the Linh Thú tab of the same shop,
    /// a slot of the bag each, nothing counted, the game's message that its bag
    /// is full the end of it (PetRestock, GuiApp::trash_loop). Never the
    /// character's Thịt bò khô, which Dược Phẩm sells. Off unless the file says
    /// so, and it ships off: it spends the character's money. The slots 0 to
    /// kBuySlotsPerRunMost, kBuyPetSlotsDefault when the file has none.
    bool buy_pet = false;
    static constexpr int kBuyPetSlotsDefault = 1;
    int  buy_pet_slots = kBuyPetSlotsDefault;
    /// The pet's food as the shop's list draws its name -- with no spaces, as
    /// it draws "Thịtbòkhô" and "Rượulátre" -- and as the card and the log
    /// write it.
    static constexpr const char* kBuyPetName = "Thịtbò";
    static constexpr const char* kBuyPetShown = "Thịt bò";
    /// How many of a potion one purchase asks for: always this, typed into the
    /// quantity box and read back before Xác Nhận.
    static constexpr int kBuyQuantity = 100;
    /// The catalog id of the Người Giữ Sân item of Khác: the portrait the
    /// other helper names (other/8950604). Another id here if the live icon is
    /// another.
    std::string buy_opener = "other/8950604";
    /// The potions bought, by catalog id: what the game's shop sells under the
    /// names Thịtbòkhô and Rượulátre, matched pixel for pixel by the icons in
    /// its first row (grabbed/ngs_hpmp).
    static constexpr const char* kBuyHpId = "spend/hp/8820131";
    static constexpr const char* kBuyMpId = "spend/mana/8810111";
    /// [Trash] buy_frame_score: how much of the sell question's box the shop's
    /// quantity box has to show to be taken for one of the game's own, with
    /// its own words (bag::shop_question). Lower it only with the frames the
    /// run keeps in trash_debug (shop_question).
    static constexpr float kBuyFrameDefault = 0.85f;
    float buy_frame_score = kBuyFrameDefault;
    static constexpr float kBuyFrameLeast = 0.70f;
    static constexpr float kBuyFrameMost = 0.99f;

    /// A bound on one pass, so a mistake costs a handful of items at most.
    int max_per_run = 40;
    /// The same for registering, [Trash] max_uses_per_run: a registration
    /// cannot be undone either.
    int max_uses_per_run = 20;

    /// Click the item and then the target, instead of dragging it there.
    bool click_to_move = false;

    /// [Trash] press_ms: how fast GoBot presses in its bag jobs (Dọn rác, Dùng
    /// bùa, Tự mua bình) -- the gap after a key typed into the shop's box, and
    /// the pauses of its clicks and drags, scaled from what they are at 80.
    /// Never the waits for the game to answer, the read-backs, nor how long a
    /// key is held. The steps the screen offers: 60, 80, 100, 150.
    static constexpr int kPressDefaultMs = 80;
    static constexpr int kPressLeastMs = 60;
    static constexpr int kPressMostMs = 150;
    int press_ms = kPressDefaultMs;

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

/// What a right-click on a Tiêu Dùng item is allowed to be, as a pass decides
/// it for an item on the Dùng list. Anything else is never right-clicked: a
/// right-click on a potion drinks it, on a box opens it, on a scroll reads it.
enum class UseKind {
    None,
    Fragment,   ///< registered in Yêu Quái Bách Khoa: use_sweep, [Trash] use_fragments
    Charm,      ///< a Bùa, used for an hour: charm_sweep, [Trash] use_charms
};

namespace bag {
/// By the catalog id: spend/fragment/... a fragment, spend/buff/... a charm,
/// everything else -- potions, mana, craft items, seals -- none.
UseKind use_kind_of(const std::string& id);
} // namespace bag

/// When each charm may be used next, by catalog id, in wall-clock seconds, kept
/// in a small file: a charm used an hour ago and a GoBot started again -- or a
/// new version unzipped into a new folder -- must not use another while the
/// first still works. Each charm costs one of the stack, and a charm used again
/// while the first works only adds to its time, up to the game's 2 hours: a
/// charm spent early.
class CharmClock {
public:
    /// How a try came out.
    enum class How {
        Used,       ///< seen taken: the stack one less, or gone
        MaybeUsed,  ///< right-clicked and the slot could not be told afterwards: taken for used
        NotUsed,    ///< nothing came of it: refused, or the stack as it was
    };

    /// %LOCALAPPDATA%\GoBot\charm_clock.txt, as the webhook and the lines
    /// Tự chat said are kept; beside the program when that folder is not known.
    static std::string default_path();

    explicit CharmClock(std::string path = default_path()) : path_(std::move(path)) {}

    /// Read the file. False when it is not there or cannot be read: nothing is
    /// known then, and every charm is due.
    bool load();
    /// Whether `id` may be used at `now_s`: it never was, or its time has come.
    bool due(const std::string& id, std::int64_t now_s) const;
    /// When `id` is next due; 0 for one never used.
    std::int64_t next(const std::string& id) const;
    /// Set and written. False when the file cannot be written; the time is kept
    /// in memory all the same.
    bool set_next(const std::string& id, std::int64_t at_s);
    /// Everything known, written as set_next writes: False when it cannot be.
    /// A run asks it before it right-clicks anything -- a charm used and not
    /// written down is a charm used again after a restart.
    bool save();

    /// When the next try is due after one that came out `how` at `now_s`: a
    /// charm used, or perhaps used, after `every_minutes`; one not used after
    /// kRetryMinutes.
    static std::int64_t next_after(How how, int every_minutes, std::int64_t now_s);
    static constexpr int kRetryMinutes = 10;

    const std::map<std::string, std::int64_t>& all() const { return next_; }

private:
    std::string path_;
    std::map<std::string, std::int64_t> next_;
};

/// When the potion shop's next plan is due (2026.11.09), kept as the charms'
/// times are -- a CharmClock of its own, one id -- in shop_clock.txt beside
/// charm_clock.txt: nothing in the bag is counted any more, so a GoBot started
/// again must not buy a plan early. Read and written by the trash worker only.
std::string shop_clock_path();
inline const char* const kShopClockId = "shop";

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
        /// A charm's catalog icon, its own transparency, corner and all: what
        /// its slot shows with nothing printed over it (bag::bare_corner).
        /// Empty for anything else.
        Frame       bare;
        float       min_score = 0.0f;   ///< 0: the caller's

        /// Catalog ids drawn like what was chosen: its own and the same
        /// drawing in other colours. By the player's rule they are one thing,
        /// so none of them is a rival to this item -- the drawing alone does
        /// not tell them apart. For a charm on the Dùng list they are still
        /// not one thing: each colour is another effect, and judge tells
        /// them apart by their colour (colour_of).
        std::vector<std::string> kin;

        /// For an icon on the list only because it is drawn like one that was
        /// chosen: the one that was. Empty for what was chosen itself, and
        /// always for a charm on the Dùng list: no other colour comes along
        /// with one, its colour being another charm.
        std::string via;

        /// The catalog colour this item is: its own id for a catalog icon;
        /// for a charm cut from the bag on the Dùng list, the icon among its
        /// kin its colour was found to be when the list was built
        /// (add_rivals), or empty when that was not sure -- and then it is
        /// never right-clicked. Empty for anything else cut from the bag.
        std::string colour_of;

        /// A Use item that is on the delete list as well: what is left of a
        /// stack a pass has just registered is deleted in that pass -- a piece
        /// is registered once, and its copies are no more use. That stack
        /// only, from the very slot it was registered from. A fragment's icon
        /// says only which of a monster's nine pieces it is, so another slot
        /// drawn the same is most likely another monster's piece: it is never
        /// deleted before a registration of its own.
        bool then_delete = false;

        /// What a right-click on it may be, and so whether a pass may
        /// right-click it when it is on the Dùng list: one of the client's
        /// fragment icons or charm icons, or a picture cut from the bag that
        /// add_rivals found drawn like icons of one kind and like nothing else
        /// in the catalog. A right-click on anything else uses it or equips
        /// it there and then -- a potion drunk, a box opened, a scroll read --
        /// so a picture of that on the Dùng list is only ever said, never
        /// right-clicked. Icons of both kinds drawn alike give none.
        UseKind use_kind() const;

        /// A fragment: use_kind() is Fragment.
        bool fragment() const;
        /// A charm (Bùa): use_kind() is Charm.
        bool charm() const;
        /// What a charm's clock (CharmClock) goes by: the icon that was chosen,
        /// each colour its own -- no other colour comes along with a charm on
        /// the Dùng list (via) -- or the picture's file for one cut from the
        /// bag. A picture is named by its file name alone: the folder it lies
        /// in changes with every version unzipped, the charm does not.
        std::string clock_id() const {
            if (!via.empty()) return via;
            if (!id.empty()) return id;
            const std::size_t slash = file.find_last_of("\\/");
            return slash == std::string::npos ? file : file.substr(slash + 1);
        }
    };
    std::vector<Item> items;

    /// Catalog icons not on the list -- everything a listed item has to look
    /// more like the slot than, unless it is drawn the same. Filled by
    /// add_rivals.
    std::vector<Item> rivals;

    /// Add catalog icons by id, and every icon drawn the same, as items that
    /// say which one brought them -- but for a charm on the Dùng list, which
    /// brings no other colour: each colour is a charm of its own, and one not
    /// chosen is left alone. The count corner and the badge corner of each
    /// are left out, as for a picture cut from the bag; ids the catalog does
    /// not have are skipped.
    void add_catalog(const ItemCatalog& catalog, const std::vector<std::string>& ids,
                     Action a, float min_score);

    /// Every catalog icon that is not on the list becomes a rival, and each
    /// picture cut from the bag learns which icons are drawn like it: the
    /// icons it is one drawing with, the badge corner compared too, as
    /// same_drawing compares two icons -- and those icons' own colours. A
    /// charm cut from the bag for the Dùng list then learns which of those
    /// colours it is (colour_of), against their icons, as judge tells a
    /// slot's. Call after everything is added.
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
        /// differently, looks as much like it (Rival). For saying so on a
        /// look-only pass. Or, for a charm on the Dùng list, drawn like it and
        /// of a colour that is not sure (Colour): never right-clicked, and
        /// not a doubt between two drawings -- a pass does not count it with
        /// those (Result::doubtful), nor do the shop's counts.
        enum class Doubt { None, Rival, Colour };
        Doubt       doubt = Doubt::None;
        /// The listed item it looks like -- for a doubt, and for a charm of a
        /// colour told apart from the one listed (colour_id, Doubt::None).
        int         like = -1;
        float       like_score = 0.0f;
        std::string rival;          ///< the unlisted look-alike
        float       rival_score = 0.0f;

        /// For a slot drawn like a charm on the Dùng list, its colour, for
        /// the log: the catalog colour of that drawing it looks most like,
        /// how alike, the next and how alike, and how far its mean colour is
        /// from the first's, the largest of the three channels' differences
        /// (-1: not measured). Set when the colour was not sure (Colour), and
        /// as well when it was -- taken, or left alone as another colour
        /// (item -1, Doubt::None).
        std::string colour_id;
        float       colour = 0.0f;
        std::string colour_next_id;
        float       colour_next = 0.0f;
        int         colour_mean_off = -1;
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
    ///
    /// A charm on the Dùng list is told by its colour as well: the client's
    /// charms are one drawing in several colours, each another effect, and in
    /// grey they are all alike (0.93 to 0.98 on the live client's slot of
    /// 8871081). So a slot it takes is then looked at in colour, against the
    /// catalog icons of that drawing, where its drawing was found: a colour
    /// that is not sure is Doubt::Colour; one that is another listed item's
    /// whose own drawing matched is that item's -- another charm chosen, or
    /// one of exactly that id on the delete or drop list; and any other is
    /// left alone (item -1, Doubt::None, colour_id saying which).
    Verdict judge(const Frame& f, const ScreenRect& slot, float min_score) const;

    /// The items on the delete or drop list drawn like an item on the Dùng
    /// list, by the catalog icons each was found drawn like (kin), as indexes
    /// into `items`: a slot holding one is the Dùng item's (judge), so it is
    /// registered rather than deleted. For the tab to say so. Not a charm's
    /// icon chosen for itself: a charm on the Dùng list is told by its colour,
    /// and a slot of that icon's colour is deleted. After add_rivals, which is
    /// what tells a picture cut from the bag its kin.
    std::vector<std::size_t> yielding_to_use() const;

    /// judge, reduced to the item, or -1. `score` gets how alike it is -- or,
    /// for a slot that is refused, how alike it was to what it resembles.
    int match(const Frame& f, const ScreenRect& slot, float min_score,
              float* score = nullptr) const;
};

namespace bag {

/// The tabs a delete pass looks through (2026.11.10): those ticked under
/// "Tìm trong:" -- or, when every item on the list is a catalog icon under
/// spend/, Tiêu Dùng alone, and only if it is ticked: potions, food,
/// fragments and charms lie nowhere else, and the other tabs' pages were
/// most of a pass's time. A picture cut from the bag, or an icon of any
/// other group -- one brought along for being drawn like a chosen one (via)
/// among them -- and every ticked tab is looked through, as before.
struct PassTabs {
    bool tabs[kBagTabs] = {};
    int  count = 0;             ///< tabs looked through
    bool ticked_any = false;    ///< a tab is ticked at all
    bool spend_only = false;    ///< every item a spend/ icon: Tiêu Dùng alone
    /// Why there is nothing to look through, in Vietnamese, as the card and
    /// the worker say it; empty when there is.
    const char* none_said() const;
};
PassTabs pass_tabs(const CleanSettings& s, const TrashList& list);
/// No tab ticked; and every item a spend/ icon with Tiêu Dùng not ticked.
inline constexpr const char* kNoTabVi = "Chưa chọn ngăn nào ở 'Tìm trong:', nên không xoá được gì";
inline constexpr const char* kNoUseTabVi =
    "Mọi món trong danh sách đều nằm ở ngăn Tiêu Dùng, mà ngăn đó chưa được chọn ở 'Tìm trong:', nên không xoá "
    "được gì";

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

/// How many frames find_dialogs and shop_full_up have searched since GoBot
/// started, never reset (2026.11.10): for the tests, which count what a look
/// costs -- a frame is searched for dialogs once.
struct SearchCounts {
    std::uint64_t dialogs = 0;
    std::uint64_t full = 0;
};
SearchCounts search_counts();

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

/// The same for a charm (TrashList::Item::charm): drawn like the catalog's
/// charm icons, and like nothing else in it. What the bag picture's "Dùng" is
/// offered for on Tiêu Dùng, besides fragments.
bool charm_cut(const Frame& cut, const BagLayout& l, const ItemCatalog& catalog);

/// `d` is one of the questions GoBot has a picture of other than the
/// registration's and the bin's: the revive question, dropping and selling,
/// found by their words where those pictures put them for these buttons. A
/// charm's question is none of these.
bool other_question_up(const Frame& f, const TemplateStore& store, const Dialog& d);

/// Whether `d` is taken for the question a charm's right-click brings up: a
/// dialog with the two buttons that is not the bin's, not the fragment
/// registration's and not one of the other questions GoBot has a picture of
/// (other_question_up). The player's rule: a question of the game's own, drawn
/// like the registration's, asked of a right-click on a charm, is confirmed.
/// The rest of what makes a confirmation safe is not in the picture: the slot
/// right-clicked is a charm's, on Tiêu Dùng, with no other window of the
/// game's open, and the stack has to be seen one less afterwards.
bool charm_question(const Frame& f, const TemplateStore& store, const Dialog& d,
                    float frame_score = CleanSettings::kCharmFrameDefault);

/// Whether the pictures a charm pass needs are there: the two buttons, the
/// bin's two questions, and the revive, drop and sell questions -- everything
/// it must tell a question apart from; a question it has no picture of cannot
/// be told from a charm's -- the client's close buttons, to know of another
/// window open beside the bag; and the message's picture (grabbed/dialog_ok)
/// with the label of the pair's Xác Nhận found in it, to close the message a
/// charm's use brings (lone_ok).
bool can_check_charm(const TemplateStore& store);

/// A message with only Xác Nhận under it is up, and no dialog with two buttons:
/// what the client shows when it refuses something, by the whole of its
/// wordless picture at 0.75 -- 0.89 on the live charm message. For saying no
/// only: nothing is ever clicked by it (lone_ok finds a button to click).
bool notice_up(const Frame& f, const TemplateStore& store);

/// How lone_ok saw a message's button: where the label of its Xác Nhận is, and
/// how alike each piece is there. -1 for a piece not looked at.
struct LoneOk {
    ScreenRect label;
    float      label_score = -1.0f;
    float      edge_score = -1.0f;      ///< the frame right of the button and under it
    float      corner_score = -1.0f;    ///< the button with the frame's corner round it
};

/// The one Xác Nhận of a message, within `band` (where its label may be): the
/// label cut from the pair's picture (grabbed/confirm_button), the frame of the
/// box right of it and under it as grabbed/dialog_ok draws them, and no dialog
/// of two buttons up. Its words are not looked at. On the live client
/// (2026-11-02 and 11-03) a charm's message puts it where the question's Hủy
/// Bỏ was: the label 1.000 there against about 0.38 for that Hủy Bỏ, and the
/// frame right of it 1.000 against under 0.1 right of a question's Xác Nhận. A
/// button lit by the pointer over it is not taken for one. `at` gets the label;
/// `how`, when given, the scores at the best place for the label, found or not.
bool lone_ok(const Frame& f, const TemplateStore& store, const ScreenRect& band, ScreenRect* at = nullptr,
             LoneOk* how = nullptr);

/// Where the label of the message a charm question at `pair` brings may be:
/// lone_ok's band. Its button where the question's Hủy Bỏ was, give or take
/// three pixels across, and from 20 above to 40 below, for a box made taller
/// by a longer text. The question's own Xác Nhận lies 43 pixels left of it.
ScreenRect message_band(const ScreenRect& pair);

/// The box of the question at `pair` -- 410-614 x 341-429 on the live client,
/// its pair at 514,397 -- with 40 more below for a taller message: what the
/// pointer is kept off after a click there, so that no button of it is lit.
ScreenRect message_box(const ScreenRect& pair);

/// notice_up, or a lone Xác Nhận anywhere on the frame: for saying no only,
/// never for a click.
bool message_up(const Frame& f, const TemplateStore& store);

/// notice_up and lone_ok with find_dialogs' result on `f` already in hand
/// (`dialogs`, as it gave it: the shop's own Mua | Thoát among them): the same
/// answers, the frame searched for dialogs once (2026.11.10). Neither looks
/// for its picture with a dialog of two buttons in the list.
bool notice_up(const Frame& f, const TemplateStore& store, const std::vector<Dialog>& dialogs);
bool lone_ok(const Frame& f, const TemplateStore& store, const ScreenRect& band, const std::vector<Dialog>& dialogs);

// ---------------------------------------------------------------- the shop

/// The pictures of the shop the Người Giữ Sân item opens, cut from the live
/// client: the first row of its list, Rượu lá tre on the left and Thịt bò khô
/// on the right (294x21); its Mua | Thoát buttons (119x21); and the message
/// that the bag is full. And the sell question, the one picture of the game's
/// quantity box there is, which a purchase's own is taken to be drawn like.
inline const char* const kShopRow = "grabbed/ngs_hpmp";
inline const char* const kShopButtons = "grabbed/ngs_buy_button";
inline const char* const kShopFull = "grabbed/nguoi_giu_san_full_hpmp";
inline const char* const kSellQuestion = "grabbed/sell_confirm";

/// Where things are from the top-left of kShopRow, measured on the client's
/// shop sprite (inven/0/Dialog_Shop.spr: the row at 11,92, the buttons at
/// 197,391) and the live grab: the icon box of each cell, to click an item
/// by, and the buttons.
constexpr int kShopMpX = 17, kShopHpX = 169, kShopItemY = 12;
constexpr int kShopButtonsDx = 186, kShopButtonsDy = 299;
/// The shop's whole window from its buttons' top-left (Dialog_Shop.spr: the
/// buttons 197,391 into a window of about 330x420).
constexpr ScreenRect kShopWindowFromButtons{ -197, -391, 330, 420 };
constexpr int kShopMuaX = 27, kShopThoatX = 91, kShopButtonY = 10;
/// Where Xác Nhận is on the full-bag message, from its top-left.
constexpr int kShopFullOkX = 166, kShopFullOkY = 67;

/// The shop as one look finds it: the potions' row, and its buttons in that
/// row's column below it, no lower than kShopButtonsDy -- the two together,
/// never one alone. The row is the list's first in the client's sprite, but
/// the live shop has shown two other items above it, its buttons 257 under it.
struct Shop {
    bool       found = false;
    ScreenRect row;                 ///< kShopRow as found
    ScreenRect buttons;             ///< kShopButtons as found
    float      row_score = 0.0f;
    float      buttons_score = 0.0f;
    int mp_x() const { return row.x + kShopMpX; }
    int hp_x() const { return row.x + kShopHpX; }
    int item_y() const { return row.y + kShopItemY; }
    int mua_x() const { return buttons.x + kShopMuaX; }
    int thoat_x() const { return buttons.x + kShopThoatX; }
    int button_y() const { return buttons.y + kShopButtonY; }
};
Shop find_shop(const Frame& f, const TemplateStore& store);

/// The message that the bag has no room, on `f`: where it is, or not found.
bool shop_full_up(const Frame& f, const TemplateStore& store, ScreenRect* at = nullptr);

/// How near `f` comes to that message, found or not, for the log: the best
/// place for the whole picture anywhere, how alike it is there, and how alike
/// its line of words is at that place. -1 for what was not looked at. Where
/// the run meets another message, or none, it says whether the owner's client
/// draws the message a little apart from the picture.
struct ShopFullLook {
    ScreenRect at;
    float      whole = -1.0f;
    float      words = -1.0f;
};
ShopFullLook shop_full_look(const Frame& f, const TemplateStore& store);

/// Where the Xác Nhận label of the full-bag message is looked for, from the
/// message's top-left: its label at 148,58 in the picture, give or take six
/// pixels across and four up and down.
constexpr ScreenRect kShopFullOkBand{ 142, 54, 46, 21 };

/// Whether anything of a potion purchase may still be up on `f`: the shop, its
/// buttons alone, or either button of a question -- one find_dialogs does not
/// see with a button drawn pressed. True on no frame or a black one. What a
/// hold kept for a purchase is let go only without. A message alone -- the
/// bag full, or one GoBot does not know -- is none of it (2026.11.09): it takes
/// no key, and a message that would not close kept every key off all night.
bool shop_left_up(const Frame& f, const TemplateStore& store);

/// The order a round of the shop buys in: Thịt bò khô and Rượu lá tre taking
/// turns, HP first, then what is left of the longer -- `hp` and `mp` of them,
/// cut to `most`. The full-bag message is for the whole of Tiêu Dùng, so a
/// round that meets it after a few has some of each.
///
/// Pet (2026.11.24): Thịt bò, the pet's HP food, from the shop's Linh Thú tab
/// -- CleanSettings::buy_pet.
enum class ShopPotion { Hp, Mp, Pet };
std::vector<ShopPotion> shop_order(int hp, int mp, int most);
/// A round with the pet's food in it too (2026.11.24): its `pet` purchases
/// first, all on the Linh Thú tab, then shop_order's turns of `hp` and `mp` on
/// Dược Phẩm -- one switch of the tab each way at most -- cut to `most`.
std::vector<ShopPotion> shop_round_order(int pet, int hp, int mp, int most);

/// What is still to be bought of a plan -- the slots set, from the time it was
/// due -- and the rounds in a row that ended in trouble: counted for the words
/// (the card's "Mua bình bị lỗi N lần liền", the log), never to drop what is
/// owed (2026.11.24; `dropped` and kShopTroublesToDrop are gone, see
/// shop_owed_after).
struct ShopOwed {
    int hp = 0, mp = 0;
    int troubles = 0;
};
/// How a round of the shop ended, as whoever runs it sees it. Full: the game
/// said the bag is full. Message: another message came up instead of the
/// quantity box. Trouble: anything else that went wrong. Clean: no trouble.
/// Paused: the round did not get to buying for a reason of its own -- the
/// check-in board up, HP low, somebody at the mouse, a stop.
enum class ShopEnd { Clean, Full, Message, Trouble, Paused };
/// What is owed after a round that bought `bought_hp` and `bought_mp` and
/// made `unsure_hp` and `unsure_mp` purchases that may or may not have gone
/// through -- counted as bought: never bought twice -- and ended `end`:
/// nothing after a full bag or a message (the game said no: the plan ends);
/// after a trouble, the rest, however many troubles came in a row -- one more
/// counted; the troubles in a row reset by a clean round, a full bag or a
/// message, and left as they were by a paused one. Never under 0.
///
/// 2026.11.24 (the owner: "Kiểm tra các phần khác xem có phần nào không làm
/// được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác"; then "thử lại
/// luôn ... chứ không phải là dừng 90s"): a trouble never drops the plan. Until
/// then the kShopTroublesToDrop-th (2) in a row dropped what was left until the
/// next plan, an hour on as shipped -- the shop stopped for want of one round
/// that went. Now every round in trouble is tried again kShopTroubleAgainMs (the
/// owner's 30 s of 2026.11.15b) later, without limit, until one goes clean or
/// the game says the bag is full; what may have gone through is still counted
/// as bought, a retry never begins a plan of its own (shop_plan_begins), and
/// no digit goes through the key window for it.
ShopOwed shop_owed_after(const ShopOwed& before, int bought_hp, int bought_mp, int unsure_hp, int unsure_mp,
                         ShopEnd end);
/// A new plan when its time comes: the slots set, owing all of them, and no
/// trouble yet. Whatever an earlier plan still owed is dropped.
ShopOwed shop_new_plan(int hp_slots, int mp_slots);

/// When a timed job of the trash worker -- the Dọn rác pass, the potion shop's
/// look -- is first due once the worker counts (2026.11.12, the owner: "khi
/// bật thì bắt đầu đếm thời gian. Lúc nào tới thời gian thì mới bắt đầu thực
/// hiện"): a whole `every_minutes` from `now_ms`, the start -- BẮT ĐẦU, F10, the
/// job's box ticked on -- but for a clock the last worker left still counting
/// (`kept_ms` later than now; 0: none kept): a quick MACRO OFF and ON keeps
/// what is left of its wait, `least_ms` at the least. One that ran out while
/// nothing ran is not counting, and the job counts afresh. Times on now_ms()'s
/// clock. Before 2026.11.12 every start had the first pass 15 s on and the
/// shop's first look 45 s on, or what was left of the plan time saved in
/// shop_clock.txt; that file is still written as a plan begins, and a whole
/// interval from the start is never sooner than the plan it holds.
std::uint64_t start_due_ms(std::uint64_t kept_ms, std::uint64_t now_ms, int every_minutes, std::uint64_t least_ms);

/// The shop right after a Dọn rác pass (2026.11.11, the owner: "Tới giờ mua:
/// dọn rồi mua"). The shop keeps its own clock and plan rules; a timed look of
/// it that comes due -- a plan, or a part still owed -- with Tự dọn rác on
/// pulls the pass forward to now, and the round rides it: bought right after
/// the pass under the same hold and the same hand on the game's mouse, with no
/// quiet second and no HP start gate of its own (GuiApp::trash_loop). The two
/// clocks knew nothing of each other before, and the shop came any time up to
/// an interval after a pass -- the bag full of loot again by then.
/// None: nothing decided for the look due. Pending: a pass pulled for it, the
/// round to ride that pass. Alone: the look goes on its own, by its own rules.
enum class ShopRide { None, Pending, Alone };
/// A pass that ended less than this long ago is fresh enough: a due shop goes
/// on its own at once rather than pull another.
constexpr std::uint64_t kRideFreshMs = 60'000;
/// The ride at a turn of the trash worker's loop, from the last turn's (`was`):
/// None with no timed look due (`shop_due`) or Tự dọn rác off (`trash_on`).
/// A look just come due is Pending -- a pass pulled, the round to ride it --
/// unless a pass ended within kRideFreshMs of `now_ms` (`last_pass_end_ms`, 0
/// for none yet) or no pass may be pulled now (`cannot_pull`): where the
/// shop's own cheap gates would put the look off -- the chat line in doubt,
/// no game window or the game off the screen, HP not read (not in the world),
/// a keeper's crossing or cold start -- or a pass found nothing it could do
/// a while ago (2026.11.11, the review's R1 and R2): Alone then, the shop
/// waiting by its own rules. A look already decided stays as it was: a pass
/// its gates put off is not pulled again every turn, only once the shop's own
/// wait is over and the next look comes due.
ShopRide ride_turn(ShopRide was, bool shop_due, bool trash_on, bool cannot_pull, std::uint64_t now_ms,
                   std::uint64_t last_pass_end_ms);
/// How a pass a due shop pulled came out, for whether the round rides it.
struct RidePass {
    bool ran = false;           ///< it took the mouse and the hold, and the cleaner ran
    bool timed = false;         ///< a timed pass of a worker that runs with the bot, never a button's
    bool stopped = false;       ///< DỪNG, F12, the worker told to stop
    bool look_only = false;     ///< Thử tìm
    bool left_held = false;     ///< begun to give back what may be left in the bin
    bool keeps_hold = false;    ///< BagCleaner::Result::keeps_hold after it: something may be left up
    bool bag_ran = false;       ///< BagCleaner::Result::ran
    bool trouble = false;       ///< it stopped early -- HP or the hold lost included
    bool board_up = false;      ///< the check-in board was up
    bool hold_ours = false;     ///< the "trash cleaning" hold still this worker's
    bool shop_due = false;      ///< Tự mua bình still on, and its look still due
};
/// Why the round does not ride `p`, in a few words for the log -- nullptr when
/// it does: a timed pass that ran clean, the hold still in hand. Otherwise the
/// shop goes on its own, by its own rules.
const char* ride_refused(const RidePass& p);

/// "Mua ngay sau khi dọn rác" (2026.11.13b, CleanSettings::buy_after_trash):
/// whether the shop buys right after each timed pass rather than by its clock
/// -- the box ticked, with Tự mua bình and Tự dọn rác on. With Tự dọn rác
/// off the clock buys, as before.
bool buys_after_pass(const CleanSettings& s);
/// Whether the shop's own clock may make a timed look, buying after each pass
/// (`after_pass`, buys_after_pass): only for what is still owed of a plan
/// (`owed`, a part's gap on), or a look a pass left owed (`look_owed`: its
/// ride refused, or the round put off by a gate, tried again by the shop's
/// rules). A plan of its own the clock never begins then. Not buying after
/// each pass: always, as before.
bool shop_clock_looks(bool after_pass, bool look_owed, int owed);
/// Whether a plan begins at a round, the numbers owed afresh: "Mua ngay"
/// (`forced`) always; buying after each pass, the look a pass left owed and
/// no other (`look_owed`); otherwise the plan's own time come (`plan_time`).
bool shop_plan_begins(bool forced, bool after_pass, bool look_owed, bool plan_time);
/// Whether a timed pass -- one that ran, or one with nothing it could do --
/// owes the shop a look, buying after each pass: only with nothing of a plan
/// still owed (`owed`). A part still owed is that plan's, bought as a part --
/// the review of 2026.11.13b: a part's look pulls a pass (kShopPartGapMs is
/// kRideFreshMs), and that pass owing a look began the plan afresh every
/// part, buying until the bag was full.
bool pass_owes_look(bool after_pass, int owed);

/// HP and the Dọn rác pass and the potion shop. 2026.11.11 ("Mua luôn, bơm
/// xen") started them at the HP potion's mark and stopped them under half of
/// it when the potion could drink during the job, else at the window's floor
/// + 10 and under it. Since 2026.11.11b (the owner: "Hiện tại máu thấp đang
/// không được mua bình, làm cho có thể mua bình ở bất cứ mức máu nào và vừa
/// mua vừa bơm máu", "Cả dọn rác") both go at any HP: `start` and `stop` are
/// kJobDownHpPct whatever the arguments -- only the character down holds a
/// job back or stops it -- and the HP and MP potions go between the job's
/// clicks (the macro's through the key window, or the job's own presses,
/// BagCleaner::OwnPotions). `drinks` still says whether the HP potion goes
/// through the window. `floor` -- chat's and the other jobs' -- is not
/// changed by this. The charm run has gone at any HP too since 2026.11.12b,
/// by kJobDownHpPct, pressing its own potions between two charms.
constexpr int kJobDownHpPct = 1;
struct HpGates {
    bool drinks = false;        ///< the HP potion is drunk through the job's key window
    int  start = 0;             ///< a timed pass or shop look starts with HP at this or over
    int  stop = 0;              ///< the job's guard stops it with HP under this
};
HpGates hp_gates(bool drinks, int hp_mark, int floor);
/// A timed pass or shop look put off for HP under its start looks again this
/// much later.
constexpr std::uint64_t kHpLookAgainMs = 5'000;
/// A pass or a shop round its guard stopped for HP is tried again this much
/// later -- not the pass's interval, nor a part's minute -- and the round is
/// never trouble for it.
constexpr std::uint64_t kHpStoppedAgainMs = 15'000;

/// A timed bag job and the macro's HP potion on a key the job neither lets
/// through its key window nor presses itself -- a digit, as the shipped 1 and
/// 2 are, which no bag job lets go (the owner: "Giữ chặn phím số") -- 2026.11.21,
/// the audit's B2 (the owner: "không để các chức năng block nhau"). The jobs go
/// at any HP (2026.11.11b, 2026.11.12b: no HP mark gates them, which kept the
/// charms off all night at HP under 90), so with that potion due -- the tier
/// the macro would press for the reading, its cooldown over -- nothing drank
/// while the job held the game, and the guard stopped it only with the
/// character down. Now the job gives the hold back -- between two items, two
/// purchases or two charms, on the safe points the guard is asked at, never
/// with a quantity box up (BagCleaner::set_yield) -- once it has held the game
/// kDrinkYieldAfterMs (`ran_ms`), so the macro drinks, and the job is tried
/// again kHpStoppedAgainMs later. Not before kDrinkYieldAfterMs: with HP
/// hovering under the mark and the potion out, a job that gave way at once
/// would never get anything done. Never a job begun with something maybe
/// left in the bin, nor one whose hold is kept (`kept`): it keeps the hold
/// whatever it does. `potion_held_off`: the macro runs, its HP potion is due,
/// and the job's window / own presses do not take that key.
constexpr std::uint64_t kDrinkYieldAfterMs = 10'000;
bool drink_yield(bool potion_held_off, bool kept, std::uint64_t ran_ms);

/// The trash worker's way out of a hold it keeps (2026.11.21, the audit's B1):
/// a shop round cut short by the check-in board over its quantity box in the
/// board's night hold keeps "trash cleaning" (Result::stuck), and after 00:03
/// nothing ended it -- the popup closer, the only one to ESC the board, will
/// not under that hold, look_for_clear lets go only with the board gone, and
/// the kept pass stops at the board -- so no potion, skill or fight until a
/// person pressed ESC; the same with any two-button dialog nobody answers. The
/// worker itself then sweeps with ESC under its own hold (BagCleaner::
/// sweep_kept, the owner's stuck rule, 2026.11.14b): once the hold has been
/// kept kReactMs, the board -- when it is seen -- up kReactMs and out of its
/// night hold (the owner: "Trường hợp qua ngày thì chờ tới 00:03 mới ấn ESC"),
/// and kKeptSweepGapMs after the last sweep. The hold is let go only on a clean
/// look after it, as ever.
struct KeptSweep {
    bool held = false;              ///< the worker keeps the hold for something left up
    bool hold_ours = false;         ///< the coordinator's hold is still "trash cleaning"
    std::uint64_t now_ms = 0;
    std::uint64_t kept_ms = 0;      ///< since when the hold has been kept; 0 unknown
    std::uint64_t board_ms = 0;     ///< since when the check-in board has been seen up; 0 not seen
    std::uint64_t swept_ms = 0;     ///< the last sweep; 0 none
    bool night = false;             ///< the PC's clock in the board's night hold (checkin_night_hold)
};
constexpr std::uint64_t kKeptSweepGapMs = 60'000;
bool kept_sweep_due(const KeptSweep& k);
/// A shop round that ended in trouble -- stuck and shut with ESC, the tab not
/// switched, a step that did not go -- is tried again this much later: 30 s
/// since 2026.11.15b (the owner: "Cần thực hiện lại sau 30s chứ không phải 10
/// phút"), 10 minutes before. Every one, since 2026.11.24, without limit (the
/// owner: "Kiểm tra các phần khác xem có phần nào không làm được thì dừng
/// không, cần thay cơ chế này bằng 1 cơ chế khác"): until then the second
/// trouble in a row dropped what was left of the plan until the next.
constexpr std::uint64_t kShopTroubleAgainMs = 30'000;

/// How a round of the shop ended, as whoever ran it saw it (2026.11.11).
struct ShopRoundSeen {
    bool full = false;          ///< the game said the bag is full (Result::shop_full)
    bool message = false;       ///< another message in the box's place (Result::shop_message)
    bool board_up = false;      ///< the check-in board (Result::board_up)
    bool stopped = false;       ///< DỪNG, F12, the worker told to stop
    bool hp_cut = false;        ///< its guard stopped it for HP under the job's level
    bool hold_lost = false;     ///< the hold no longer the job's after it
    bool keys_late = false;     ///< a key of the macro's not seen up in time (Result::keys_late)
    bool trouble = false;       ///< anything else said in Result::trouble, or the bag not searched
};
/// Full and Message as the game said them; then Paused -- never Trouble, which
/// is counted in a row (two in a row dropped the rest of the plan until
/// 2026.11.24) and waits kShopTroubleAgainMs rather than a part's gap -- for
/// the board, a stop, HP the guard stopped it for (whatever HP reads by the
/// end: a potion may have lifted it since), the hold lost, and a key of the
/// macro's not seen up in time; Trouble for anything else; Clean.
ShopEnd shop_end(const ShopRoundSeen& s);

/// The shop's row of potions anywhere on `f`, its buttons or not: a shop
/// whose Mua | Thoát an item's description lies over is still up.
bool shop_row_up(const Frame& f, const TemplateStore& store);

/// Whether `d` is taken for the quantity question a shop's Mua brings up: a
/// dialog with the two buttons that is not the bin's, not the registration's
/// and not another question GoBot has a picture of -- the sell question
/// included, by its words -- whose box under its words is drawn like the sell
/// question's, the box's own digits left out, at `frame_score` or more, and
/// that has a box with a number or nothing in it: one that reads. There is no
/// picture of the buy question itself. The rest of what makes it safe is not
/// in the picture: it came up within seconds of the one double-click on a
/// potion, with no
/// other dialog up, and the number is read back before Xác Nhận.
bool shop_question(const Frame& f, const TemplateStore& store, const Dialog& d, const CountReader& counts,
                   float frame_score = CleanSettings::kBuyFrameDefault, bool check_box = true);

/// Whether the pictures a purchase needs are there: the shop's row and
/// buttons, the sell question, the bin's questions and the revive and drop
/// questions it is told from, the full-bag message, and the close buttons, to
/// know of another window open beside the bag.
bool can_check_shop(const TemplateStore& store);

// ------------------------------------------------------------ an NPC's shop
//
// 2026.11.24, the owner: "khi làm đội trưởng để đi Cổ Long thì không mua được
// Lư Hương. Cụ thể là khi ấn vào Lư Hương thì không ấn mua mà lại tắt luôn
// shop mua". The shop an NPC's "Shop" row opens -- Lư Hương Long Mỹ at Cổ
// Long's, the pickaxes at Thợ Mỏ's -- is the window the Người Giữ Sân item
// opens: its Mua | Thoát (kShopButtons) and its list of two columns of cells.
// A cell, as GoC's own pictures of three NPCs' shops have it (grabbed/
// pickaxe_npc_shop, rod_in_shop, baiter_in_shop) and as this client draws its
// potions (kShopRow), all four measured alike: the item's icon box at its
// left, its middle kShopMpX across and kShopIconY down from the cell's
// top-left; beside it the name over the price, drawn with no spaces --
// "CuốcHắcQuang", "CáNhỏ", "Rượulátre" -- and ending at kShopNameLastX
// whatever its length, the middle of its letters kShopIconBelowName over the
// icon's middle. The columns' cells kShopCellPitch apart, the left one's left
// edge kShopButtonsDx left of the buttons, as kShopRow's is.
constexpr int kShopCellPitch = kShopHpX - kShopMpX;
constexpr int kShopIconY = 18;
constexpr int kShopNameFieldX = 38, kShopNameFieldEndX = 139;    ///< the name's field, across the cell
constexpr int kShopNameLastX = 135;                              ///< the name's last pixel, across the cell
constexpr int kShopIconBelowName = 8;
/// How far the icon's middle as the name's right edge puts it may be from
/// its column's: MenuText's letters are not the game's, a few pixels wider or
/// narrower over a name; a name found anywhere else in its field is a piece
/// of another name, or a list laid out otherwise than the one measured.
constexpr int kShopIconSlack = 20;

/// Where the item whose name MenuText found at `name` (its letters with a
/// pixel round them) is double-clicked, in the shop whose Mua | Thoát are at
/// `buttons`: the middle of its icon box -- the spot the potion shop has
/// double-clicked live since 2026-10-31 -- in the column whose name field
/// holds the name, kShopIconBelowName under the name's middle. Not `ok`, and
/// nothing to click, when the name lies in neither column's field, or ends
/// more than kShopIconSlack from where the names of that column end: a
/// double-click there could land on the next item.
struct ShopCell {
    bool ok = false;
    int  column = -1;       ///< 0 the left column, 1 the right; -1 neither
    int  x = 0, y = 0;      ///< the icon box's middle: what is double-clicked
    int  by_name_x = 0;     ///< the icon's middle as the name's right edge puts it
    const char* why = "";   ///< not ok: why, for the log
};
ShopCell shop_cell(const ScreenRect& name, const ScreenRect& buttons);

/// Where an NPC's shop with its Mua | Thoát at `buttons` has its list's names:
/// both columns' name fields, from under the window's title to just above
/// the buttons -- what MenuText searches for an item, never the window's
/// title or anything beside the window.
ScreenRect shop_list(const ScreenRect& buttons);

/// The quantity box of a question whose two buttons are at `pair`: left of
/// them, a little inset -- what CountReader::read_field reads, as for the
/// bin's quantity question and the potion shop's.
ScreenRect quantity_box(const ScreenRect& pair);

/// How much of quantity_box(pair) on `f` is a quantity box's dark field, 0 to
/// 1: the sell question's (grabbed/sell_confirm) is (58..74, 66..74, 74..90)
/// round its digits, where a question with no box -- the bin's, the revive's,
/// a message -- has the light bar (214, 214, 222) its buttons stand on.
/// kFieldBox and over: a box is there; kFieldNone and under: none. 0 off the
/// frame.
float quantity_field(const Frame& f, const ScreenRect& pair);
constexpr int   kFieldDarkest = 120;    ///< a field's pixel: no channel brighter
constexpr float kFieldBox = 0.60f;
constexpr float kFieldNone = 0.15f;

/// Whether the two buttons of a question are still at `pair` on `f`: either
/// button alone, as either drawing of the pair has it, within three pixels --
/// one is drawn pressed after keys the box did not take, or lit with the
/// pointer over it after a click, never both at once, and find_dialogs then
/// misses the pair (the live client, 2026-10-31). BagCleaner::question_still's
/// look, for a question whose place is known.
bool pair_still(const Frame& f, const TemplateStore& store, const ScreenRect& pair);

// ---------------------------------------------------------- the shop's tabs
//
// 2026.11.24, the owner: "Khi ra tới bản đồ TổCổLong thì cần mua lại HP cho
// pet. Cái này ở trong tab Linh Thú của người giữ sân và là item "Thịtbò"".
// The Người Giữ Sân item's shop -- "Cửa Hàng" -- has five tabs across its top,
// Dược Phẩm (chosen as it opens: the potions), Vũ Khí Phụ, Tạp Hóa, Linh Thú
// and Công Thức Chế Tạo. Measured on the live client's own pictures of it,
// trash_debug\20261111_214922_0_shop_full.png and 20261111_221637_5_shop_full.png
// (1024x768, its Mua | Thoát -- kShopButtons -- found at 497,541 on both, 0.17
// of a grey level from the picture): five tabs of 64 pixels side by side,
// Dược Phẩm's from x 306 to 369 -- kShopTab0Dx, 191 left of the buttons --
// Linh Thú's from 498 to 561, every one from y 178, its light top edge, to 198
// -- kShopTabDy, 363 above the buttons. The tab chosen is drawn dark (14, 14,
// 14 on the rows above and below its words, y 179 to 181 and 195 to 196), the
// others grey (115, 123, 132); the words between. A tab is clicked in its
// middle: Linh Thú's at 530,188, 33 right of the buttons' top-left and 353
// above. The same sprite at every size of the game, so the same offsets.
constexpr int kShopTabs = 5;
constexpr int kShopTabPotions = 0;      ///< Dược Phẩm: Thịt bò khô, Rượu lá tre
constexpr int kShopTabPet = 3;          ///< Linh Thú: Thịt bò, the pet's
constexpr int kShopTab0Dx = -191, kShopTabDy = -363;
constexpr int kShopTabW = 64, kShopTabH = 21;
/// The rows of a tab, from its top, with nothing of its words on them: its
/// body's colour says whether it is the one chosen.
constexpr int kShopTabBodyRows[] = { 1, 2, 3, 17, 18 };
/// Each tab's words, as the shop writes them.
inline const char* const kShopTabNames[kShopTabs] = { "Dược Phẩm", "Vũ Khí Phụ", "Tạp Hóa", "Linh Thú",
                                                      "Công Thức Chế Tạo" };
/// Tab `tab` (0 to kShopTabs - 1) of the shop whose Mua | Thoát are at
/// `buttons`: the whole tab, its top edge included. What is clicked is its
/// middle.
ScreenRect shop_tab_rect(const ScreenRect& buttons, int tab);
/// Whether tab `tab` is drawn chosen on `f`: 1 its body dark, 0 grey, -1 can
/// not be told -- something over it, off the frame, no shop there. Each of
/// kShopTabBodyRows across the tab but for a pixel at each side: kShopTabSure
/// of them dark (no channel over kShopTabDarkMost) is chosen, as many grey
/// (kShopTabGreyLeast to kShopTabGreyMost) is not.
int shop_tab_drawn(const Frame& f, const ScreenRect& buttons, int tab);
constexpr int   kShopTabDarkMost = 60;
constexpr int   kShopTabGreyLeast = 90, kShopTabGreyMost = 160;
constexpr float kShopTabSure = 0.85f;
/// The strip of tabs on `f` as one look reads it: `strip`, every tab read as
/// chosen or not and exactly one chosen -- what says the tabs are where the
/// buttons put them before one is clicked -- and which (`chosen`, -1 none).
struct ShopTabs {
    bool strip = false;
    int  chosen = -1;
};
ShopTabs shop_tabs(const Frame& f, const ScreenRect& buttons);

/// Whether the words MenuText found at `name` in column `column` of the list
/// of the shop whose Mua | Thoát are at `buttons` are an item's whole name: no
/// letter of the game's -- grey over MenuText's cut, 150 -- in that column's
/// name field on the words' rows, right of them (from kShopNameTail past their
/// box to where the names end) or left of them (from the field's start to
/// kShopNameTail short of their box), in kShopNameInkCols columns or more. The
/// names are drawn right to left from the same place, so "Thịtbò" found in
/// Dược Phẩm's "Thịtbòkhô" has khô's 16 columns right of it -- 17 pixels short
/// of the names' end, inside shop_cell's kShopIconSlack, so shop_cell alone
/// would take it -- and one found at the end of a longer name has letters
/// left of it.
bool shop_name_alone(const Frame& f, const ScreenRect& name, const ScreenRect& buttons, int column);
constexpr int kShopNameTail = 3;
constexpr int kShopNameInkCols = 2;

/// An item found by its name in the list of the shop whose Mua | Thoát are at
/// `buttons` (2026.11.24): MenuText over shop_list, `list_name` as the list
/// draws it (no spaces), in an item's place (shop_cell) and whole
/// (shop_name_alone). A find that is not -- a piece of a longer name, words
/// out of an item's place -- is wiped from a copy of the list and the list
/// searched again, kShopItemFinds finds at most: the words wanted may be
/// another cell's. `cell` is what is double-clicked.
struct ShopItemSeen {
    bool        ok = false;
    ShopCell    cell;
    ScreenRect  at;             ///< the words found last (the one taken, when ok)
    float       score = 0.0f;   ///< MenuText's for them
    int         passed = 0;     ///< finds passed over
    const char* why = "";       ///< not ok: why, for the log
};
ShopItemSeen shop_item(const Frame& f, const ScreenRect& buttons, const std::string& list_name);
constexpr int kShopItemFinds = 3;

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

/// Whether other_window_up searches for its close buttons one after another
/// on the caller's thread rather than each on a thread of its own: so on a PC
/// of two logical CPUs or fewer (2026.11.16, a user's PC of one core, with the
/// game, stuttered), where the extra threads only take turns on the same core
/// and cost a frame's worth of OpenCV buffers each. The answer is the same
/// either way: the matches are taken in kCloseButtons' order both ways.
bool searches_inline();

/// For tests: 1 forces the searches inline, 0 onto their own threads, and
/// anything else gives them back to the PC's own count of CPUs.
void force_searches_inline(int how);

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
    /// Neither copied nor moved: its grab_ calls back into it.
    BagCleaner(const BagCleaner&) = delete;
    BagCleaner& operator=(const BagCleaner&) = delete;

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

    /// Whether the jobs before this one may have left an item in the bin, or
    /// its dialog open (Result::keeps_hold): a pass with no tab to look
    /// through (bag::pass_tabs) still runs then, to give that back. Unset, it
    /// stops before its first look, nothing pressed -- not even the bag key.
    void set_left_held(bool on) { left_held_ = on; }

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
        /// A key sent as its scan code alone, GoC's way (InputManager::KeyInject::
        /// Scancode); `key` when unset.
        std::function<bool(std::uint32_t)>           scan_key;
        /// A double-click; two of `click` when unset.
        std::function<bool(int, int)>                double_click;
        /// The pointer moved there, no button pressed: what park_pointer and
        /// the shop's moves off its list and off its question do with pretend
        /// hands. Unset, pretend hands leave the pointer where the last click
        /// put it; the real pointer is never moved by them.
        std::function<bool(int, int)>                move;
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

    /// How long the game is given to react to each of the run's own inputs
    /// (2026.11.15d, the owner: "khi GoBot thực hiện thao tác, sẽ có thêm 30
    /// giây chờ phản ứng. Nếu màn hình phản ứng luôn thì thực hiện tiếp bước
    /// tiếp theo, còn nếu màn hình không phản ứng gì thì sẽ dừng xử lý"):
    /// kReactMs unless set, and set only by a test that runs on the real clock
    /// and must not sit through 30 s of it. The owner's stuck rule (kStuckMs)
    /// goes by it too.
    void set_react_ms(int ms) { react_ms_ = std::max(1, ms); }
    int  react_ms() const { return react_ms_; }
    /// The second of the local day, 0 to 86399, for the check-in board's night
    /// hold in esc_out (checkin_night_hold): local_second_of_day() unless set
    /// -- a test's pretend clock of the day, or -1 for none.
    void set_day_clock(std::function<int()> second_of_day) { day_clock_ = std::move(second_of_day); }

    /// The macro's letter and Space keys while a job runs (2026.11.10, the
    /// owner: "Khi dọn rác thì cần ra các skill thuộc phím chữ cái", "cả khi
    /// mua HP và MP cũng cần ra skill", "lúc dùng bùa thì không cần ra skill"):
    /// the hold's key window (ExecutionCoordinator::arm_pass_keys), through
    /// these, a test's pretend one or the coordinator. Only a Pass -- a look
    /// only too -- and a ShopBuy run open it, never a charm run; and only on a
    /// look just taken with nothing up on it: no dialog, no message, no
    /// check-in board, no quantity question, nothing out towards the bin.
    /// Every click, drag, right-click, double-click and key of the run's own
    /// shuts it first, the keys let through seen up (kKeysUpWaitMs at most) --
    /// one still down, and that input is not made and the run stops. A shop
    /// round shuts it from just before each double-click until the question
    /// is seen gone and the full-bag message has had its time (kShopFullLateMs).
    /// A slot is dragged or right-clicked in a pass only from a look taken with
    /// it shut and kPotionSettleMs after the last potion let through went down:
    /// the bag may change after one. Unset, nothing is let through.
    ///
    /// A potion break (2026.11.11, the owner: "Mua luôn, bơm xen"): with an HP
    /// or MP potion due whose key the window lets through (`potion_due`), the
    /// window is opened -- through all of keys_open's checks -- and kept open
    /// kPotionBreakMs at most while one still is, HP's going first: before each
    /// purchase's double-click, on the look that first sees the shop, and in a
    /// pass between two items on the look handed over clean. Never the pet's.
    ///
    /// Home, End, PgUp, PgDn, Insert and Delete go through it too (2026.11.11,
    /// the owner: "Home End PgUp PgDn Ins Del"; pass_key_allowed). What the
    /// game does with them on the bag or the shop is not known -- a page or the
    /// shop's list scrolled, the box's caret moved -- so after one came up
    /// (`nav_ago_ms`) the run's next input of its own, and the look it goes by,
    /// wait until kNavSettleMs after it (keys_shut); and a shop's double-click
    /// goes only by a look on which the potions' row itself was matched.
    struct KeyWindow {
        std::function<bool()>    open;          ///< let the keys through; false when it may not be opened
        std::function<bool(int)> shut;          ///< no more through, and those that went up within `ms`
        std::function<int()>     potion_ago_ms; ///< since a potion let through went down; -1 none
        std::function<bool()>    potion_due;    ///< an HP or MP potion due whose key it lets through; unset none
        std::function<int()>     nav_ago_ms;    ///< since one of the six no letter let through came up; -1 none
    };
    void set_key_window(KeyWindow w) { keys_ = std::move(w); }
    /// HP and MP potions the run presses itself (2026.11.11b, the owner: "GoBot
    /// tự bấm bình", "Giữ chặn phím số"): with the macro not running -- the bot
    /// stopped, "Mua ngay" or "Dọn ngay" pressed -- nothing else presses them,
    /// so a Pass or a ShopBuy run does, at the moments of a potion break and
    /// on the same checks as the key window (nothing up, nothing out, never in
    /// the money stretch or a charm run): the key `due` names -- one the
    /// window would let through, never a digit; HP's first, never the pet's --
    /// pressed through the run's own hands, then kPotionSettleMs waited, and
    /// the next double-click or drag goes by a look taken after that. Unset,
    /// nothing is pressed. Set only with no key window (the macro running
    /// presses its own potions through that). Since 2026.11.12b a CharmsOnly
    /// run has them too, the macro running or not -- its hold lets no window
    /// through -- pressed at the top of a round of charm_sweep, between two
    /// charms, on keys_clear (no dialog of any kind on that look).
    struct OwnPotions {
        std::function<std::uint32_t(std::string&)> due;     ///< the key to press now, 0 none; its line for the log
        std::function<void(std::uint32_t)>          pressed; ///< that key went down
    };
    void set_own_potions(OwnPotions p) { own_potions_ = std::move(p); }
    static constexpr int kKeysUpWaitMs = 600;
    static constexpr int kPotionSettleMs = 500;
    static constexpr int kPotionBreakMs = 400;
    static constexpr int kPotionBreakStepMs = 20;
    static constexpr int kNavSettleMs = 200;

    /// What a run is for. Pass: all of it -- every tab ticked, the fragments
    /// registered, the delete sweep. CharmsOnly: only the charms on the Dùng
    /// list that are due (set_charm_due), and nothing else: the bag opened if
    /// it is shut, Tiêu Dùng, one right-click and one question for each and
    /// the game's message after it closed with its own Xác Nhận, the bag shut
    /// again if it was opened here. Quick, with no tab but Tiêu Dùng, and
    /// nothing deleted. A Pass leaves the charms to it: it says where it sees
    /// one and never right-clicks it.
    ///
    /// ShopBuy: the purchases set_shop_plan asks for, Thịt bò khô and Rượu lá
    /// tre taking turns, from the Người Giữ Sân item's shop (see shop_buy) --
    /// nothing in the bag counted -- until the game says the bag is full; with
    /// set_dry_run one of each as far as the quantity box, nothing typed and no
    /// Xác Nhận. The list it goes by holds the two potions and the item as
    /// catalog icons (the settings' ids), and nothing in it is ever deleted.
    ///
    /// Every mode looks for the check-in board first, on the look before the
    /// bag key: with it up, nothing at all is pressed (Result::board_up).
    enum class Mode { Pass, CharmsOnly, ShopBuy };
    void set_mode(Mode m) { mode_ = m; }

    /// How many purchases of 100 a ShopBuy run makes of each potion, in the
    /// order bag::shop_order gives, kShopMaxBuys at most. Unset, or both 0, a
    /// run that is not a trial presses nothing. A trial (set_dry_run) tries
    /// one of each whatever this says.
    ///
    /// `pet` (2026.11.24): purchases of 100 of Thịt bò, the pet's HP food,
    /// from the shop's Linh Thú tab, before the potions (bag::shop_round_order)
    /// -- CleanSettings::buy_pet. The window's round on reaching TổCổLong asks
    /// for these alone. Never in a trial.
    void set_shop_plan(int hp, int mp, int pet = 0) {
        shop_plan_hp_ = std::max(0, hp);
        shop_plan_mp_ = std::max(0, mp);
        shop_plan_pet_ = std::max(0, pet);
    }
    /// The deadline of a ShopBuy run of `purchases`, in milliseconds: 30 s and
    /// 6 s a purchase, kShopDeadlineMostMs at most.
    static int shop_deadline_ms(int purchases);
    static constexpr int kShopDeadlineMostMs = 300'000;
    /// The most purchases of one run -- what "Mua ngay" may make in one hold --
    /// and of a timed run in one hold, the rest of its plan kShopPartGapMs
    /// later: a round of twenty would hold every skill, potion and buff off for
    /// a minute or more with nobody watching.
    static constexpr int kShopMaxBuys = 40;
    static constexpr int kShopBuysPerHold = 10;
    static constexpr int kShopPartGapMs = 60'000;
    /// A look before a double-click on which the shop was found by its buttons
    /// alone, the potions' row not matched -- the list scrolled, say: looked
    /// at again this many times, a look apart, and then the round stops with
    /// nothing clicked (2026.11.11).
    static constexpr int kShopRowRelooks = 3;
    /// How long a job that found the check-in board up waits before it looks
    /// again. Not counted as trouble.
    static constexpr int kBoardWaitMs = 60'000;
    /// The owner's stuck rule (2026.11.14b: "Trường hợp bị dừng quá 10s thì
    /// tắt toàn bộ hiển thị shop, túi đồ trên màn hình bằng phím ESC. Xử lý
    /// này thực hiện cho cả khi hộp nhập số lượng không biến mất"), made 30 s
    /// by the owner's 2026.11.15d rule ("sẽ có thêm 30 giây chờ phản ứng ...
    /// nếu màn hình không phản ứng gì thì sẽ dừng xử lý"): kReactMs, counted
    /// from the step's last input of GoBot's own -- for the shop the
    /// right-click that asks for it, a purchase's double-click, its quantity
    /// question seen, each typing into its box, its Xác Nhận, a Hủy Bỏ; for a
    /// pass an item's drag towards the bin, the click on the bin, each Xác
    /// Nhận. So a step's own wait for the game, up to kReactMs from that input
    /// (react_ms()), is never cut short, and a step nothing has come of for
    /// that long ends with esc_out: ESC until nothing of the shop, no dialog,
    /// no message, no check-in board and no bag is on the screen -- the stop's
    /// clean-up -- and the round to be tried again later. Never in a charm
    /// run, which presses no Esc.
    static constexpr int kStuckMs = kReactMs;
    /// A wait of kReactMs after an input that a stop, the guard's no, or a
    /// step given up on cut short still looks this long before it is over,
    /// as it always did: Hủy Bỏ on the bin's question, the bin clicked to
    /// bring one up, a Hủy Bỏ of cancel_all and shop_cancel (each of their
    /// clicks given react_ms() / kCancelClicksShare otherwise, the first
    /// click's kReactMs watched to its end).
    static constexpr int kCancelLookMs = 1800;
    static constexpr int kCancelClicksShare = 5;
    /// How often, inside a wait for the game of kReactMs, an HP or MP potion
    /// due is given its moment (potion_break, with every check of its own): a
    /// long wait is no reason for the character to go without, where the
    /// look allows a key at all -- never after a drag, a right-click or a
    /// double-click until what it brought is seen.
    static constexpr int kReactPotionMs = 2'000;
    /// One sweep: at most kEscMax ESCs, each on a fresh look that shows
    /// something up and given kEscGoneMs to close something; one that closes
    /// nothing is let go once, the second ends the sweep. Clear: nothing up on
    /// kEscClearLooks looks kEscClearGapMs apart -- those looks taken even
    /// when the sweep's time is up. After an ESC that closed nothing seen
    /// (the review of 2026.11.14b: a slow game may still be drawing what it
    /// shut) the next goes only on two looks at least kEscSettleMs apart that
    /// agree, so that it does not land on an empty screen and bring up the
    /// game's menu.
    static constexpr int kEscMax = 6;
    static constexpr int kEscGoneMs = 1500;
    static constexpr int kEscClearLooks = 3;
    static constexpr int kEscClearGapMs = 400;
    static constexpr int kEscSettleMs = 500;
    /// The game's own menu (grabbed/client_menu_dialog) and its X, as the popup
    /// closer has them (PopupEyes::kMenuXDx, kMenuXDy): come up after an ESC
    /// of the sweep's, that ESC went past what was up -- it counts as one that
    /// closed nothing -- and the menu is closed by its X, found within
    /// kEscMenuXSlack of that place as the popup closer finds it, looked at
    /// again after each click, kEscMenuClicks clicks at most; the sweep then
    /// goes on. One that was up before is the player's, and left alone.
    static constexpr int kEscMenuXDx = 156, kEscMenuXDy = 7;
    static constexpr int kEscMenuXSlack = 3;
    static constexpr int kEscMenuClicks = 2;

    /// Which charms are due, by TrashList::Item::clock_id: a charm used or
    /// perhaps used less than charm_every_minutes ago is not (CharmClock).
    /// Unset, every charm is.
    void set_charm_due(std::function<bool(const std::string&)> due) { charm_due_ = std::move(due); }

    /// Asked before the bag key and before every right-click of a charm run:
    /// false -- whatever holds everything else of the bot off is gone -- stops
    /// it there, with nothing more pressed. Asked too, with the hold check, on
    /// every look of a wait for the game to react (2026.11.15d, react_stop): a
    /// no ends that wait, and the step is given up as it always was.
    void set_guard(std::function<bool()> still_held) { guard_ = std::move(still_held); }
    /// The hold alone, HP not asked (2026.11.11): what a shop round asks
    /// before each click on the game's full-bag message, and on a lone message
    /// in the quantity box's place (shop_message, close_notice) -- closed with
    /// HP low all the same, for a message left up keeps the shop and the hold
    /// with it, but never under another job's hold. Unset, the guard is asked.
    /// The ESCs of a step stuck (esc_out) ask both, the hold and the guard:
    /// the guard's HP stop is the character down alone (kJobDownHpPct), and no
    /// ESC goes with the death panel up (the review of 2026.11.14b).
    void set_hold_check(std::function<bool()> ours) { hold_check_ = std::move(ours); }
    /// Whether the job gives its hold back now for the macro's HP potion
    /// (2026.11.21, the audit's B2; bag::drink_yield): asked only where the
    /// guard is asked between two items of a pass, before each purchase's
    /// double-click and the opener's right-click, and between two charms --
    /// nothing in the bin, no quantity box or question up -- never in a wait
    /// for the game, nor by the ESCs of a step stuck. Yes stops the run there
    /// as the guard's no does (Result::yielded, `trouble` saying why); whoever
    /// runs it tries again kHpStoppedAgainMs later. Unset, never. Never asked
    /// before the run's first item, purchase or charm's right-click either
    /// (the review of 2026.11.21): a slow start must not use the
    /// kDrinkYieldAfterMs up, so every run gets something done.
    void set_yield(std::function<bool()> yield) { yield_ = std::move(yield); }

    /// Called with the clock id of a charm just before it is right-clicked --
    /// before anything that cannot be taken back: whoever keeps the clock
    /// writes it down as used then, and corrects it from the outcome when the
    /// run is over. A GoBot closed or killed in between has still not used it
    /// twice.
    void set_charm_claim(std::function<void(const std::string&)> claim) { charm_claim_ = std::move(claim); }

    /// What the packets' bag says of a charm's slot (set_charm_packets).
    enum class PacketSays { Nothing, Agrees, Other, CountDiffers };
    /// The packets' bag on a charm's slot -- row * 6 + cell of Tiêu Dùng from
    /// the top -- for the catalog colour about to be right-clicked and the
    /// count read on the screen. Other: the game's own list names another
    /// item there, with the screen's count, and the slot is not
    /// right-clicked. CountDiffers: the list has the slot with another count
    /// than the screen's, or empty -- for a slot the screen reads as one, a
    /// stack of one told by its bare icon, it is not right-clicked; for a
    /// count printed it says nothing, the slot being most likely not the one
    /// the screen shows (a page scrolled, a bag sorted). Nothing: it has
    /// nothing current to say. `said` is for the log: the item and count the
    /// slot holds for Agrees, Other and CountDiffers ("8871081 x63", or
    /// "empty"), else why nothing. Only ever stops a right-click, never
    /// causes one; unset, the screen decides.
    void set_charm_packets(std::function<PacketSays(const std::string&, int, int, std::string&)> look) {
        charm_packets_ = std::move(look);
    }

    /// For tests: what a charm run says once each GoBot start -- a colour it
    /// is not sure of, a slot the packets' bag stops -- may be said again.
    static void forget_said_once();

    /// A message of GoBot's own that the last charm run left up, or may have:
    /// the pair of the question whose Xác Nhận it followed (Result::notice_place).
    /// This run closes it -- with its own Xác Nhận, where that pair puts it
    /// (bag::message_band) -- before anything else, the bag key included; and
    /// with no charm due, does nothing else. A message anywhere else, or none
    /// handed over, is anyone's, and left alone. One that the last run's two
    /// clicks did not close is handed over the same way since 2026.11.24, and
    /// clicked again here, twice at most. `clicked_out`: the last run clicked
    /// it to no end (the 2026.11.24 review) -- this run makes one click and
    /// watches it kNoticeCloseMs x 2, not the whole 30 s reaction wait the
    /// run that first met it already spent: a message that never closes
    /// costs the hunt a few seconds a minute, not 30.
    void set_own_notice(const ScreenRect& pair, bool clicked_out = false);

    /// The deadline of a charm run with `due_kinds` kinds due, in milliseconds:
    /// 12 s and 3 s a kind, kept between 20 and 40 s. The bag opened, Tiêu Dùng,
    /// its top and its pages take most of the first 20.
    static int charm_deadline_ms(int due_kinds);

    /// The most a run may take from here on, in milliseconds: it stops, with
    /// nothing more right-clicked, when it has. 0: no limit. Asked only
    /// between steps, never inside a wait for the game; and the time the game
    /// took to react to an input past kReactSlowMs -- up to kReactMs on a slow
    /// PC (2026.11.15d) -- moves it on by as much (react_took), so that a slow
    /// game's waiting is not what runs a charm run or a shop round out of time:
    /// the deadline stays the backstop for a run that is slow for any other
    /// reason.
    void set_deadline_ms(int ms);
    /// What a wait for the game of `ms` from an input moves the deadline on
    /// by: what it was past kReactSlowMs, less what an earlier wait measured
    /// from the same input has moved it on by already -- `credited_ms` of it
    /// (the review of 2026.11.15d: a charm's question gone and its stack down
    /// are both measured from its Xác Nhận, and were counted twice). Never
    /// below 0.
    static int react_credit_ms(int ms, int credited_ms = 0);

    /// A look at the bag's strip that reads no tab chosen at all -- something
    /// drawn over it for a moment, the pointer resting on it -- is looked at
    /// again, up to kTabRelooks times kTabRelookMs apart, before it stops the
    /// run: after the click on a tab (select_tab), and in the shop's look for
    /// its item on Khác and the charm sweep, the pointer moved off first. A look
    /// that reads another tab chosen stops those two at once. Nothing is
    /// bought or right-clicked on a look that does not read the tab wanted.
    static constexpr int kTabRelooks = 8;
    static constexpr int kTabRelookMs = 150;
    /// The shop's click on Khác (2026.11.14b): up to kShopTabClicks clicks on
    /// a strip read as another tab for sure -- a pass and a charm run keep two
    /// -- the third and fourth kTabRetryPauseMs after a fresh look, a pixel
    /// further aside, and each looked after for kTabRetryLookMs by the clock.
    /// On 2026-11-14 at 21:37 the game took neither of two clicks.
    static constexpr int kShopTabClicks = 4;
    static constexpr int kTabRetryPauseMs = 400;
    static constexpr int kTabRetryLookMs = 1200;

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

        /// Charms (Bùa), apart from the fragments above: every right-click of
        /// the run and how it came out, for CharmClock -- a charm perhaps used
        /// is never right-clicked again before its hour is over.
        struct CharmTry {
            std::string      id;        ///< TrashList::Item::clock_id
            CharmClock::How  how = CharmClock::How::NotUsed;
            std::string      why;       ///< in Vietnamese, when it was not seen used
        };
        std::vector<CharmTry> charm_tries;
        int  charms_used = 0;           ///< seen taken
        int  charms_seen = 0;           ///< on the Dùng list and found in the bag
        int  charms_waiting = 0;        ///< of those, not due yet
        /// Slots drawn like a charm on the Dùng list and left alone, each
        /// slot once a run: of another colour, not chosen; of a colour that
        /// is not sure; and of a kind chosen that the packets' bag says the
        /// slot does not hold.
        int  charms_other_colour = 0;
        int  charms_colour_doubt = 0;
        int  charms_vetoed = 0;
        /// Slots of a kind due left alone because no count reads there --
        /// nor its bare icon, for a stack of one (CountReader::read_charm) --
        /// each slot once a run: after its use it could not be looked at.
        int  charms_unread = 0;
        /// Kinds on the Dùng list that were due when the run began, by clock
        /// id; and the game's messages after a charm's Xác Nhận closed with
        /// their own Xác Nhận.
        int  charms_due = 0;
        int  charm_notices = 0;
        /// A Xác Nhận of this run on a charm's question -- or the one handed
        /// over by set_own_notice -- whose message was not seen closed: it may
        /// still be up, or come. `notice_place` is that question's pair, for
        /// the next run's set_own_notice; `notice_carried`, that it was the one
        /// handed over, which is not handed on for longer -- unless this run
        /// clicked it (`notice_clicked`). Since 2026.11.24 a message two clicks
        /// did not close is handed over too (the owner: "Kiểm tra các phần khác
        /// xem có phần nào không làm được thì dừng không, cần thay cơ chế này
        /// bằng 1 cơ chế khác"): the next run clicks its own Xác Nhận again,
        /// twice at most, at the place remembered only, never a key, never with
        /// a two-button dialog up -- and so on, without limit. Until then that
        /// message was the player's, and no charm was used while it was up.
        bool notice_left = false;
        ScreenRect notice_place;
        bool notice_carried = false;
        /// That message clicked by this run -- its own Xác Nhận, once or twice
        /// -- and still not seen closed (2026.11.24): GoBot's window renews the
        /// two minutes it is GoBot's to close from this run, so it stays
        /// GoBot's while GoBot keeps clicking it, about once a minute.
        bool notice_clicked = false;

        /// The shop (Mode::ShopBuy): purchases seen through -- the question
        /// gone after Xác Nhận, and no message after it -- in all and of each
        /// potion; and those whose Xác Nhận was clicked and whose outcome was
        /// not seen, which stop the run and count as bought (never bought
        /// twice). Whether the shop was seen open.
        int  shop_bought = 0;
        int  shop_bought_hp = 0, shop_bought_mp = 0;
        int  shop_unsure = 0;
        int  shop_unsure_hp = 0, shop_unsure_mp = 0;
        bool shop_opened = false;
        /// The pet's food (2026.11.24): its purchases seen through and unsure,
        /// counted in shop_bought and shop_unsure as well; and `pet_ended`
        /// when the game said no during one of them -- the pet's bag full
        /// ("Túi đồ [Linh Thú] đã đầy", a message GoBot has no picture of),
        /// or another message in the quantity box's place or after Xác Nhận:
        /// its message closed, the pet's buying over for this round, the rest
        /// of the round going on. No trouble, and never shop_full nor
        /// shop_message, which end the potions' plan.
        int  shop_bought_pet = 0;
        int  shop_unsure_pet = 0;
        bool pet_ended = false;
        /// The game said the bag has no room: the round ends there, the
        /// message closed -- `full_stuck` when two clicks did not close it,
        /// and then no key is pressed for it, ESC least of all.
        bool shop_full = false;
        bool full_stuck = false;
        /// A message GoBot does not know came up instead of the quantity box,
        /// on two looks: its picture kept (shop_message), its Xác Nhận
        /// clicked, and the round ended as for a full bag -- most likely the
        /// same message drawn a little apart from the picture.
        bool shop_message = false;
        /// Messages GoBot does not know that came up after a purchase's Xác
        /// Nhận, or before the shop's ESC, and that GoBot closed by their own
        /// Xác Nhận (shop_notice). One after a purchase counts that purchase
        /// as bought (shop_unsure) and the round goes on; the second ends it.
        int  shop_notices = 0;
        /// The check-in board was up: nothing pressed from then on -- at the
        /// start of a run, with no key pressed at all -- and the run is to be
        /// looked at again later (kBoardWaitMs). Not trouble.
        bool board_up = false;
        /// The run stopped on the bag's strip not reading as the tab it
        /// wanted -- no tab drawn chosen after its looks again, or another --
        /// at the tab's click or in the shop's look on Khác (`trouble` says
        /// so), or in the charm sweep (said, and no trouble: nothing more was
        /// right-clicked). Not "no charm
        /// there": none was looked for. Whoever runs a charm job waits longer
        /// before the next (gui_app.cpp).
        bool tab_unsure = false;
        /// A key the window let through was not seen up before an input of
        /// the run's own, which stopped it (kKeysLateVi in `trouble`): no
        /// trouble of the round's own, put off as a stop is (2026.11.11).
        bool keys_late = false;
        /// A step stuck kStuckMs (2026.11.14b): ESC pressed this many times,
        /// each on a look that showed something up (esc_out); `esc_swept` when
        /// any was, `trouble` then saying so.
        bool esc_swept = false;
        int  esc_presses = 0;
        /// The run gave its hold back for the macro's HP potion, which it could
        /// not let through (set_yield, 2026.11.21): a stop for HP, put off
        /// kHpStoppedAgainMs, no trouble of the job's own.
        bool yielded = false;
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

    /// The trash worker's sweep of a hold it keeps (2026.11.21, the audit's
    /// B1; bag::kept_sweep_due): esc_out on its own, the step taken to be
    /// kStuckMs old -- ESC on whatever a fresh look shows up: the check-in
    /// board, the quantity box under it, the shop and the bag; in a Pass the
    /// bin's question its Hủy Bỏ first, and -- nobody else may answer it under
    /// this hold -- another's two-button dialog its ESC too (a trade, a
    /// friend's request). Under the guard and the hold check, as esc_out ever
    /// is: never with the character down or the hold another job's; and by
    /// esc_out's night rule for the board alone. True when the screen is clear
    /// after it; whoever kept the hold looks again for itself before letting
    /// it go.
    bool sweep_kept(Result& res, const char* where);

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
    /// run, but for what a charm run hands to the next (Result::notice_left).
    Result run_pass();

    /// Hủy Bỏ on every dialog with the two buttons, the bin's first, until
    /// none is left or `tries` run out -- whoever's they are, since Hủy Bỏ
    /// never destroys anything. What a probe answers with; true when none is
    /// left.
    bool cancel_all(int tries);

    bool grab_view(Frame& f, BagView& v);
    /// A look at the game through the grab it was made with -- what every
    /// look of a run is, by way of grab_: a black frame is looked at again, a
    /// moment apart, for up to kBlackLooks looks, and one black still is no
    /// look at all: false, `f` left empty, as on every false. A frame black
    /// where the game is shows nothing of what is up, and was taken on the live
    /// client for a quantity box gone (2026-10-30). One stretch is waited out
    /// once: after it, black gives up at once until a look is not black.
    bool see(Frame& f);
    static constexpr int kBlackLooks = 20;
    static constexpr int kBlackGapMs = 200;

    /// The screen is in a state to start on an item: the bag found, the can
    /// in the bin and no dialog with the two buttons anywhere.
    bool clean(const Frame& f, const BagView& v);
    /// The same with find_dialogs' result on the frame in hand (`up`): a pass
    /// searches a fresh look once, for this and the key window (2026.11.10).
    static bool clean(const BagView& v, const std::vector<bag::Dialog>& up);
    /// Whether `v` lets a look be clean at all -- the bag found, nothing in
    /// the bin -- before its dialogs are searched for.
    static bool bin_clear(const BagView& v) { return v.found && (v.bin_empty || !v.bin_on_frame); }

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
    /// Gone, its last look -- the item seen gone on it, nothing up at all --
    /// goes to `after` and `after_view` when given, `seen_clean` set: the
    /// pass's next round judges on it rather than on a look of its own. Not
    /// on anything short of that (2026.11.10).
    bool trash_one(const Frame& f, const BagView& v, int index, int count, Frame* after = nullptr,
                   BagView* after_view = nullptr, bool* seen_clean = nullptr);

    /// How long each step of the last trash_one took, for its line in the log
    /// (2026.11.10), in ms on now()'s clock; -1 for a step not come to. The
    /// drag; from it to the item seen in the bin, and in how many looks; the
    /// bin's click to its dialog; the box watched, and how many looks read it;
    /// each Xác Nhận to its dialog seen closed; the wait for another question
    /// after the last, and whether the item seen gone ended it early; the
    /// last look; and the whole.
    struct ItemTimes {
        int  drag = -1, in_bin = -1, in_bin_looks = 0, dialog = -1, box = -1, box_reads = 0;
        int  closed = -1, questions = 0, after = -1, check = -1, total = -1;
        bool early = false;
    };
    ItemTimes item_times_;
    /// The line item_times_ makes, for the item at `where`, `gone` or not.
    std::string item_times_line(const std::string& where, bool gone) const;

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
        /// A message with only Xác Nhận right after the right-click, with no
        /// question: never clicked, left for the player. A charm's own message
        /// after its question's Xác Nhận is no Notice: it is closed
        /// (close_notice), and the stack says how the use came out.
        Notice,
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

        // A charm (charm_one) only.
        /// The question's pair, set once its Xác Nhận is pressed -- and when.
        ScreenRect  asked;
        std::chrono::steady_clock::time_point asked_at{};
        /// The game's message after it: seen and closed with its own Xác Nhận;
        /// or seen and left up -- two clicks did not close it (`notice_stuck`:
        /// not clicked again by this run, handed to the next since 2026.11.24),
        /// the mouse could not be had there, or a stop came first.
        bool        notice_closed = false;
        bool        notice_left = false;
        bool        notice_stuck = false;
        int         notice_ms = -1;     ///< from the Xác Nhận to the message first seen
        int         notice_clicks = 0;
        /// The guard said no, or a stop came, while it waited: nothing more
        /// was clicked, and the run is to stop.
        bool        stopped = false;
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
    /// `charm`, for a charm's slot: the catalog icon of its colour, empty when
    /// there is none, and the count read as the charm sweep read it
    /// (CountReader::read_charm) -- a stack of one only while the slot still
    /// shows that very icon with nothing over its corner. Null for anything
    /// else, read as ever (CountReader::read).
    enum class SlotNow {
        Gone,       ///< empty
        Less,       ///< the same drawing, one less than `count`
        Same,       ///< the same drawing, still `count`
        Asking,     ///< a dialog with two buttons is up, which nobody here asked for
        Unknown,    ///< anything else; `why` says what, in Vietnamese
    };
    SlotNow slot_now(const BagView& v, int index, const Frame& before, int count, Frame& look,
                     Frame& shot, std::string& why, int gap_ms = 150, const Frame* charm = nullptr);

    /// A charm: one right-click on the slot, then what the game asks. Its
    /// question is confirmed with Xác Nhận only when bag::charm_question says
    /// so on a fresh look the moment before, with nothing else up and no other
    /// window open -- else Hủy Bỏ -- and counts only when the slot is seen
    /// empty or one less, the same on two looks. A right-click that brings no
    /// question and takes the stack down is not taken for a use: it may be a
    /// shop's or a storage window's doing (UsedUnasked), and the run stops.
    /// Quicker than use_one all the way: it watches in short steps, has no
    /// monster book to look for or shut, and looks at the slot again after
    /// charm_gap_ms rather than 150. `count` is the stack count read off the
    /// slot, by CountReader::read_charm with `icon`, the catalog icon of its
    /// colour (empty when there is none): the slot is read again the same way.
    /// A stack of one counts only seen empty; still that icon with nothing
    /// over it is the stack as it was, and anything else no answer.
    ///
    /// After Xác Nhận the game says the charm worked in a message with only
    /// Xác Nhận under it, where the question's Hủy Bỏ was: watched for, and
    /// closed with that Xác Nhận (close_notice) before the stack is looked at.
    /// A message right after the right-click, with no question, is never
    /// clicked. The pointer is moved off the buttons after every click, so
    /// that none of them is lit for the next look. No key is ever pressed.
    UseResult charm_one(const Frame& f, const BagView& v, int index, int count, const Frame& icon);

    /// How close_notice came out. Closed: seen gone on two looks, after a
    /// click or with none needed. NotClosed: still up after two clicks.
    /// Dialog: a dialog of two buttons came up. NotClicked: the mouse could
    /// not be had, or the screen not seen. Stopped: the guard said no, or a
    /// stop came, before a click.
    enum class NoticeClose { Closed, NotClosed, Dialog, NotClicked, Stopped };
    /// Close a message with its own Xác Nhận, its label looked for in `band`
    /// (bag::lone_ok): on a fresh look before each click, the pointer moved
    /// off `box` after it. Once more, a pixel aside and no sooner than
    /// kNoticeRetryMs after the first, when it is still up; no third time, and
    /// no key ever. `shot` gets the last look; `clicks` how many were made.
    /// `quick` (2026.11.24 review, set_own_notice's clicked_out): one click,
    /// watched kNoticeCloseMs x 2.
    NoticeClose close_notice(const ScreenRect& band, const ScreenRect& box, Frame& shot, int& clicks,
                             bool quick = false);
    /// GoBot's own message on `f`: a lone Xác Nhận where the last Xác Nhận
    /// pressed on a charm question puts one, that Xác Nhận pressed less than
    /// kOwnNoticeMs ago -- or handed over by set_own_notice -- and not one this
    /// run has clicked to no end already (`last_clicked_out_`, 2026.11.24: the
    /// next run's to click). `at` gets its label.
    bool own_message(const Frame& f, ScreenRect* at = nullptr);
    /// own_message closed, said as `when` says: 1 when one was up and is
    /// closed, 0 when none is (one handed over and not up is gone), -1 when it
    /// would not close -- `res.trouble` then says why, and nothing more is to
    /// be pressed.
    int  close_own(const Frame& f, Result& res, const char* when);
    /// What a close_notice that did not close says, the picture kept and a
    /// dialog answered Hủy Bỏ: `res.trouble`. `clicks`, how many it made: a
    /// message still up after them is not clicked again by this run, and is
    /// handed to the next (2026.11.24; until then it was the player's from
    /// then on).
    void notice_trouble(NoticeClose how, const Frame& shot, Result& res, int clicks);
    /// The end of a charm run, however the sweep ended but by a stop: the
    /// message of the last Xác Nhận not yet seen closed, watched for until
    /// kCharmNoticeMs and a little after that Xác Nhận, and closed.
    void finish_charms(Result& res);
    /// The first look of a charm run, before the bag key or a tab: GoBot's
    /// own message from the last run closed (close_own); and with the bag
    /// shut, its key not pressed under anyone else's. False, with what to say
    /// said, when the run ends here -- also with the bag shut and no charm due,
    /// a message to close having been all it was for.
    bool charms_start(Frame& f, BagView& v, Result& res);
    /// Whether any charm on the Dùng list is due (set_charm_due).
    bool charm_due_any() const;

    /// The charm sweep of tab `tab` -- Tiêu Dùng -- for Mode::CharmsOnly: every
    /// slot judged a charm on the Dùng list that is due, once each, in the
    /// order they lie, at most kMaxCharms a run. `res.charm_tries` gets each.
    /// Each kind once a run, by its clock id and by its colour; and done once
    /// every kind due has been tried, the pages below not looked at. Nothing is
    /// right-clicked with another of the game's windows open, nor with a
    /// message of the game's up -- GoBot's own is closed first -- nor with the
    /// guard saying no, nor past the deadline; nor a slot of another colour or
    /// of a colour not sure (TrashList::judge), nor one whose count does not
    /// read -- a stack of one reads as one only by its bare icon
    /// (CountReader::read_charm) -- nor one the packets' bag says holds
    /// something else, or, for a stack of one, another count
    /// (set_charm_packets). A stop after the first thing that is not as
    /// expected: nothing more is right-clicked.
    void charm_sweep(int tab, BagView& v, Result& res, bool& stop);
    static constexpr int kMaxCharms = 10;

    /// The guard says the hold is gone, or the deadline has passed: said and
    /// `stop` set. False when the run may go on.
    bool charm_must_stop(Result& res, bool& stop);

    /// Mode::ShopBuy, after the bag has been found: open the shop -- a
    /// right-click on the Người Giữ Sân item of Khác -- and make the purchases
    /// set_shop_plan asks for, in bag::shop_order's order, 100 at a time, the
    /// number typed into the quantity box and read back before Xác Nhận; then
    /// close the shop (ESC, seen to work, else its Thoát), the full-bag message
    /// closed first if it is up. Nothing in the bag is counted, and Tiêu Dùng
    /// is never clicked (the owner, 2026-11-09). The game's message that the
    /// bag is full, another message instead of the box, a purchase not seen
    /// through, the check-in board, or anything else not as expected ends the
    /// round, with Hủy Bỏ on whatever is up and a picture kept.
    void shop_buy(BagView& v, Result& res, bool& stop);
    /// The page on show with its scroll bar read: the pointer is moved off the
    /// bar and the page looked at again a few times when the thumb is not
    /// readable. False when the bag is not found at all.
    bool look_page(Frame& f, BagView& v);
    /// One click on the scroll bar's arrow -- down, or up -- that moved the
    /// page, tried again a pixel aside and waited for longer when it did not,
    /// up to four times. `v` is the page after. False when it never moved.
    bool scroll_arrow(bool down, BagView& v);
    /// The use sweep's and the charm sweep's one click on the down arrow,
    /// just made over `f` and `v`: true when a look shows the page moved --
    /// within react_ms() while the scroll bar says it is not at the bottom
    /// (2026.11.15d), else on one look 250 ms on as ever -- or when the last
    /// look found no bag, which the sweep's next round says; false when it did
    /// not move, or on a stop or the guards' no. `where` is for the log.
    bool scroll_watch(const Frame& f, const BagView& v, const char* where);
    /// Click tab `tab` and look until it is the one chosen, the pointer moved
    /// off the strip after the click: the game lights what lies under it. A
    /// strip that reads no tab chosen is looked at again (kTabRelooks); one
    /// that reads another, clicked once more. False with `res.trouble`, and
    /// with `res.tab_unsure` when the strip was there and never read as `tab`.
    /// `where` names the caller in the log and in the picture kept. At most
    /// `most_clicks` clicks: the shop's kShopTabClicks, two for the rest.
    bool select_tab(int tab, BagView& v, Result& res, const char* where, int most_clicks = 2);
    /// A shop round's bag before anything else (2026.11.14b, the owner: "trước
    /// khi thực hiện xử lý mua HPMP cần check xem túi đồ có đang mở không, nếu
    /// có thì thực hiện luôn, còn không thì mở túi đồ lên"): `f` and `v`, the
    /// round's first look, are looked at again kBagSettleMs later. Seen on
    /// both, the bag is open and nothing is pressed; not seen on both, its key
    /// once -- never within kBagSettleMs of a look that saw it -- and then
    /// looked for until it is seen, and once more kBagSettleMs later; looks
    /// that differ are looked at again until two in a row agree. False, with
    /// `res.trouble`, when the bag is not to be had: no second key.
    bool shop_bag_ready(Frame& f, BagView& v, Result& res, bool& opened_here, int& open_ms, int& open_looks);
    /// The stuck rule's 30 s (kStuckMs, react_ms(); 10 s before 2026.11.15d):
    /// from the step's last input of GoBot's own -- at once when it is
    /// older -- ESC on every fresh look that shows something up -- the
    /// check-in board, a message, a dialog or the quantity question, the shop,
    /// the bag -- until nothing is (kEscClearLooks looks), kEscMax at most. In
    /// a pass the bin's question, when seen, gets its Hủy Bỏ first. Never in a
    /// charm run, nor in a pass that only looks, nor after a stop, a key of
    /// the macro's late, the hold lost or the guard's no (the character down),
    /// nor on a look that shows nothing or none at all; once a run. True when
    /// the screen is clear after it (with or without an ESC): `res` then says
    /// what was pressed, and the question and the shop are no longer taken to
    /// be up. Whoever called it looks again for what is still up whenever it
    /// ran (esc_tried_), whatever it returned.
    bool esc_out(Result& res, const char* where);
    /// The step's last input, the stuck rule's 30 s are counted from: see
    /// kStuckMs.
    std::chrono::steady_clock::time_point step_from_{};
    bool step_on_ = false;
    bool esc_tried_ = false;
    void step_begin() {
        step_from_ = now();
        step_on_ = true;
    }
    /// An input of the step's own after its start (2026.11.15d): the stuck
    /// rule's kReactMs counted again from it -- only while a step is on.
    void step_again() {
        if (step_on_) step_from_ = now();
    }
    bool step_over() const { return step_on_ && now() - step_from_ >= std::chrono::milliseconds(react_ms_); }
    /// The strip not read as `tab` where a run stops on it: the tab read,
    /// where the strip is, how alike, and every tab's mean grey in the log;
    /// and the screen kept the first time at each `where` this GoBot start
    /// (keep holds only the newest few dozen, and these are not to push the
    /// rest out).
    void tab_unsure(const Frame& f, const BagView& v, int tab, const char* where);
    /// The strip read as `tab` after `looks` more looks; `first` is the first
    /// look that read no tab chosen. For the log.
    void tab_read_late(const BagView& first, int tab, int looks, const char* where);
    /// find_dialogs, without the shop's own Mua | Thoát buttons, which a pair
    /// of buttons can pass for; and Hủy Bỏ on what is left, as cancel_all.
    std::vector<bag::Dialog> dialogs_here(const Frame& f, const TemplateStore& store) const;
    /// dialogs_here's filter alone, over find_dialogs' result in hand.
    std::vector<bag::Dialog> shop_filter(const std::vector<bag::Dialog>& all) const;
    bool shop_cancel(int tries);
    /// Where the shop's buttons are while it is open: set by shop_open and
    /// every purchase, empty when it is not.
    ScreenRect shop_buttons_;
    /// How far under the shop's row its buttons were when both were last seen.
    int shop_row_dy_ = bag::kShopButtonsDy;
    /// The shop's quantity question's two buttons while it is taken to be up:
    /// set when it is recognised, emptied when a look shows it gone.
    ScreenRect shop_q_;
    /// The run's purchases type as scan codes, clicked into the box first: the
    /// way that put the number in the box once the plain way had not.
    bool shop_scan_ = false;
    /// A notice that two clicks on its Xác Nhận did not close (shop_notice) is
    /// the player's from then on in this run: not clicked a third time, as the
    /// full-bag message is not (Result::full_stuck).
    bool shop_notice_stuck_ = false;
    /// Whether `f` shows that question still where it was: its Xác Nhận where
    /// it was. Its Hủy Bỏ is not asked -- on the live client (2026-10-31) the
    /// box drew it pressed after keys it did not take, and the pair as a whole
    /// then matched at 0.69, so find_dialogs saw no dialog at all.
    bool question_still(const Frame& f) const;
    /// Whether the game itself is on top over `r` -- its middle and two corners
    /// -- so that a copy of the screen there shows the game's. Nothing is taken
    /// for gone where it is not: the shop's frames come off the screen only.
    bool seen_at(const ScreenRect& r);
    /// Whether the check-in board on `f` lies over the question taken to be up
    /// (shop_q_, its whole box): a question it may hide is not gone, and what
    /// was typed into it is still there.
    bool board_over_question(const Frame& f) const;
    /// The number in the box of the dialog at `pair`, looked at over a whole
    /// blink of its caret: the number every look agrees on, kBoxMixed,
    /// kUnreadable, or kBoxGone when the dialog was no longer where it was.
    int  shop_box(const ScreenRect& pair, Frame& last, int min_ms = 0);
    int  shop_box_once(const ScreenRect& pair, Frame& last, int min_ms = 0);
    /// Open the shop: a right-click on the opener, which has to be on the tab
    /// on show, confidently, with no other window open, and the shop seen open
    /// -- its row and buttons -- within a few seconds. False with `res.trouble`.
    /// `clicked`: the item was right-clicked -- a shop may come even when this
    /// gave up waiting for it, and is then waited for once more and shut.
    bool shop_open(BagView& v, Result& res, Frame& shot, bag::Shop& shop, bool& clicked);
    /// One purchase of potion `hp` (else MP): on a fresh look with the shop up
    /// and nothing else -- no dialog, no message, no check-in board -- its cell
    /// double-clicked -- never Mua, which brings up a box that takes no keys
    /// (the player's own way, 2026-10-31) -- the quantity question seen, 100
    /// typed and read back, Xác Nhận on a fresh look with only that question
    /// up, and the question seen gone with no message after it. kShopBought;
    /// kShopFullBag when the game said no room, instead of the box or after
    /// Xác Nhận -- that one not counted -- the message closed (shop_full_close);
    /// kShopMessage for a message GoBot does not know (lone_message), closed;
    /// kShopUnsure when Xác Nhận was clicked and what came of it was not seen;
    /// kShopNoticed when a message GoBot does not know came up after Xác Nhận
    /// and was closed by its own Xác Nhận (2026.11.11c): counted as bought, as
    /// an unsure one is, and the round goes on;
    /// kShopBoard with the check-in board up, nothing clicked from then on; or
    /// kShopStopped with `res.trouble`. Dry: stops at the question, which is
    /// answered Hủy Bỏ (kShopProbed).
    enum {
        kShopStopped = 0,
        kShopBought = 1,
        kShopFullBag = 2,
        kShopProbed = 3,
        kShopMessage = 4,
        kShopUnsure = 5,
        kShopBoard = 6,
        kShopNoticed = 7,
        /// The quantity question up kStuckMs and the number not read back
        /// as 100, before Xác Nhận (2026.11.14b): nothing bought, no Hủy Bỏ
        /// more -- the round ends with esc_out.
        kShopStuck = 8,
    };
    /// Notices closed after Xác Nhận that one shop round puts up with: the
    /// second ends it (the purchase counted, the notice closed, `res.trouble`).
    static constexpr int kShopNoticesMax = 2;
    /// `what`: Thịt bò khô, Rượu lá tre -- by their cells in the potions' row
    /// -- or the pet's Thịt bò (2026.11.24): on the Linh Thú tab, which
    /// shop_tab has chosen, found by its name on the very look the double-click
    /// goes by (bag::shop_item) -- the tab still drawn chosen -- in place of the
    /// potions' row, looked at again kShopRowRelooks times and then given up
    /// with its picture kept (pet_no_item, pet_item_place, pet_tab). The rest
    /// is the potions' own: the money stretch, the quantity question, 100 typed
    /// and read back, Xác Nhận, what came after it.
    int  shop_purchase(bag::ShopPotion what, Result& res);
    /// The shop's tab `tab` chosen (2026.11.24): on a fresh look with the shop
    /// up and nothing over it -- no dialog, no message, no check-in board --
    /// and its strip of tabs where its buttons put it (bag::shop_tabs), the
    /// tab's middle clicked, the pointer moved off at once, and the tab watched
    /// for react_ms() until two looks in a row draw it chosen -- for Linh Thú
    /// with the potions' row gone from the list, for Dược Phẩm with it back. A
    /// second click a pixel aside when the first brought nothing, no third.
    /// Chosen already: nothing clicked. False with `res.trouble` and the
    /// picture kept (pet_tab, shop_tab), or with `res.board_up`.
    bool shop_tab(int tab, Result& res);
    static constexpr int kShopTabClicksMost = 2;
    /// The shop on `f`: bag::find_shop, or -- with its first row hidden -- its
    /// buttons alone, where they were last seen (row_score 0): a shop still
    /// open, for closing it and for the run's end; never what a double-click
    /// goes by (shop_purchase's judge wants the row itself, 2026.11.11).
    bag::Shop shop_now(const Frame& f) const;
    /// Close the shop with ESC -- looked at, and its Thoát clicked when it is
    /// still there -- and nothing else pressed. False when it is still open.
    bool shop_close(const bag::Shop& shop, Result& res);
    /// Close the full-bag message found at `at` by its own Xác Nhận, with a
    /// loop of its own: gone on a fresh look and on another kShopFullGapMs
    /// later is closed with no click; else its label is clicked where lone_ok
    /// finds it in kShopFullOkBand -- or, the label not to be told (lit by the
    /// pointer), at the picture's own place for it, the message still up on
    /// that fresh look -- and it is closed only once two looks kShopFullGapMs
    /// apart do not show it, within kShopFullCloseMs; once more on a fresh
    /// look no sooner than kShopFullGapMs after the first click, and no
    /// third time. Still up: `res.full_stuck`, said, and never an ESC -- an ESC
    /// would close the message the round is ended by, or the shop under it.
    bool shop_full_close(const ScreenRect& at, Result& res);
    static constexpr int kShopFullGapMs = 300;
    static constexpr int kShopFullCloseMs = 2500;
    /// Looks after the question is seen gone, kShopFullTailGapMs apart, for a
    /// full-bag message that comes a moment after it: kShopFullTailLooks at
    /// least, and on until kShopFullLateMs after Xác Nhận was clicked.
    static constexpr int kShopFullTailLooks = 2;
    static constexpr int kShopFullTailGapMs = 150;
    static constexpr int kShopFullLateMs = 1000;
    /// A message GoBot does not know -- one Xác Nhận under it, lone_ok
    /// anywhere -- and nothing else: not the full-bag message, no dialog of
    /// two buttons, not the quantity question taken to be up. On `f` and on a
    /// fresh look kShopTailGap later both, so a message caught for one frame
    /// is none. `at` gets its label; `shot`, when given, the second look.
    bool lone_message(const Frame& f, ScreenRect* at, Frame* shot = nullptr);
    /// That message closed by its own Xác Nhận (close_notice), its picture kept
    /// as shop_message first, and the full-bag picture's scores on it logged
    /// `where`: `res.shop_message`, and kShopMessage. A message that would not
    /// close is said too: nothing more is pressed for it.
    int  shop_message(const Frame& f, const ScreenRect& label, Result& res, const char* where);
    /// A message GoBot does not know that came up after Xác Nhận, or that
    /// stands before the shop's ESC (2026.11.11c, the owner: "cần sửa để bot
    /// tự tắt thông báo này"): closed by its own Xác Nhận as a charm's message
    /// is (close_notice), not left for the player. Asked of `f` through
    /// lone_message first -- a second look a moment later, no dialog, not the
    /// full-bag message, not a question drawn with Hủy Bỏ pressed -- so that
    /// what is not surely a lone message is never clicked. 1 closed; 0 not a
    /// lone message (nothing clicked); -1 would not close, `why` says so.
    int  shop_notice(const Frame& f, const char* where, std::string& why);
    /// The check-in board on `f` (checkin_board_title): logged and said once a
    /// run, `res.board_up`. Nothing is clicked where it is.
    bool board_up(const Frame& f, Result& res);
    /// The pointer moved to x, y with no button pressed: with pretend hands
    /// through Hands::move -- nothing when unset -- and the game's own pointer
    /// otherwise. Never a click.
    void move_to(int x, int y);
    /// What set_shop_plan asked for.
    int shop_plan_hp_ = 0, shop_plan_mp_ = 0, shop_plan_pet_ = 0;
    /// The shop's tab as this run last chose or saw it (2026.11.24): Dược
    /// Phẩm as a shop opens, Linh Thú once shop_tab has chosen it.
    int shop_tab_ = bag::kShopTabPotions;
    /// The check-in board has been said this run.
    bool board_said_ = false;

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
    /// `done`, when given, says from a look that the item is gone -- its slot
    /// empty, the can back, nothing up -- which ends the wait for a question
    /// after Xác Nhận early (answered_early_).
    int  answer(const bag::Dialog& first, int count, const std::function<bool(const Frame&)>& done = {});
    bool answered_early_ = false;

    /// What the box of the bin's quantity dialog `d` holds, looked at again
    /// and again for a whole blink of its caret: the number every look that
    /// read one agrees on, two looks at least. kUnreadable when fewer read
    /// one, kBoxMixed when two read different numbers, kBoxGone when the
    /// dialog was no longer the bin's, where it was. `last` gets the last
    /// frame looked at.
    static constexpr int kBoxMixed = -2;
    static constexpr int kBoxGone = -3;
    /// `reads`, when given, gets how many looks read a number.
    int  box_holds(const bag::Dialog& d, Frame& last, int* reads = nullptr);

    /// A dialog of the bin's on screen within `ms`, or one with kind -1 and
    /// an empty pair. `foreign` is set when another dialog came up instead --
    /// on two looks at least kDoneApartMs apart -- and `last` gets the last
    /// frame looked at, to keep when that is wrong. Waits up to `ms` by the
    /// clock, looking every 80 ms (2026.11.10; 150 before), for one of the
    /// bin's dialogs; `done`, when given and true on two looks at least
    /// kDoneApartMs apart with nothing up, ends the wait early with no
    /// dialog, and answered_early_ says so.
    bag::Dialog wait_for_dialog(int ms, bool& foreign, Frame* last = nullptr,
                                const std::function<bool(const Frame&)>& done = {});
    /// The pointer moved off the bag's slots and buttons -- right of the bag,
    /// else above it, else left of it, the first of these inside the game's
    /// window, clear of a shop open beside the bag and at least 24 pixels off
    /// `avoid` -- so no item's description lies over the grid, the bin or a
    /// dialog, and no button of a box at `avoid` is lit. A move, never a
    /// click. With pretend hands, through Hands::move and the size of the last
    /// look; nothing when that is unset.
    void park_pointer(bool title_first = false, const ScreenRect* avoid = nullptr);
    /// Where the bag's strip was on the last look that found it, and the size
    /// of the last look.
    ScreenRect last_strip_;
    int last_w_ = 0, last_h_ = 0;

    void keep(const Frame& f, const char* what);
    void scroll_to_top(BagView& v);
    void say(const std::string& line);
    bool wait_ms(int ms);           ///< false when cancelled
    void pause_ms(int ms);          ///< whether cancelled or not

    // ---- waiting for the game to react (2026.11.15d, react_wait.hpp) ----

    /// One gap of a wait for the game: `ms` waited through wait_ms -- a stop
    /// ends it -- and then the job's guards asked (react_stop). False, and the
    /// wait is over, on either.
    bool react_gap(int ms);
    /// A stop, a key of the macro's late, or the guards' no: the hold no
    /// longer the job's (set_hold_check), or the guard's (set_guard) -- the
    /// character down, HP under the job's level. A guard's no ends the wait
    /// as nothing seen would, the step given up as it always was; the guard,
    /// asked between steps as ever, stops the run at the next, and its
    /// trouble says why (guard_stopped_). react_halted_: the last ask said so.
    /// Inside a ReactAfter scope the no still ends the wait, and is not
    /// counted as a stop (see ReactAfter).
    bool react_stop();
    bool react_halted_ = false;
    /// The waits that come after the work, or give back what a stop or a
    /// problem left (the review of 2026.11.15d): the bag shut at a run's end
    /// (close_bag), the shop's ESC and Thoát (shop_close), the Hủy Bỏ watches
    /// (back_out, cancel_all, shop_cancel). A guard's or the hold's no there
    /// still shortens the wait -- after the no, as long as it ever was -- but
    /// does not set guard_stopped_: nothing the run was doing is cut short by
    /// it, and a pass that deleted everything and dipped under its HP level
    /// while its bag shut is no pass "stopped mid-way" -- that would put the
    /// pass on again a few seconds later and refuse the shop its ride. A run
    /// still being worked through hears the guard between steps, as ever.
    struct ReactAfter {
        BagCleaner& c;
        bool was;
        explicit ReactAfter(BagCleaner& cleaner) : c(cleaner), was(cleaner.react_after_) { c.react_after_ = true; }
        ~ReactAfter() { c.react_after_ = was; }
        ReactAfter(const ReactAfter&) = delete;
        ReactAfter& operator=(const ReactAfter&) = delete;
    };
    bool react_after_ = false;
    /// What took the game `ms` to show, from the input: said in the log when
    /// it is past kReactSlowMs ("the game took N ms to show <what>"), and a
    /// run's deadline moved on by what it was past that. `what` in English.
    /// `credited_ms`: how much of those `ms`, from the same input, an earlier
    /// wait has already moved the deadline on by (the charm's question gone,
    /// before its stack is read) -- only what is past it moves it again, so
    /// that two waits measured from one Xác Nhận are not counted twice.
    void react_took(int ms, const char* what, int credited_ms = 0);
    /// Nothing shown react_ms() after the input: "nothing after 30 s" in the
    /// log, the deadline moved on as react_took moves it, `credited_ms` the
    /// same.
    void react_none(const char* what, int credited_ms = 0);
    /// Inside a wait for the game, on `f` just looked at: an HP or MP potion
    /// due given its moment (potion_break) every kReactPotionMs of the wait
    /// from `from` -- only through potion_break's own checks of the look --
    /// and the key window shut again after it, so that what the game brings
    /// up next takes no key.
    void react_potion(const Frame& f, const BagView& v, const char* where,
                      std::chrono::steady_clock::time_point from);
    /// The " sau 30 giây" a give-up of such a wait is said with, and its "30
    /// giây": react_ms() in whole seconds.
    std::string react_said_vi() const;
    std::string react_secs_vi() const;
    int  react_ms_ = kReactMs;
    bool guard_stopped_ = false;
    std::chrono::steady_clock::time_point react_potion_at_{};
    std::function<int()> day_clock_;
    // Through the clock when it is set, else the steady clock and a sleep.
    std::chrono::steady_clock::time_point now() const;
    void sleep(int ms);

    // Through the hands when they are set, else the game's input. A click on
    // the game goes only where the game is on top as the button goes down,
    // and only with the game in front unless `need_front` is false -- for
    // giving an item back, as with mouse_ok. That one goes even once
    // cancelled, being what a stop needs; any other does not.
    bool click(int x, int y, bool need_front = true);
    /// A double-click on the game, as click: see InputManager::click_on_game.
    bool double_click(int x, int y);
    bool drag(int fx, int fy, int tx, int ty);
    bool press(std::uint32_t vk);
    /// A key as its scan code alone: see Hands::scan_key.
    bool press_scan(std::uint32_t vk);
    bool right_click(int x, int y);
    /// The game's own click, for click and right_click: says why not when it
    /// is not made.
    bool click_game(int x, int y, bool right_button, bool need_front, int presses = 1);
    /// Why a click or a drag on the game was not made, said as the trash
    /// screen says it.
    void say_not_made(InputManager::GameClick why, int x, int y);

    // ---- the key window (set_key_window, 2026.11.10) ----

    /// Before every input of the run's own: the window shut and the keys let
    /// through seen up -- and, one of the six no letter among them, until
    /// kNavSettleMs after it came up (KeyWindow::nav_ago_ms), so the input and
    /// any look taken after this go after whatever the game did with it.
    /// False -- one still down after kKeysUpWaitMs -- and the input is not
    /// made: the run is stopped (keys_late_), and each input after it asks
    /// again.
    bool keys_shut();
    /// Whether the window may be open on `f`, a look just taken: nothing up on
    /// it and nothing out (see KeyWindow), in a pass `v` clean as well; not in
    /// a purchase's money stretch, a stop or a charm run. What the caller has
    /// already found on `f` is not searched for again: `up`, find_dialogs'
    /// result as it gave it; `lone`, lone_ok over the whole frame (1 up, 0
    /// not, -1 not asked); `full_asked`, shop_full_up asked and not up. Every
    /// check is made all the same, on the same frame.
    bool keys_may_open(const Frame& f, const BagView& v, const std::vector<bag::Dialog>* up = nullptr, int lone = -1,
                       bool full_asked = false);
    /// keys_may_open's checks with no window asked for (2026.11.11b): what an
    /// own potion press goes by as well -- a charm run's too since 2026.11.12b,
    /// with no window set, no dialog on the look, and a right-click that
    /// brought nothing up not holding it (late_ask_ is the pass's alone here).
    bool keys_clear(const Frame& f, const BagView& v, const std::vector<bag::Dialog>* up, int lone, bool full_asked);
    /// An HP or MP potion the run presses itself (OwnPotions), on `f` when
    /// keys_clear says so of it; true when one went down.
    bool press_own_potion(const Frame& f, const BagView& v, const char* where, const std::vector<bag::Dialog>* up,
                          int lone, bool full_asked);
    /// The window opened on `f` when keys_may_open says it may -- `v`, `up`,
    /// `lone` and `full_asked` as it takes them -- unless it is open already.
    void keys_open(const Frame& f, const BagView& v, const std::vector<bag::Dialog>* up = nullptr, int lone = -1,
                   bool full_asked = false);
    /// keys_shut, and then kPotionSettleMs since the last potion let through
    /// went down: what the bag may still change for has come. Before a look a
    /// slot is dragged or right-clicked from.
    bool keys_settle();
    /// An HP or MP potion due (KeyWindow::potion_due): the window opened on
    /// `f` through keys_open -- `v`, `up`, `lone` and `full_asked` as it takes
    /// them -- and kept open while one still is, kPotionBreakMs at most. Open
    /// already -- since a purchase's tail, say -- it is kept open for one only
    /// when keys_may_open says so of `f` (2026.11.11, the review's KEYS-1),
    /// and otherwise left as it is for the caller's own shut. `asked`: the
    /// caller asked keys_open of `f` just now, which is not asked again. True
    /// when it was open for one; nothing at all with none due, or with the
    /// window refused -- the money stretch, anything up, a charm run.
    bool potion_break(const Frame& f, const BagView& v, const char* where, const std::vector<bag::Dialog>* up = nullptr,
                      int lone = -1, bool full_asked = false, bool asked = false);
    /// Whether a look taken at `at`, the window shut through it (`shut`), may
    /// be dragged from: kPotionSettleMs after the last potion let through.
    bool drag_grade(std::chrono::steady_clock::time_point at, bool shut) const;
    KeyWindow keys_;
    OwnPotions own_potions_;
    /// The run's own last potion press (2026.11.11b): no drag from a look
    /// begun less than kPotionSettleMs after it; and a purchase's double-click
    /// goes by a look taken after it (own_pressed_).
    std::chrono::steady_clock::time_point own_potion_at_{};
    bool own_potion_any_ = false;
    bool own_pressed_ = false;
    /// The window opened by this run and not shut since.
    bool keys_out_ = false;
    /// A shut whose keys were not seen up: the run stops there.
    bool keys_late_ = false;
    /// A shop's purchase from just before its double-click to the end of its
    /// tail looks: the window never opens.
    bool money_ = false;
    /// The last look: when it was begun, and whether the window was shut
    /// through it (see()).
    std::chrono::steady_clock::time_point look_at_{};
    bool look_shut_ = true;

    InputManager&        input_;
    const TemplateStore& store_;
    Grab                 raw_grab_;     ///< the grab as given
    Grab                 grab_;         ///< see(), or empty with no grab given
    bool                 black_said_ = false;
    bool                 blacked_out_ = false;  ///< the last look gave up on a whole black stretch
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
    bool                 left_held_ = false;    ///< see set_left_held
    Mode                 mode_ = Mode::Pass;
    std::function<bool(const std::string&)> charm_due_;
    std::function<bool()> guard_;
    std::function<bool()> hold_check_;     ///< set_hold_check
    std::function<bool()> yield_;          ///< set_yield
    /// Asked at the guard's safe points (set_yield): true, and `res` says the
    /// run gave its hold back for the macro's HP potion.
    bool yield_now(Result& res);
    /// esc_out run by sweep_kept: in a Pass another's dialog gets its ESC too.
    bool kept_sweep_ = false;
    std::function<void(const std::string&)> charm_claim_;
    std::function<PacketSays(const std::string&, int, int, std::string&)> charm_packets_;
    /// The last Xác Nhận pressed on a charm question: its pair and when --
    /// cleared by a charm_one that pressed none -- or the one handed over by
    /// set_own_notice (`last_carried_`); and whether its message has been seen
    /// closed or gone. Its message clicked by this run and still up
    /// (`last_clicked_`), and clicked to no end -- two clicks, or one whose
    /// label could not be told again -- so not clicked again by this run
    /// (`last_clicked_out_`): handed to the next run all the same
    /// (Result::notice_left, notice_clicked). 2026.11.24: until then such a
    /// message was given up on (`last_gave_up_`), the player's from then on.
    ScreenRect           last_asked_;
    std::chrono::steady_clock::time_point last_asked_at_{};
    bool                 last_carried_ = false;
    bool                 last_carried_out_ = false;   ///< handed over clicked to no end (set_own_notice)
    bool                 last_closed_ = false;
    bool                 last_clicked_ = false;
    bool                 last_clicked_out_ = false;
    /// The deadline of a run, on now()'s clock; unset when deadline_set_ is not.
    std::chrono::steady_clock::time_point deadline_;
    bool                 deadline_set_ = false;
};

} // namespace gobot
