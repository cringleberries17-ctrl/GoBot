#include "gobot/potion_stock.hpp"
#include "gobot/template_matcher.hpp"

#include <algorithm>
#include <cmath>

namespace gobot {

namespace hotbar {

const std::uint32_t kKeys[kRows][kCols] = {
    { 'A', 'S', 'D', 'F', 'G', 'H', '1', '2', '3' },
    { 'Z', 'X', 'C', 'V', 'B', 'N', '4', '5', '6' },
};

} // namespace hotbar

namespace {

using namespace hotbar;

int lum_at(const Frame& f, int x, int y) {
    std::uint8_t r, g, b;
    f.get(x, y, r, g, b);
    return (r + g + b) / 3;
}

/// White as the game draws digits and key labels: bright and grey. The same
/// test CountReader reads the counts by.
bool white_at(const Frame& f, int x, int y) {
    std::uint8_t r, g, b;
    f.get(x, y, r, g, b);
    const int lo = std::min({ r, g, b }), hi = std::max({ r, g, b });
    return (r + g + b) / 3 > 175 && hi - lo < 40;
}

ScreenRect clamp_to(const Frame& f, ScreenRect r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.right()), y1 = std::min(f.height, r.bottom());
    return ScreenRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

/// How alike the label in slot `s` of `f` is to the picture's label for the
/// slot at `row`, `col`: correlation over the label's pixels, in grey. False
/// when the picture carries no label there, and nothing can be said.
bool label_score(const Frame& f, const ScreenRect& s, const Frame& pic, int row, int col,
                 float& score) {
    const int px = kSlotDx + col * kPitch, py = kSlotDy + row * kPitch;
    if (!pic.valid() || px + kCell > pic.width || py + kCell > pic.height) return false;
    std::vector<double> a, b;
    for (int y = kLabelY; y < kLabelY + kLabelH; ++y)
        for (int x = kLabelX; x < kLabelX + kLabelW; ++x) {
            const std::size_t i =
                (static_cast<std::size_t>(py + y) * pic.width + static_cast<std::size_t>(px + x)) * 4;
            const std::uint8_t alpha = pic.pixels[i + 3];
            if (alpha == 0 || alpha >= TemplateMatcher::kOpaque) continue;
            b.push_back((pic.pixels[i] + pic.pixels[i + 1] + pic.pixels[i + 2]) / 3.0);
            a.push_back(lum_at(f, s.x + x, s.y + y));
        }
    // A label is a few dozen pixels; a handful is no label at all.
    if (b.size() < 12) return false;
    double ma = 0, mb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
    ma /= static_cast<double>(a.size());
    mb /= static_cast<double>(b.size());
    double cov = 0, va = 0, vb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        cov += (a[i] - ma) * (b[i] - mb);
        va += (a[i] - ma) * (a[i] - ma);
        vb += (b[i] - mb) * (b[i] - mb);
    }
    // Flat on screen -- covered by something plain -- is no label.
    const double least = static_cast<double>(a.size()) * 4.0;
    score = va < least || vb < least ? -1.0f : static_cast<float>(cov / std::sqrt(va * vb));
    return true;
}

/// The worst of the four sides of the bar's own frame round the slot at
/// `row`, `col`: on each, the mean difference in grey between `f` and the
/// picture over the picture's frame pixels in the kRingBand pixels next to
/// the slot. 0 where the frame is as the picture has it; 1000 for a side
/// that is off the frame.
double ring_worst(const Frame& f, const HotbarSpot& bar, const Frame& pic, int row, int col) {
    const int ox = bar.x - kSlotDx, oy = bar.y - kSlotDy;
    const int sx = kSlotDx + col * kPitch, sy = kSlotDy + row * kPitch;
    const auto side = [&](int x0, int y0, int x1, int y1) {
        long sum = 0, n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                if (x < 0 || y < 0 || x >= pic.width || y >= pic.height) continue;
                const std::size_t i = (static_cast<std::size_t>(y) * pic.width + static_cast<std::size_t>(x)) * 4;
                if (pic.pixels[i + 3] != 255) continue;
                const int fx = ox + x, fy = oy + y;
                if (fx < 0 || fy < 0 || fx >= f.width || fy >= f.height) return 1000.0;
                const int want = (pic.pixels[i] + pic.pixels[i + 1] + pic.pixels[i + 2]) / 3;
                sum += std::abs(lum_at(f, fx, fy) - want);
                ++n;
            }
        return n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
    };
    return std::max({ side(sx - 1, sy - kRingBand, sx + kCell + 1, sy),
                      side(sx - 1, sy + kCell, sx + kCell + 1, sy + kCell + kRingBand),
                      side(sx - kRingBand, sy, sx, sy + kCell),
                      side(sx + kCell, sy, sx + kCell + kRingBand, sy + kCell) });
}

