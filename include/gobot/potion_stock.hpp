#pragma once

#include "gobot/bag.hpp"
#include "gobot/config.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/screen_capture.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gobot {

/// How many potions are left: the number the game prints on the hotbar slot
/// of each potion key, read the way CountReader reads a bag slot's count --
/// in the client's own digit cells, 5 pixels wide and 6 apart, a 1 too, by
/// each digit's strokes and the ring round them, so that a bright icon under
/// the count is not taken for part of it (on 2026-10-20 the boiled egg's rim
/// took the 1 off 61, 51, 41 and 21 in slot 7).
///
/// GoC has nothing like it -- it buys potions on a timer and never looks at
/// the hotbar -- so this is GoBot's own. Without it a long run ends the same
/// way every time: the stack runs out, the potion key goes on being pressed
/// at an empty slot, and the character dies.
///
/// The number on a hotbar slot is the stack in use, not everything in the
/// bag: on a saved frame slot 1 shows 27 while the bag holds 614 of the same
/// potion. So the warning says "the slot", never "the bag".
///
/// Look only. Nothing here presses, clicks or holds anything: it reads frames
/// the HP reader has already taken.

/// The hotbar: 2 rows of 9 slots along the bottom of the screen. Measured on
/// the live client at both sizes it runs at: slots 34 pixels square on a 37
/// pixel pitch across and down; slot A's top-left at 475,631 on 1280 x 720
/// and at 419,679 on 1024 x 768 -- the same height above the bottom, but not
/// the same distance from either side, so it is found rather than worked out.
namespace hotbar {

constexpr int kCols = 9;
constexpr int kRows = 2;
constexpr int kCell = 34;           ///< a slot, its own edge included
constexpr int kPitch = 37;          ///< slot to slot, across and down

/// The picture it is found by: the frame round the 18 slots, cut from a live
/// frame, with the slots themselves see-through. Slot A's top-left is this
/// far into it. By its file, which needs no name map; templates.ini names it
/// too, as hotbar_frame, so that the package keeps it.
constexpr const char* kPicture = "grabbed/hotbar_frame";
constexpr int kPictureW = 342;
constexpr int kPictureH = 82;
constexpr int kSlotDx = 6;
constexpr int kSlotDy = 6;
/// The frame scores 1.000 on every one of 1,025 saved frames where the
/// hotbar is in view, at both client sizes, and 0.55 at most where another
/// window covers it.
constexpr float kFrameScore = 0.90f;
/// Looked for in this many rows at the bottom of the frame.
constexpr int kSearchRows = 160;

/// The key labels, the letter or digit in each slot's bottom-right corner.
/// They are carried by the same picture, inside the see-through slots, at
/// this alpha: below TemplateMatcher::kOpaque, so the search does not look
/// at them, but there for read_slot to tell the slot by. Each label is its
/// white strokes and the dark outline round them, inside the slot's
/// bottom-right 10 x 10 from 24,22. Another key's label scores 0.90 at most
/// against one, the same label on another icon 1.000.
constexpr std::uint8_t kLabelAlpha = 64;
constexpr int kLabelX = 24, kLabelY = 22, kLabelW = 10, kLabelH = 10;
constexpr float kLabelScore = 0.95f;

/// The box at a slot's top-left the stack count is printed in, as
/// CountReader::read looks at it.
constexpr int kCountW = 28, kCountH = 16;
/// An empty slot is the bar's dark gradient: its brightest pixel, the label
/// corner left out, is 57 on every saved frame. An item icon is far brighter.
constexpr int kEmptyBrightest = 60;
/// The label's corner, left out of that.
constexpr int kLabelCorner = 12;
/// The bar's own frame round a slot, this many pixels of it along each side,
/// has to be as the picture has it before the slot is read: on every one of
/// the 1,026 saved frames where the bar is in view, every side of every slot
/// is within 1.5 grey levels of it on average. A window over the top of a
/// slot -- opaque, or black at a third of the way through -- is 11 or more.
constexpr int kRingBand = 3;
constexpr double kRingDiff = 6.0;
/// What lies over a slot inside its frame leaves the frame as it is, and is
/// told by the count's corner instead, when nothing white is in it: a corner
/// whose darkest and brightest grey are this close is all one colour -- a
/// window's body, not an icon -- and so is no reading. On the 1,057 saved
/// frames with the bar in view, every slot read as one potion spans 130 grey
/// levels or more there; so do the 23 of the catalog's icons that read as one
/// drawn into an empty slot with no number. A count seen through a dark shade
/// is told by reading the corner brightened back up to white.
constexpr int kPlainSpread = 6;

/// The keys the client's hotbar shows, row by row, left to right.
extern const std::uint32_t kKeys[kRows][kCols];

} // namespace hotbar

/// Where the hotbar is in one frame.
struct HotbarSpot {
    bool  found = false;
    int   x = 0, y = 0;             ///< slot A's top-left
    float score = 0.0f;             ///< the frame's, found or not

    ScreenRect slot(int row, int col) const {
        return ScreenRect{ x + col * hotbar::kPitch, y + row * hotbar::kPitch, hotbar::kCell,
                           hotbar::kCell };
    }
};

