#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

#include <windows.h>

namespace gobot {

namespace {

// The pictures this goes by, under the names the template store gives them.
//
// The strip as the Vietnamese client draws it. The commercial client's own
// picture of the window has page numbers where this has tab names, so it is
// no use for finding the strip, only for measuring the window -- which is
// where the layout's numbers came from.
const char* const kStrips[]    = { "grabbed/inventory", "inventory" };
const char* const kTrashCans[] = { "inven/0/trash_can", "inven/0/trash_can1",
                                   "inven/1/trash_can", "inven/1/trash_can1" };

/// [Xác Nhận][Hủy Bỏ], the pair under every one of the dialogs below.
const char* const kConfirm = "grabbed/confirm_button";

/// The dialogs that putting an item in the trash or on the floor brings up:
/// "do you really want to put it in the trash?", "how many to put in the
/// trash?", the drop confirmation, and the quantity box the drop shares with
/// selling.
const char* const kDialogs[] = { "grabbed/del_item_confirm_1", "grabbed/del_item_confirm_2",
                                 "grabbed/drop_confirm", "grabbed/sell_confirm" };

/// How alike the strip has to be. Lower than a fixed button: whichever tab is
/// selected is drawn differently, and the picture was taken with the first.
constexpr float kStripScore = 0.62f;
constexpr float kTrashScore = 0.70f;
constexpr float kButtonScore = 0.82f;
constexpr float kDialogScore = 0.60f;

/// The bag window's size, measured from the strip: it runs a strip-width or
/// so either side and about 200 pixels below.
constexpr int kBagWidth = 272;

ScreenRect clamp_to(const Frame& f, ScreenRect r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.x + r.w), y1 = std::min(f.height, r.y + r.h);
    return ScreenRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

int lum(const Frame& f, int x, int y) {
    std::uint8_t r, g, b;
    f.get(x, y, r, g, b);
    return (r + g + b) / 3;
}

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

} // namespace

// ----------------------------------------------------------------- layout

void BagLayout::load(const IniFile& ini) {
    tab_w          = ini.get_int("Bag", "tab_w", tab_w);
    cell           = ini.get_int("Bag", "cell", cell);
    pitch          = ini.get_int("Bag", "pitch", pitch);
    grid_dx        = ini.get_int("Bag", "grid_dx", grid_dx);
    grid_dy        = ini.get_int("Bag", "grid_dy", grid_dy);
    cols           = std::clamp(ini.get_int("Bag", "cols", cols), 1, 12);
    rows           = std::clamp(ini.get_int("Bag", "rows", rows), 1, 12);
    scroll_up_dx   = ini.get_int("Bag", "scroll_up_dx", scroll_up_dx);
    scroll_up_dy   = ini.get_int("Bag", "scroll_up_dy", scroll_up_dy);
    scroll_down_dx = ini.get_int("Bag", "scroll_down_dx", scroll_down_dx);
    scroll_down_dy = ini.get_int("Bag", "scroll_down_dy", scroll_down_dy);
    count_w        = ini.get_int("Bag", "count_w", count_w);
    count_h        = ini.get_int("Bag", "count_h", count_h);
    edge           = ini.get_int("Bag", "edge", edge);
}

const char* bag_tab_name(int tab) {
    switch (tab) {
        case 0: return "Trang Bị 1";
        case 1: return "Trang Bị 2";
        case 2: return "Tiêu Dùng";
        case 3: return "Khác";
        case 4: return "Linh Thú";
        default: return "?";
    }
}

ScreenRect BagView::slot(const BagLayout& l, int col, int row) const {
    return ScreenRect{ strip.x + l.grid_dx + col * l.pitch,
                       strip.y + l.grid_dy + row * l.pitch, l.cell, l.cell };
}

std::vector<ScreenRect> BagView::slots(const BagLayout& l) const {
    std::vector<ScreenRect> out;
    out.reserve(static_cast<std::size_t>(l.cols * l.rows));
    for (int r = 0; r < l.rows; ++r)
        for (int c = 0; c < l.cols; ++c) out.push_back(slot(l, c, r));
    return out;
}

void BagView::tab_point(const BagLayout& l, int tab, int& x, int& y) const {
    x = strip.x + tab * l.tab_w + l.tab_w / 2;
    y = strip.y + strip.h / 2;
}

// ---------------------------------------------------------------- looking

namespace bag {

BagView find(const Frame& f, const TemplateStore& store) {
    BagView v;
    if (!f.valid()) return v;

    const ScreenRect all{ 0, 0, f.width, f.height };
    for (const char* name : kStrips) {
        const Frame* t = store.get(name);
        if (t == nullptr) continue;
        const auto m = TemplateMatcher::find(f, all, *t, kStripScore);
        if (m.found && m.score > v.score) {
            v.found = true;
            v.score = m.score;
            v.strip = m.at;
        }
        if (v.found) break;           // the two names are one picture
    }
    if (!v.found) return v;

    // The trash can sits in the window's bottom corner. Looked for around the
    // bag rather than across the screen, where any small grey picture would do.
    const ScreenRect around = clamp_to(f, ScreenRect{ v.strip.x - 20, v.strip.y,
                                                    kBagWidth + 40, 240 });
    float best = 0.0f;
    for (const char* name : kTrashCans) {
        const Frame* t = store.get(name);
        if (t == nullptr || around.w < t->width || around.h < t->height) continue;
        const auto m = TemplateMatcher::find(f, around, *t, kTrashScore);
        if (m.found && m.score > best) {
            best = m.score;
            v.has_trash = true;
            v.trash = m.at;
        }
    }
    return v;
}

Frame cut(const Frame& f, const ScreenRect& r) {
    Frame out;
    if (!f.valid() || !r.valid()) return out;
    out.width = r.w;
    out.height = r.h;
    out.pixels.assign(static_cast<std::size_t>(r.w) * r.h * 4, 0);
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            std::uint8_t cr, cg, cb;
            f.get(r.x + x, r.y + y, cr, cg, cb);
            const std::size_t i = (static_cast<std::size_t>(y) * r.w + x) * 4;
            out.pixels[i] = cb;
            out.pixels[i + 1] = cg;
            out.pixels[i + 2] = cr;
            out.pixels[i + 3] = 255;
        }
    }
    return out;
}