/// Whether the count of slot `s` -- nothing white in its corner, an icon in
/// it -- is one that something lies over inside the slot, where the bar's own
/// frame round it shows nothing: the corner all one colour, as a window's body
/// is and nothing the game draws there ever is; or digits that read once the
/// corner is brightened back up to white, as a dark shade seen through leaves
/// a count -- grey, too dim to be taken for white.
bool count_covered(const Frame& f, const ScreenRect& s, const CountReader& digits) {
    int lo = 255, hi = 0;
    for (int y = 0; y < kCountH; ++y)
        for (int x = 0; x < kCountW; ++x) {
            const int v = lum_at(f, s.x + x, s.y + y);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    if (hi - lo <= kPlainSpread) return true;
    if (!digits.loaded() || hi <= 0) return false;
    // The slot again, every colour scaled so that the corner's brightest is
    // white: a count under a shade reads as itself, an icon's own corner as
    // no number.
    Frame lit;
    lit.width = kCell;
    lit.height = kCell;
    lit.pixels.assign(static_cast<std::size_t>(kCell) * kCell * 4, 255);
    for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < kCell; ++x) {
            std::uint8_t r, g, b;
            f.get(s.x + x, s.y + y, r, g, b);
            std::uint8_t* p = &lit.pixels[(static_cast<std::size_t>(y) * kCell + static_cast<std::size_t>(x)) * 4];
            p[0] = static_cast<std::uint8_t>(std::min(255, b * 250 / hi));
            p[1] = static_cast<std::uint8_t>(std::min(255, g * 250 / hi));
            p[2] = static_cast<std::uint8_t>(std::min(255, r * 250 / hi));
        }
    const int n = digits.read(lit, ScreenRect{ 0, 0, kCell, kCell });
    return n != CountReader::kUnreadable && n >= 2;
}

} // namespace

HotbarSpot find_hotbar(const Frame& f, const Frame& picture, const HotbarSpot* last) {
    HotbarSpot out;
    if (!f.valid() || !picture.valid()) return out;
    const auto take = [&](const TemplateMatcher::Match& m) {
        out.score = std::max(out.score, m.found ? m.score : 0.0f);
        if (!m.found || m.score < kFrameScore) return false;
        out.found = true;
        out.x = m.at.x + kSlotDx;
        out.y = m.at.y + kSlotDy;
        out.score = m.score;
        return true;
    };

    // Where it was, first: it stays put unless the window is resized, and a
    // look there costs a fraction of a search along the bottom.
    if (last && last->found) {
        const ScreenRect there = clamp_to(f, ScreenRect{ last->x - kSlotDx - 2, last->y - kSlotDy - 2,
                                                         picture.width + 4, picture.height + 4 });
        if (there.w >= picture.width && there.h >= picture.height &&
            take(TemplateMatcher::find(f, there, picture, 0.5f)))
            return out;
    }
    const int rows = std::min(f.height, kSearchRows);
    const ScreenRect bottom{ 0, f.height - rows, f.width, rows };
    if (bottom.w < picture.width || bottom.h < picture.height) return out;
    take(TemplateMatcher::find(f, bottom, picture, 0.5f));
    return out;
}

bool hotbar_slot_of(std::uint32_t vk, int& row, int& col) {
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < kCols; ++c)
            if (kKeys[r][c] == vk) {
                row = r;
                col = c;
                return true;
            }
    row = col = -1;
    return false;
}

const char* slot_state_name(SlotState s) {
    switch (s) {
        case SlotState::NoHotbar:   return "no hotbar";
        case SlotState::NotOnBar:   return "not on the hotbar";
        case SlotState::Unreadable: return "unreadable";
        case SlotState::Empty:      return "empty";
        case SlotState::Single:     return "one, no number";
        case SlotState::Count:      return "count";
    }
    return "?";
}