/// Look for the hotbar in the bottom rows of `f` by `picture`. With `last`
/// found, it is looked for there first, give or take two pixels -- it does
/// not move unless the window does -- and over the bottom rows only when it is
/// not there.
HotbarSpot find_hotbar(const Frame& f, const Frame& picture, const HotbarSpot* last = nullptr);

/// Where key `vk` is on the hotbar. False for a key that is not one of its 18.
bool hotbar_slot_of(std::uint32_t vk, int& row, int& col);

/// What one look at a potion key's slot found.
enum class SlotState {
    NoHotbar,       ///< the hotbar is not in view -- covered, or not in the world
    NotOnBar,       ///< the key is not one of the hotbar's
    Unreadable,     ///< something is there that does not read as a count
    Empty,          ///< nothing in the slot
    Single,         ///< an icon and no number: one left
    Count,          ///< a number
};
const char* slot_state_name(SlotState s);

struct SlotReading {
    SlotState state = SlotState::NoHotbar;
    int   count = 0;                ///< for Count, the number; for Single, 1
    int   row = -1, col = -1;       ///< on the hotbar, when the key is on it
    float label = -1.0f;            ///< how alike the slot's label is to the key's; -1 unchecked

    /// Two looks that say the same.
    bool same(const SlotReading& o) const {
        return state == o.state && (state != SlotState::Count || count == o.count);
    }
    /// A number of potions there is: a count, or the one of a single.
    bool has_count() const { return state == SlotState::Count || state == SlotState::Single; }
};

/// Read key `vk`'s slot. `picture` is the one the hotbar was found by, which
/// carries the labels: the slot is read only when its label is the key's,
/// and only when the bar's frame round it is as the picture has it -- nothing
/// over any part of the slot; otherwise it is Unreadable. So is an icon with
/// no number whose count's corner says something lies over it inside the
/// frame (kPlainSpread): never one potion.
/// Without the digits loaded a slot with a number in it is Unreadable; empty
/// and single slots are still told.
SlotReading read_slot(const Frame& f, const HotbarSpot& bar, std::uint32_t vk,
                      const CountReader& digits, const Frame& picture);

/// The picture of the hotbar, cut from a frame with slot A's top-left at
/// `x`, `y`: the frame round the slots opaque, the slots and the four rounded
/// corners see-through, each label at kLabelAlpha. How grabbed/hotbar_frame
/// was made, from the live client's frame.png, so that it can be made again
/// from another frame -- and so the tests can check the one that ships.
/// Empty when the hotbar would not fit in `f` there.
Frame hotbar_picture_from(const Frame& f, int x, int y);

/// configs/app.ini [Potions].
struct PotionSettings {
    /// Warn when a potion key's slot shows fewer than this; 0 warns of nothing
    /// running low (the slot running empty is still said).
    int warn_below = 20;
    /// How often the slots are read during a run, in seconds.
    int read_every_s = 10;

    static constexpr int kMaxWarn = 500;
    static constexpr int kMinEvery = 2;
    static constexpr int kMaxEvery = 300;

    /// Anything missing or out of range is the default, or brought into range.
    void load(const IniFile& ini);
};

/// Decides when a potion key's slot is worth a message, from one look at a
/// time. Pure logic: no clock, no frames.
///
/// The window looks at the slots every few seconds, and each look reads every
/// slot twice, 400 ms apart: the two readings make one (look_of), the look's,
/// and that is what is fed -- once per look. A misread is seldom a one-frame
/// flicker: an icon that makes a digit read wrong makes it read wrong on both
/// frames of a look, and on the next look too. So:
///
/// - A count, or an icon with no number, is believed at once -- the two frames
///   of the look agreed on it. Anything else (empty, unreadable, no hotbar) is
///   believed when the look before said the same.
/// - A wild drop is not believed: a count a fifth or less of the last count
///   believed (kWildFloor or more), which no drinking does in one look. It is
///   held -- the tab and believed() keep the old count, and nothing is warned
///   of -- for up to kWildLooks looks that read it or less. A count back above
///   a fifth ends it: a misread (Event::Misread). The kWildLooks-th look is
///   believed after all (Event::WildBelieved): potions given away or traded.
///   Each look held is Event::Wild, so that its frame can be kept. Looks that
///   are no count -- the slot covered, unreadable, its key off the bar --
///   neither confirm a wild drop nor clear it, believed so or not, and keep
///   the last count believed to tell the next count from: 62, 6, covered,
///   covered, 60 is a misread, and 62, covered, covered, 6, 6 a drop held.
///   The slot believed empty, or holding one, ends both.
/// - Low: the count under warn_below on two looks in a row -- not the two
///   frames of one -- once. A look in between that is no count (covered,
///   unreadable, the frames not agreeing) breaks the pair. Not again until
///   re-armed: by a reading of warn_below + 5 or more, by one 20 or more above
///   the last believed count -- the next stack -- or by a new run. A slot with
///   an icon and no number is not warned of: that is how a skill on the key
///   looks too, and a stack running down was warned of at its numbers.
/// - Out: the slot empty on two looks in a row, once, until a count or single
///   or a new run.
class PotionWatch {
public:
    /// The least believed count a wild drop is told from: below it a fifth is
    /// a potion or two, which drinking does.
    static constexpr int kWildFloor = 10;
    /// Looks in a row a wild drop is held for before it is believed.
    static constexpr int kWildLooks = 3;