Frame masked(const Frame& picture, const BagLayout& l) {
    Frame out = picture;
    if (!out.valid()) return out;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const bool at_edge = x < l.edge || y < l.edge ||
                                 x >= out.width - l.edge || y >= out.height - l.edge;
            const bool in_count = x < l.count_w && y < l.count_h;
            out.pixels[(static_cast<std::size_t>(y) * out.width + x) * 4 + 3] =
                (at_edge || in_count) ? 0 : 255;
        }
    }
    return out;
}

bool is_empty(const Frame& f, const ScreenRect& r, const BagLayout& l) {
    // The brightest few pixels decide it, not the average: an item drawn small
    // in the middle of a dark slot averages out dark too.
    int counts[256] = {};
    int n = 0;
    for (int y = r.y + l.edge + 1; y < r.y + r.h - l.edge - 1; ++y)
        for (int x = r.x + l.edge + 1; x < r.x + r.w - l.edge - 1; ++x) {
            ++counts[lum(f, x, y)];
            ++n;
        }
    if (n == 0) return true;
    int seen = 0;
    for (int v = 255; v >= 0; --v) {
        seen += counts[v];
        if (seen * 50 >= n) return v < 70;      // the 98th percentile
    }
    return true;
}

std::uint64_t grid_hash(const Frame& f, const BagView& v, const BagLayout& l) {
    std::uint64_t h = 1469598103934665603ull;
    for (const ScreenRect& s : v.slots(l)) {
        for (int y = s.y + 3; y < s.y + s.h - 3; y += 3)
            for (int x = s.x + 3; x < s.x + s.w - 3; x += 3) {
                h ^= static_cast<std::uint64_t>(lum(f, x, y) / 8);
                h *= 1099511628211ull;
            }
    }
    return h;
}

int read_count(const Frame& f, const ScreenRect& r, const BagLayout& l, const GlyphFont& font) {
    if (!font.loaded()) return 0;
    const std::string text =
        font.read_line(f, ScreenRect{ r.x + 1, r.y, l.count_w + 6, l.count_h + 3 });
    if (text.empty() || text.size() > 4) return 0;
    for (const char c : text)
        if (c < '0' || c > '9') return 0;
    return std::atoi(text.c_str());
}

} // namespace bag

// ------------------------------------------------------------------ list

const char* TrashList::folder(Action a) {
    return a == Action::Drop ? "drop" : "delete";
}