SlotReading read_slot(const Frame& f, const HotbarSpot& bar, std::uint32_t vk,
                      const CountReader& digits, const Frame& picture) {
    SlotReading r;
    if (!hotbar_slot_of(vk, r.row, r.col)) {
        r.state = SlotState::NotOnBar;
        return r;
    }
    const ScreenRect s = bar.slot(r.row, r.col);
    if (!bar.found || !f.valid() || s.x < 0 || s.y < 0 || s.right() > f.width ||
        s.bottom() > f.height) {
        r.state = SlotState::NoHotbar;
        return r;
    }

    // The bar's own frame round the slot, as the picture has it on all four
    // sides: whatever lies over part of the slot lies over its frame on that
    // side too. A dark window's edge over the top of slot 1 hid the count and
    // left an icon with no number, "one left" -- or, over half a count, would
    // leave the other half of it to be read.
    if (ring_worst(f, bar, picture, r.row, r.col) > kRingDiff) {
        r.state = SlotState::Unreadable;
        return r;
    }

    // The slot has to say it is the key's: a hotbar whose keys the player
    // has changed in the game's settings puts another key's label there, and
    // the number under it is then some other slot's.
    float label = 0.0f;
    if (label_score(f, s, picture, r.row, r.col, label)) {
        r.label = label;
        if (label < kLabelScore) {
            r.state = SlotState::Unreadable;
            return r;
        }
    }

    // Nothing white where the count goes: one item, or none.
    int white = 0;
    for (int y = 0; y < kCountH; ++y)
        for (int x = 0; x < kCountW; ++x)
            if (white_at(f, s.x + x, s.y + y)) ++white;
    if (white == 0) {
        int brightest = 0;
        for (int y = 0; y < kCell; ++y)
            for (int x = 0; x < kCell; ++x)
                if (x < kCell - kLabelCorner || y < kCell - kLabelCorner)
                    brightest = std::max(brightest, lum_at(f, s.x + x, s.y + y));
        if (brightest <= kEmptyBrightest) {
            r.state = SlotState::Empty;
            return r;
        }
        // An icon and no number: one left -- unless the number is there under
        // something inside the slot, which the frame round it does not show.
        if (count_covered(f, s, digits)) {
            r.state = SlotState::Unreadable;
            return r;
        }
        r.state = SlotState::Single;
        r.count = 1;
        return r;
    }

    // Something white is there, so it reads as a number or not at all --
    // never as the 1 CountReader gives a corner with nothing in it. Nor as a
    // 1 that does read: the client prints counts from 2 up, so a "1" there is
    // a white stroke of the icon's own, not a number.
    const int n = digits.read(f, s);
    if (n == CountReader::kUnreadable || n <= 1) {
        r.state = SlotState::Unreadable;
        return r;
    }
    r.state = SlotState::Count;
    r.count = n;
    return r;
}

