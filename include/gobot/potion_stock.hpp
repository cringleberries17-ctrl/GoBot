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
/// of each potion key, read the way CountReader reads a bag slot's count.
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
/// carries the labels: the slot is read only when its label is the key's.
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

/// Decides when a potion key's slot is worth a message, from one reading at
/// a time. Pure logic: no clock, no frames.
///
/// A reading is believed only when the one before it said the same, so a
/// digit misread on one frame says nothing; the window reads each slot twice
/// in quick succession for that, every few seconds.
///
/// - Low: a believed count under warn_below, once. Not again until re-armed:
///   by a reading of warn_below + 5 or more, by one 20 or more above the last
///   believed count -- the next stack -- or by a new run. A slot with an icon
///   and no number is not warned of: that is how a skill on the key looks
///   too, and a stack running down was warned of at its numbers.
/// - Out: the slot believed empty, once, until a believed count or single or
///   a new run.
class PotionWatch {
public:
    struct Event {
        enum class Kind { Low, Out };
        Kind          kind = Kind::Low;
        std::uint32_t vk = 0;
        int           count = 0;    ///< for Low
    };

    void set_warn_below(int n) { warn_below_ = n < 0 ? 0 : n; }
    int  warn_below() const { return warn_below_; }

    /// One reading of key `vk`'s slot; the messages it calls for, at most one.
    std::vector<Event> feed(std::uint32_t vk, const SlotReading& r);

    /// The last reading of `vk` that two looks agreed on. False when there is
    /// none yet.
    bool believed(std::uint32_t vk, SlotReading& out) const;

    /// A new run: every key may warn again, and nothing seen before counts.
    void reset();
    /// A key that is no longer a potion key.
    void forget(std::uint32_t vk) { keys_.erase(vk); }

private:
    struct Key {
        bool        have_last = false;
        SlotReading last;
        bool        have_believed = false;
        SlotReading believed;
        bool        low_armed = true;
        bool        out_armed = true;
    };
    std::map<std::uint32_t, Key> keys_;
    int warn_below_ = 20;
};

/// What the Đánh quái tab says under a potion row, in Vietnamese: "Còn 27
/// bình (ô phím 1)", "Ô phím 1 đang trống", "Chưa đọc được số bình", "Phím Q
/// không có trên thanh phím". `key` is the key as the tab writes it.
std::string potion_line_vi(const SlotReading& r, const std::string& key);

} // namespace gobot