bool TrashList::load(const std::string& dir, const BagLayout& l) {
    items.clear();
    for (const Action a : { Action::Delete, Action::Drop }) {
        const std::string sub = dir + "\\" + folder(a);
        if (GetFileAttributesA(sub.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

        TemplateStore store;
        store.load(sub);
        for (const std::string& name : store.names()) {
            const Frame* picture = store.get(name);
            if (picture == nullptr || !picture->valid()) continue;
            Item item;
            item.file = sub + "\\" + name + ".png";
            item.action = a;
            item.shown = *picture;
            for (std::size_t i = 3; i < item.shown.pixels.size(); i += 4)
                item.shown.pixels[i] = 255;
            item.picture = bag::masked(item.shown, l);
            items.push_back(std::move(item));
        }
    }
    // A folder that does not exist yet is an empty list, not a failure.
    return true;
}

std::string TrashList::add(const std::string& dir, const Frame& cut, Action a,
                           const BagLayout& l) {
    if (!cut.valid()) return {};
    const std::string sub = dir + "\\" + folder(a);
    CreateDirectoryA(dir.c_str(), nullptr);
    CreateDirectoryA(sub.c_str(), nullptr);

    // Named by when, which is unique enough for something a person clicks.
    std::string path;
    for (int n = 0; n < 100; ++n) {
        path = sub + "\\item_" + stamp() + "_" + std::to_string(n) + ".png";
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    if (!save_png(cut, path)) return {};

    Item item;
    item.file = path;
    item.action = a;
    item.shown = cut;
    item.picture = bag::masked(cut, l);
    items.push_back(std::move(item));
    return path;
}

void TrashList::add_picture(const Frame& cut, Action a, const BagLayout& l) {
    Item item;
    item.action = a;
    item.shown = cut;
    item.picture = bag::masked(cut, l);
    items.push_back(std::move(item));
}

bool TrashList::remove(std::size_t index) {
    if (index >= items.size()) return false;
    if (!items[index].file.empty()) DeleteFileA(items[index].file.c_str());
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

int TrashList::match(const Frame& f, const ScreenRect& slot, float min_score,
                     float* score) const {
    const ScreenRect area = clamp_to(f, ScreenRect{ slot.x - 3, slot.y - 3,
                                                    slot.w + 6, slot.h + 6 });
    int best = -1;
    float best_score = 0.0f;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Frame& p = items[i].picture;
        if (!p.valid() || area.w < p.width || area.h < p.height) continue;
        const auto m = TemplateMatcher::find(f, area, p, min_score,
                                             TemplateMatcher::Tone::Colour);
        if (m.found && m.score > best_score) {
            best = static_cast<int>(i);
            best_score = m.score;
        }
    }
    if (score) *score = best_score;
    return best;
}

// --------------------------------------------------------------- settings

bool CleanSettings::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) return false;
    enabled          = ini.get_bool("Trash", "enabled", enabled);
    interval_minutes = std::clamp(ini.get_int("Trash", "interval_minutes", interval_minutes), 1, 720);
    bag_key          = static_cast<std::uint32_t>(ini.get_int("Trash", "bag_key", static_cast<int>(bag_key)));
    const char* const kTabKeys[kBagTabs] = { "tab_equip1", "tab_equip2", "tab_use",
                                             "tab_other", "tab_pet" };
    for (int i = 0; i < kBagTabs; ++i) tabs[i] = ini.get_bool("Trash", kTabKeys[i], tabs[i]);
    min_score        = std::clamp(ini.get_float("Trash", "min_score", min_score), 0.70f, 0.99f);
    max_per_run      = std::clamp(ini.get_int("Trash", "max_per_run", max_per_run), 1, 500);
    click_to_move    = ini.get_bool("Trash", "click_to_move", click_to_move);
    layout.load(ini);
    return true;
}

// ---------------------------------------------------------------- cleaner

BagCleaner::BagCleaner(InputManager& input, const TemplateStore& store, Grab grab)
    : input_(input), store_(store), grab_(std::move(grab)) {}

void BagCleaner::say(const std::string& line) {
    GB_INFO("Trash: %s", line.c_str());
    if (report_) report_(line);
}

bool BagCleaner::wait_ms(int ms) {
    for (int slept = 0; slept < ms; slept += 30) {
        if (cancelled_) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(30, ms - slept)));
    }
    return !cancelled_;
}

bool BagCleaner::grab_view(Frame& f, BagView& v) {
    if (!grab_ || !grab_(f) || !f.valid()) return false;
    v = bag::find(f, store_);
    return v.found;
}

void BagCleaner::scroll_to_top(BagView& v) {
    const BagLayout& l = settings_.layout;
    Frame f;
    BagView now;
    if (!grab_view(f, now)) return;
    std::uint64_t before = bag::grid_hash(f, now, l);
    for (int i = 0; i < 40 && !cancelled_; ++i) {
        input_.click_at(now.strip.x + l.scroll_up_dx, now.strip.y + l.scroll_up_dy);
        if (!wait_ms(140) || !grab_view(f, now)) break;
        const std::uint64_t after = bag::grid_hash(f, now, l);
        if (after == before) break;
        before = after;
    }
    v = now;
}