Frame hotbar_picture_from(const Frame& f, int x, int y) {
    Frame out;
    const int x0 = x - kSlotDx, y0 = y - kSlotDy;
    if (!f.valid() || x0 < 0 || y0 < 0 || x0 + kPictureW > f.width || y0 + kPictureH > f.height)
        return out;
    out.width = kPictureW;
    out.height = kPictureH;
    out.pixels.assign(static_cast<std::size_t>(kPictureW) * kPictureH * 4, 0);
    const auto at = [&](int px, int py) {
        return (static_cast<std::size_t>(py) * kPictureW + static_cast<std::size_t>(px)) * 4;
    };
    for (int py = 0; py < kPictureH; ++py)
        for (int px = 0; px < kPictureW; ++px) {
            const std::size_t src =
                (static_cast<std::size_t>(y0 + py) * f.width + static_cast<std::size_t>(x0 + px)) * 4;
            std::copy_n(f.pixels.begin() + static_cast<std::ptrdiff_t>(src), 3,
                        out.pixels.begin() + static_cast<std::ptrdiff_t>(at(px, py)));
            out.pixels[at(px, py) + 3] = 255;
        }

    // The four corners are rounded: the scenery shows through outside them.
    for (const auto& c : { std::pair{ 0, 0 }, std::pair{ kPictureW - 3, 0 },
                           std::pair{ 0, kPictureH - 3 }, std::pair{ kPictureW - 3, kPictureH - 3 } })
        for (int dy = 0; dy < 3; ++dy)
            for (int dx = 0; dx < 3; ++dx) out.pixels[at(c.first + dx, c.second + dy) + 3] = 0;

    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kCols; ++col) {
            const int sx = kSlotDx + col * kPitch, sy = kSlotDy + row * kPitch;
            // The slot: whatever is in it is the player's, not the bar's.
            for (int dy = 0; dy < kCell; ++dy)
                for (int dx = 0; dx < kCell; ++dx) out.pixels[at(sx + dx, sy + dy) + 3] = 0;

            // Its label: the white strokes in the corner and the dark outline
            // round them, at the alpha that marks a label. Only where a label
            // sits, a pixel in from the label's corner, so the item under it
            // -- white itself, sometimes -- is not taken for part of it.
            const auto stroke = [&](int dx, int dy) {
                return dx >= kLabelX + 1 && dx < kLabelX + kLabelW - 1 && dy >= kLabelY + 1 &&
                       dy < kLabelY + kLabelH - 1 && white_at(f, x + col * kPitch + dx, y + row * kPitch + dy);
            };
            for (int dy = kLabelY; dy < kLabelY + kLabelH; ++dy)
                for (int dx = kLabelX; dx < kLabelX + kLabelW; ++dx) {
                    bool mark = stroke(dx, dy);
                    if (!mark && lum_at(f, x + col * kPitch + dx, y + row * kPitch + dy) < 70)
                        for (int ny = -1; ny <= 1 && !mark; ++ny)
                            for (int nx = -1; nx <= 1 && !mark; ++nx) mark = stroke(dx + nx, dy + ny);
                    if (mark) out.pixels[at(sx + dx, sy + dy) + 3] = kLabelAlpha;
                }
        }
    return out;
}

void PotionSettings::load(const IniFile& ini) {
    warn_below = std::clamp(ini.get_int("Potions", "warn_below", 20), 0, kMaxWarn);
    read_every_s = std::clamp(ini.get_int("Potions", "read_every_s", 10), kMinEvery, kMaxEvery);
}

std::vector<PotionWatch::Event> PotionWatch::feed(std::uint32_t vk, const SlotReading& r) {
    std::vector<Event> out;
    Key& k = keys_[vk];

    // Re-armed by one reading alone: what it costs when that reading is
    // wrong is one more message, which is cheap; a stack swapped for the next
    // one and read only once is a warning never given again.
    if (r.state == SlotState::Count &&
        (r.count >= warn_below_ + 5 ||
         (k.have_believed && k.believed.has_count() && r.count >= k.believed.count + 20)))
        k.low_armed = true;

    const bool agreed = k.have_last && k.last.same(r);
    k.last = r;
    k.have_last = true;
    if (!agreed) return out;
    k.believed = r;
    k.have_believed = true;

    if (r.has_count()) {
        k.out_armed = true;
        if (r.state == SlotState::Count && warn_below_ > 0 && r.count < warn_below_ && k.low_armed) {
            k.low_armed = false;
            out.push_back(Event{ Event::Kind::Low, vk, r.count });
        }
    } else if (r.state == SlotState::Empty && k.out_armed) {
        k.out_armed = false;
        out.push_back(Event{ Event::Kind::Out, vk, 0 });
    }
    return out;
}

bool PotionWatch::believed(std::uint32_t vk, SlotReading& out) const {
    const auto it = keys_.find(vk);
    if (it == keys_.end() || !it->second.have_believed) return false;
    out = it->second.believed;
    return true;
}

void PotionWatch::reset() { keys_.clear(); }

std::string potion_line_vi(const SlotReading& r, const std::string& key) {
    switch (r.state) {
        case SlotState::Count:
        case SlotState::Single:
            return "Còn " + std::to_string(r.count) + " bình (ô phím " + key + ")";
        case SlotState::Empty:      return "Ô phím " + key + " đang trống";
        case SlotState::NotOnBar:   return "Phím " + key + " không có trên thanh phím";
        case SlotState::Unreadable:
        case SlotState::NoHotbar:   break;
    }
    return "Chưa đọc được số bình";
}

} // namespace gobot