    struct Event {
        enum class Kind {
            Low,            ///< "Sắp hết bình": `count` left
            Out,            ///< the slot empty
            Wild,           ///< a look read `count`, a wild drop from `was`: not believed, held
            Misread,        ///< the wild `count` came back to `back`: it was a misread of `was`
            WildBelieved,   ///< the wild drop to `count` read kWildLooks looks running: believed
        };
        Kind          kind = Kind::Low;
        std::uint32_t vk = 0;
        int           count = 0;
        int           was = 0;      ///< Wild, Misread, WildBelieved: the believed count before
        int           back = 0;     ///< Misread: the count read when it came back
    };

    void set_warn_below(int n) { warn_below_ = n < 0 ? 0 : n; }
    int  warn_below() const { return warn_below_; }

    /// One look's reading of a slot from its two frames: the reading when
    /// they agree (SlotReading::same), otherwise Unreadable -- the slot
    /// changed between them, or one frame misread it.
    static SlotReading look_of(const SlotReading& first, const SlotReading& second);

    /// One look's reading of key `vk`'s slot (look_of); the messages and
    /// events it calls for.
    std::vector<Event> feed(std::uint32_t vk, const SlotReading& r);

    /// The last reading of `vk` that is believed. False when there is none
    /// yet.
    bool believed(std::uint32_t vk, SlotReading& out) const;

    /// Whether a wild drop of `vk` is held: read, not believed, and not come
    /// back yet -- so that the frame of its look is still worth keeping.
    bool holding(std::uint32_t vk) const;

    /// A new run: every key may warn again, and nothing seen before counts.
    void reset();
    /// A key that is no longer a potion key.
    void forget(std::uint32_t vk) { keys_.erase(vk); }

private:
    struct Key {
        bool        have_last = false;
        SlotReading last;           ///< the last look that was not held as wild
        bool        have_believed = false;
        SlotReading believed;
        bool        low_armed = true;
        bool        out_armed = true;
        bool        under_before = false;   ///< the look before was a count under warn_below
        int         wild_count = 0;         ///< the wild drop held: the least it read
        int         wild_looks = 0;         ///< count looks that read it; 0 when none is held
        /// The last count believed, kept while the slot is believed covered
        /// or unreadable: what a wild drop is told from. -1 for none.
        int         ref = -1;
    };
    std::map<std::uint32_t, Key> keys_;
    int warn_below_ = 20;
};

/// For the black box written after a death: what each potion key's slot was
/// last believed to hold, and when a look last read it so -- "Potion slots:
/// key 1 16 (read 31 s earlier; last look 2 s earlier: no hotbar), key 2 40
/// (read 1 s earlier)". A press the black box shows with the count unchanged
/// is one the game did not drink -- unless the count is old: a look that did
/// not read what is believed (the hotbar covered, the slot unreadable, a wild
/// drop held) leaves that and its time as they were, and is told beside them
/// (R8R-4). No clock of its own, and not thread-safe: the GUI keeps it under
/// its own lock.
class PotionSlotLog {
public:
    /// One look at key `key`'s slot, back at `now` (any steady clock, the one
    /// line() is asked on): `look` is what the look itself read
    /// (PotionWatch::look_of), `have_believed` and `believed` what the watch
    /// believes of the key once that look was fed to it
    /// (PotionWatch::believed). What is believed counts as read at `now` only
    /// when the look says the same (SlotReading::same). A key with nothing
    /// believed yet is left out.
    void note(const std::string& key, const SlotReading& look, bool have_believed, const SlotReading& believed,
              std::uint64_t now);
    /// The line: each key in the order it was first believed, "key 1 16
    /// (read 31 s earlier)", and "; last look 2 s earlier: no hotbar" inside
    /// the brackets when a newer look read something else. Empty when nothing
    /// is believed of any key.
    std::string line(std::uint64_t now) const;
    /// A new run: nothing read yet.
    void clear() { keys_.clear(); }

private:
    struct Seen {
        std::string   key;          ///< the key's name, as the log writes it
        std::string   said;         ///< what its slot is believed to hold
        std::uint64_t read_ms = 0;  ///< when a look last read it so
        std::string   last;         ///< what a newer look read instead; empty when none did
        std::uint64_t last_ms = 0;  ///< when
    };
    std::vector<Seen> keys_;
};

/// What the Đánh quái tab says under a potion row, in Vietnamese: "Còn 27
/// bình (ô phím 1)", "Ô phím 1 đang trống", "Chưa đọc được số bình", "Phím Q
/// không có trên thanh phím". `key` is the key as the tab writes it.
std::string potion_line_vi(const SlotReading& r, const std::string& key);

} // namespace gobot