BagCleaner::Result BagCleaner::run() {
    Result res;
    cancelled_ = false;
    const BagLayout& l = settings_.layout;

    if (trash_ == nullptr || trash_->items.empty()) {
        res.trouble = "danh sách đồ bỏ đi đang trống";
        return res;
    }

    Frame f;
    BagView v;
    bool opened_here = false;
    if (!grab_view(f, v)) {
        input_.key_press(settings_.bag_key);
        opened_here = true;
        for (int i = 0; i < 12 && !v.found; ++i) {
            if (!wait_ms(150)) return res;
            grab_view(f, v);
        }
        if (!v.found) {
            res.trouble = "không mở được túi đồ (phím " + InputManager::key_name(settings_.bag_key) +
                          "), hoặc túi đồ bị che";
            return res;
        }
    }

    bool needs_trash = false;
    for (const auto& item : trash_->items)
        if (item.action == TrashList::Action::Delete) needs_trash = true;
    if (needs_trash && !v.has_trash && !dry_run_) {
        res.trouble = "không thấy thùng rác trong túi đồ";
        if (opened_here) input_.key_press(settings_.bag_key);
        return res;
    }
    res.ran = true;

    int done = 0;
    bool stop = false;
    for (int tab = 0; tab < kBagTabs && !stop && !cancelled_; ++tab) {
        if (!settings_.tabs[tab]) continue;

        int tx = 0, ty = 0;
        v.tab_point(l, tab, tx, ty);
        input_.click_at(tx, ty);
        if (!wait_ms(350)) break;
        scroll_to_top(v);

        // Round and round the page on show: after anything is thrown away the
        // page is looked at afresh, since the slot it was in is now empty and
        // the rest may have moved up into it.
        for (int round = 0; round < 200 && !stop && !cancelled_; ++round) {
            if (!grab_view(f, v)) {
                res.trouble = "mất dấu túi đồ";
                stop = true;
                break;
            }

            bool acted = false;
            const auto slots = v.slots(l);
            for (std::size_t s = 0; s < slots.size() && !cancelled_; ++s) {
                if (bag::is_empty(f, slots[s], l)) continue;
                float score = 0.0f;
                const int which = trash_->match(f, slots[s], settings_.min_score, &score);
                if (which < 0) continue;
                const TrashList::Item& item = trash_->items[static_cast<std::size_t>(which)];
                ++res.matched;

                char where[96];
                std::snprintf(where, sizeof where, "%s, hàng %zu ô %zu (giống %.0f%%)",
                              bag_tab_name(tab), s / static_cast<std::size_t>(l.cols) + 1,
                              s % static_cast<std::size_t>(l.cols) + 1,
                              static_cast<double>(score) * 100.0);
                if (dry_run_) {
                    say(std::string("thấy đồ bỏ đi ở ") + where);
                    continue;
                }
                if (done >= settings_.max_per_run) {
                    say("đã bỏ " + std::to_string(done) + " món, đủ cho một lần");
                    stop = true;
                    break;
                }

                const int count = font_ ? bag::read_count(f, slots[s], l, *font_) : 0;
                if (rid(f, v, static_cast<int>(s), item, count)) {
                    ++done;
                    if (item.action == TrashList::Action::Delete) ++res.deleted;
                    else                                          ++res.dropped;
                    say(std::string(item.action == TrashList::Action::Delete ? "đã xoá" : "đã vứt") +
                        " món ở " + where);
                    acted = true;
                    break;
                }
                ++res.failed;
                say(std::string("không bỏ được món ở ") + where);
                if (res.failed >= 3) {
                    res.trouble = "bỏ đồ không được 3 lần, dừng lại";
                    stop = true;
                    break;
                }
            }
            if (acted || stop) continue;

            // Nothing more on this page: the next rows, until scrolling stops
            // changing anything.
            const std::uint64_t before = bag::grid_hash(f, v, l);
            input_.click_at(v.strip.x + l.scroll_down_dx, v.strip.y + l.scroll_down_dy);
            if (!wait_ms(180)) break;
            Frame g;
            BagView w;
            if (!grab_view(g, w) || bag::grid_hash(g, w, l) == before) break;
        }
    }

    if (opened_here) input_.key_press(settings_.bag_key);
    return res;
}

bool BagCleaner::rid(Frame& f, BagView& v, int index, const TrashList::Item& item, int count) {
    const BagLayout& l = settings_.layout;
    const auto slots = v.slots(l);
    if (index < 0 || index >= static_cast<int>(slots.size())) return false;
    const ScreenRect& s = slots[static_cast<std::size_t>(index)];
    const int fx = s.cx(), fy = s.cy();

    int tx = 0, ty = 0;
    if (item.action == TrashList::Action::Delete) {
        tx = v.trash.cx();
        ty = v.trash.cy();
    } else {
        // Onto the ground, clear of the bag: whichever side has more room.
        const int room_left = v.strip.x;
        const int room_right = f.width - (v.strip.x + kBagWidth);
        tx = room_left > room_right ? std::max(40, v.strip.x - 160)
                                    : std::min(f.width - 40, v.strip.x + kBagWidth + 160);
        ty = std::clamp(v.strip.y + 40, 140, std::max(140, f.height - 160));
    }

    bool moved = false;
    if (settings_.click_to_move) {
        moved = input_.click_at(fx, fy) && wait_ms(220) && input_.click_at(tx, ty);
    } else {
        moved = input_.drag(fx, fy, tx, ty);
    }
    if (!moved) {
        say("không điều khiển được chuột trong cửa sổ game");
        return false;
    }

    if (!answer_dialog(count)) {
        // Whatever is half done -- a dialog, an item on the pointer -- undone.
        input_.key_press(VK_ESCAPE);
        wait_ms(300);
        return false;
    }

    // Only gone counts.
    wait_ms(350);
    Frame g;
    BagView w;
    if (!grab_view(g, w)) return false;
    const auto after = w.slots(l);
    const ScreenRect& same = after[static_cast<std::size_t>(index)];
    if (!bag::is_empty(g, same, l)) {
        const int still = trash_->match(g, same, settings_.min_score);
        if (still >= 0 && &trash_->items[static_cast<std::size_t>(still)] == &item) {
            say("món đồ vẫn còn trong túi");
            return false;
        }
    }
    f = g;
    v = w;
    return true;
}

bool BagCleaner::answer_dialog(int count) {
    const Frame* confirm = store_.get(kConfirm);
    if (confirm == nullptr) {
        say("thiếu hình nút Xác Nhận, nên không bấm gì");
        return false;
    }

    Frame f;
    TemplateMatcher::Match button;
    for (int i = 0; i < 12 && !button.found; ++i) {
        if (!wait_ms(150)) return false;
        if (!grab_ || !grab_(f) || !f.valid()) continue;
        button = TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *confirm,
                                       kButtonScore);
    }
    if (!button.found) {
        say("không thấy hộp thoại xác nhận");
        return false;
    }

    // Ours, and not some other dialog with the same two buttons: one of the
    // trash or drop dialogs has to be sitting on top of them.
    const ScreenRect above = clamp_to(f, ScreenRect{ button.at.x - 210, button.at.y - 95,
                                                     button.at.w + 230, button.at.h + 105 });
    bool ours = false;
    for (const char* name : kDialogs) {
        const Frame* t = store_.get(name);
        if (t == nullptr || above.w < t->width || above.h < t->height) continue;
        if (TemplateMatcher::find(f, above, *t, kDialogScore).found) { ours = true; break; }
    }
    if (!ours) {
        say("hộp thoại này không phải hộp thoại bỏ đồ, nên không bấm");
        return false;
    }

    // The dialogs for a stack have a quantity box left of the buttons: dark,
    // where the others are the dialog's pale grey.
    const ScreenRect box = clamp_to(f, ScreenRect{ button.at.x - 88, button.at.y + 3,
                                                   82, std::max(4, button.at.h - 6) });
    long total = 0;
    int n = 0;
    for (int y = box.y; y < box.y + box.h; ++y)
        for (int x = box.x; x < box.x + box.w; ++x) { total += lum(f, x, y); ++n; }
    if (n > 0 && total / n < 100) {
        input_.click_at(box.cx(), box.cy());
        wait_ms(120);
        for (int i = 0; i < 6; ++i) input_.key_press(VK_BACK, 25);
        // The whole stack. When the count cannot be read, more than any
        // stack holds: the box takes what is there.
        input_.type_text(std::to_string(count > 0 ? count : 9999));
        wait_ms(150);
    }

    // Xác Nhận is the left half of the pair.
    input_.click_at(button.at.x + button.at.w / 4, button.at.cy());
    for (int i = 0; i < 10; ++i) {
        if (!wait_ms(150)) return false;
        if (grab_ && grab_(f) && f.valid() &&
            !TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *confirm,
                                   kButtonScore).found)
            return true;
    }
    say("hộp thoại không đóng lại");
    return false;
}

} // namespace gobot
