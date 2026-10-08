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

/// The dialogs each way of getting rid of something brings up. Into the
/// trash: "do you really want to put it in the trash?" for one item, "how many
/// to put in the trash?" for a stack. Onto the floor: the drop confirmation,
/// and the quantity box it shares with selling. Kept apart so that an item
/// meant for the trash that lands outside the bag -- and gets asked whether it
/// should be dropped -- is never dropped by accident.
const char* const kDeleteDialogs[] = { "grabbed/del_item_confirm_1",
                                       "grabbed/del_item_confirm_2" };
const char* const kDropDialogs[]   = { "grabbed/drop_confirm", "grabbed/sell_confirm" };

/// A message with only Xác Nhận under it: what the client says when it
/// refuses, a wrong quantity for one.
const char* const kNotice = "grabbed/dialog_ok";

/// How alike the strip has to be. Lower than a fixed button: whichever tab is
/// selected is drawn differently, and the picture was taken with the first.
constexpr float kStripScore = 0.62f;
constexpr float kTrashScore = 0.80f;
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
    bin_dx         = ini.get_int("Bag", "bin_dx", bin_dx);
    bin_dy         = ini.get_int("Bag", "bin_dy", bin_dy);
    bin_w          = ini.get_int("Bag", "bin_w", bin_w);
    bin_h          = ini.get_int("Bag", "bin_h", bin_h);
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

BagView find(const Frame& f, const TemplateStore& store, const BagLayout& l) {
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

    // The bin is where the layout says. The can is only drawn in it while it
    // is empty, so looking for the can says whether it is -- looked for right
    // there and nowhere else.
    v.trash = ScreenRect{ v.strip.x + l.bin_dx, v.strip.y + l.bin_dy, l.bin_w, l.bin_h };
    const ScreenRect around = clamp_to(f, ScreenRect{ v.trash.x - 4, v.trash.y - 4,
                                                      v.trash.w + 8, v.trash.h + 8 });
    for (const char* name : kTrashCans) {
        const Frame* t = store.get(name);
        if (t == nullptr || around.w < t->width || around.h < t->height) continue;
        const auto m = TemplateMatcher::find(f, around, *t, kTrashScore);
        if (m.found) {
            v.bin_empty = true;
            v.trash = m.at;
            break;
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

bool same_page(const Frame& a, const BagView& va, const Frame& b, const BagView& vb,
               const BagLayout& l) {
    const auto sa = va.slots(l), sb = vb.slots(l);
    if (sa.size() != sb.size() || sa.empty()) return false;
    std::size_t alike = 0;
    for (std::size_t i = 0; i < sa.size(); ++i) {
        // The middle of each slot, coarsely: enough to tell items apart, too
        // coarse to notice a glint.
        long da = 0, db = 0;
        int n = 0;
        for (int y = 8; y < l.cell - 8; y += 2)
            for (int x = 8; x < l.cell - 8; x += 2) {
                da += lum(a, sa[i].x + x, sa[i].y + y);
                db += lum(b, sb[i].x + x, sb[i].y + y);
                ++n;
            }
        if (n > 0 && std::abs(da - db) / n <= 6) ++alike;
    }
    return alike * 10 >= sa.size() * 9;
}

} // namespace bag

// ---------------------------------------------------------------- counts

bool CountReader::load(const std::string& font_dir) {
    loaded_ = false;
    int got = 0;
    for (int d = 0; d < 10; ++d) {
        Frame f;
        if (!load_png(font_dir + "\\lower\\" + std::to_string(d) + ".png", f) || !f.valid())
            continue;
        std::vector<std::uint8_t> on(static_cast<std::size_t>(f.width) * f.height, 0);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x)
                on[static_cast<std::size_t>(y) * f.width + x] = lum(f, x, y) > 128 ? 1 : 0;
        set_digit(d, f.width, f.height, on);
        ++got;
    }
    loaded_ = got == 10;
    return loaded_;
}

void CountReader::set_digit(int d, int w, int h, const std::vector<std::uint8_t>& on) {
    if (d < 0 || d > 9 || w <= 0 || h <= 0 ||
        on.size() != static_cast<std::size_t>(w) * h) return;
    Glyph& g = digits_[d];
    g.w = w;
    g.h = h;
    g.on = on;
    g.lit = 0;
    for (const auto v : on) g.lit += v ? 1 : 0;
    loaded_ = true;
    for (const Glyph& each : digits_)
        if (each.w == 0) loaded_ = false;
}

int CountReader::read(const Frame& f, const ScreenRect& slot) const {
    if (!loaded_ || !f.valid()) return 0;

    // The digits are white and the item under them is not: bright and grey
    // picks out the one from the other, even over a white potion, because
    // the digits carry a dark outline that keeps them apart.
    constexpr int kW = 28, kH = 16;
    std::uint8_t white[kH][kW] = {};
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            std::uint8_t r, g, b;
            f.get(slot.x + x, slot.y + y, r, g, b);
            const int lo = std::min({ r, g, b }), hi = std::max({ r, g, b });
            white[y][x] = (r + g + b) / 3 > 175 && hi - lo < 40 ? 1 : 0;
        }

    // How well digit `d` sits at (x, y): the share of its box that agrees,
    // and the share of its strokes that are there.
    const auto fit = [&](int d, int x, int y, double& agree, double& strokes) {
        const Glyph& g = digits_[d];
        agree = strokes = 0.0;
        if (x + g.w > kW || y + g.h > kH || g.lit == 0) return;
        int same = 0, hit = 0;
        for (int gy = 0; gy < g.h; ++gy)
            for (int gx = 0; gx < g.w; ++gx) {
                const std::uint8_t want = g.on[static_cast<std::size_t>(gy) * g.w + gx];
                const std::uint8_t have = white[y + gy][x + gx];
                if (want == have) ++same;
                if (want && have) ++hit;
            }
        agree = static_cast<double>(same) / (g.w * g.h);
        strokes = static_cast<double>(hit) / g.lit;
    };

    // Left to right along each starting row; the row that reads the most
    // digits wins.
    std::string best;
    for (int y = 0; y + 8 <= kH && y < 7; ++y) {
        std::string text;
        int x = 0, misses = 0;
        while (x < kW - 2 && text.size() < 5) {
            int found = -1;
            double found_agree = 0.0;
            for (int d = 0; d < 10; ++d) {
                double agree = 0.0, strokes = 0.0;
                fit(d, x, y, agree, strokes);
                if (agree >= 0.88 && strokes >= 0.88 && agree > found_agree) {
                    found = d;
                    found_agree = agree;
                }
            }
            if (found >= 0) {
                text += static_cast<char>('0' + found);
                x += digits_[found].w + 1;
                misses = 0;
            } else {
                if (!text.empty() && ++misses > 2) break;
                ++x;
            }
        }
        if (text.size() > best.size()) best = text;
    }
    if (best.empty() || best.size() > 4) return 0;
    return std::atoi(best.c_str());
}

// --------------------------------------------------------------- catalog

namespace {

/// An icon drawn into the middle of an empty slot, the way the bag shows it.
Frame slot_tile(const Frame& icon) {
    Frame t;
    t.width = 34;
    t.height = 34;
    t.pixels.assign(34u * 34u * 4u, 255);
    for (std::size_t i = 0; i < t.pixels.size(); i += 4)
        t.pixels[i] = t.pixels[i + 1] = t.pixels[i + 2] = 16;
    const int ox = (34 - icon.width) / 2, oy = (34 - icon.height) / 2;
    for (int y = 0; y < icon.height; ++y)
        for (int x = 0; x < icon.width; ++x) {
            const std::size_t s = (static_cast<std::size_t>(y) * icon.width + x) * 4;
            if (icon.pixels[s + 3] < 128) continue;
            const int tx = ox + x, ty = oy + y;
            if (tx < 0 || ty < 0 || tx >= 34 || ty >= 34) continue;
            const std::size_t d = (static_cast<std::size_t>(ty) * 34 + tx) * 4;
            t.pixels[d] = icon.pixels[s];
            t.pixels[d + 1] = icon.pixels[s + 1];
            t.pixels[d + 2] = icon.pixels[s + 2];
        }
    return t;
}

} // namespace

std::string ItemCatalog::default_dir() {
    const std::string staged = TemplateStore::default_dir();
    if (!staged.empty() &&
        GetFileAttributesA((staged + "\\spend").c_str()) != INVALID_FILE_ATTRIBUTES)
        return staged;
    return TemplateStore::client_images();
}

bool ItemCatalog::load(const std::string& images_dir) {
    groups.clear();
    if (images_dir.empty()) return false;
    const struct { const char* folder; const char* name; } kGroups[] = {
        { "spend/fragment", "Mảnh ghép" },
        { "spend/craft",    "Nguyên liệu" },
        { "spend/hp",       "Hồi máu" },
        { "spend/mana",     "Hồi mana" },
        { "spend/buff",     "Bùa" },
        { "seal",           "Ấn" },
        { "other",          "Khác" },
    };
    for (const auto& k : kGroups) {
        Group group;
        group.folder = k.folder;
        group.name = k.name;
        std::string dir = images_dir + "\\" + k.folder;
        std::replace(dir.begin(), dir.end(), '/', '\\');

        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((dir + "\\*.png").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            Entry e;
            std::string file = fd.cFileName;
            if (!load_png(dir + "\\" + file, e.icon) || !e.icon.valid()) continue;
            e.id = std::string(k.folder) + "/" + file.substr(0, file.size() - 4);
            e.tile = slot_tile(e.icon);
            group.entries.push_back(std::move(e));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        std::sort(group.entries.begin(), group.entries.end(),
                  [](const Entry& a, const Entry& b) { return a.id < b.id; });
        if (!group.entries.empty()) groups.push_back(std::move(group));
    }
    return !groups.empty();
}

const ItemCatalog::Entry* ItemCatalog::find(const std::string& id) const {
    for (const Group& g : groups)
        for (const Entry& e : g.entries)
            if (e.id == id) return &e;
    return nullptr;
}

std::size_t ItemCatalog::size() const {
    std::size_t n = 0;
    for (const Group& g : groups) n += g.entries.size();
    return n;
}

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

void TrashList::add_catalog(const ItemCatalog& catalog, const std::vector<std::string>& ids,
                            Action a, float min_score) {
    for (const std::string& id : ids) {
        const ItemCatalog::Entry* e = catalog.find(id);
        if (e == nullptr) continue;
        Item item;
        item.id = id;
        item.action = a;
        item.min_score = min_score;

        // The icon as it is, with its own transparency, less the corner the
        // count is printed over. The icon sits about two pixels into the slot,
        // so the slot's count corner is this much of the icon.
        item.picture = e->icon;
        for (int y = 0; y < item.picture.height; ++y)
            for (int x = 0; x < item.picture.width; ++x) {
                std::uint8_t& alpha =
                    item.picture.pixels[(static_cast<std::size_t>(y) * item.picture.width + x) * 4 + 3];
                if (x < 20 && y < 11) alpha = 0;
            }

        item.shown = e->tile.valid() ? e->tile : slot_tile(e->icon);
        items.push_back(std::move(item));
    }
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
        const float need = items[i].min_score > 0.0f ? items[i].min_score : min_score;
        const auto m = TemplateMatcher::find(f, area, p, need,
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
    catalog_score    = std::clamp(ini.get_float("Trash", "catalog_score", catalog_score), 0.70f, 0.99f);

    const auto ids = [&](const char* key) {
        std::vector<std::string> out;
        std::string all = ini.get("Items", key, ""), one;
        all += ',';
        for (const char c : all) {
            if (c == ',' || c == ' ') {
                if (!one.empty()) out.push_back(one);
                one.clear();
            } else {
                one += c;
            }
        }
        return out;
    };
    delete_ids = ids("delete");
    drop_ids   = ids("drop");
    layout.load(ini);
    return true;
}

// ---------------------------------------------------------------- cleaner

BagCleaner::BagCleaner(InputManager& input, const TemplateStore& store, Grab grab)
    : input_(input), store_(store), grab_(std::move(grab)) {}

void BagCleaner::keep(const Frame& f, const char* what) {
    if (debug_dir_.empty() || !f.valid()) return;
    CreateDirectoryA(debug_dir_.c_str(), nullptr);

    // A few dozen at most: the newest are the ones that matter.
    std::vector<std::string> old;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((debug_dir_ + "\\*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { old.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(old.begin(), old.end());
    for (std::size_t i = 0; old.size() >= 30 && i + 29 < old.size(); ++i)
        DeleteFileA((debug_dir_ + "\\" + old[i]).c_str());

    static int n = 0;
    const std::string path = debug_dir_ + "\\" + stamp() + "_" + std::to_string(n++ % 100) +
                             "_" + what + ".png";
    if (save_png(f, path)) GB_INFO("Trash: kept the screen as %s", path.c_str());
}

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
    v = bag::find(f, store_, settings_.layout);
    return v.found;
}

void BagCleaner::scroll_to_top(BagView& v) {
    const BagLayout& l = settings_.layout;
    Frame f;
    BagView now;
    if (!grab_view(f, now)) return;
    for (int i = 0; i < 15 && !cancelled_; ++i) {
        input_.click_at(now.strip.x + l.scroll_up_dx, now.strip.y + l.scroll_up_dy);
        Frame g;
        BagView after;
        if (!wait_ms(160) || !grab_view(g, after)) break;
        const bool moved = !bag::same_page(f, now, g, after, l);
        f = std::move(g);
        now = after;
        if (!moved) break;
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

    // Something left waiting in the bin -- by a pass that stopped half way,
    // or by hand -- goes back first. Otherwise the next item dropped on the bin
    // is dropped on that one.
    if (!v.bin_empty && !dry_run_ && !clear_bin()) {
        res.trouble = "thùng rác đang có đồ chờ xoá và không trả lại được";
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
        int scrolls = 0;
        for (int round = 0; round < 120 && !stop && !cancelled_; ++round) {
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

                const int count = counts_ ? counts_->read(f, slots[s]) : 0;
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
            // changing anything. Bounded too, in case it never seems to stop.
            if (++scrolls > 15) break;
            input_.click_at(v.strip.x + l.scroll_down_dx, v.strip.y + l.scroll_down_dy);
            if (!wait_ms(200)) break;
            Frame g;
            BagView w;
            if (!grab_view(g, w) || bag::same_page(f, v, g, w, l)) break;
        }
    }

    if (opened_here) input_.key_press(settings_.bag_key);
    return res;
}

bool BagCleaner::clear_bin() {
    Frame f;
    BagView v;
    if (!grab_view(f, v)) return false;
    if (v.bin_empty) return true;

    const Frame* confirm = store_.get(kConfirm);
    if (confirm == nullptr) return false;
    input_.click_at(v.trash.cx(), v.trash.cy());
    for (int i = 0; i < 10; ++i) {
        if (!wait_ms(150) || !grab_ || !grab_(f) || !f.valid()) continue;
        const auto m = TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *confirm,
                                             kButtonScore);
        if (m.found) {
            // Hủy Bỏ, the right half of the pair: the item goes back.
            input_.click_at(m.at.x + m.at.w * 3 / 4, m.at.cy());
            break;
        }
    }
    wait_ms(400);
    if (!grab_view(f, v)) return false;
    if (!v.bin_empty) keep(f, "bin_stuck");
    return v.bin_empty;
}

bool BagCleaner::trash_one(Frame& f, BagView& v, int index, const TrashList::Item& item,
                           int count) {
    const BagLayout& l = settings_.layout;
    const auto slots = v.slots(l);
    const ScreenRect& s = slots[static_cast<std::size_t>(index)];

    if (!v.bin_empty && !clear_bin()) {
        say("thùng rác đang có đồ chờ xoá và không trả lại được");
        return false;
    }

    // 1. Onto the bin. It takes the item and waits, showing it where the can
    //    was, and leaves a faded copy in the slot.
    bool moved = false;
    if (settings_.click_to_move)
        moved = input_.click_at(s.cx(), s.cy()) && wait_ms(220) &&
                input_.click_at(v.trash.cx(), v.trash.cy());
    else
        moved = input_.drag(s.cx(), s.cy(), v.trash.cx(), v.trash.cy());
    if (!moved) {
        say("không điều khiển được chuột trong cửa sổ game");
        return false;
    }

    Frame g;
    BagView w;
    bool waiting = false;
    for (int i = 0; i < 8 && !waiting; ++i) {
        if (!wait_ms(150)) return false;
        waiting = grab_view(g, w) && !w.bin_empty;
    }
    if (!waiting) {
        say("món đồ không vào thùng rác");
        keep(g, "not_in_bin");
        return false;
    }

    // 2. A click on the bin, which is what asks -- and 3. the answer.
    input_.click_at(w.trash.cx(), w.trash.cy());
    const int answered = answer_dialog(TrashList::Action::Delete, count);
    if (answered != kDone) {
        if (answered == kNoDialog) say("bấm vào thùng rác mà không thấy hộp thoại xác nhận");
        // Whatever is still open is said no to, and the item goes back.
        clear_bin();
        return false;
    }

    // 4. Only gone counts: the bin empty again, and the slot not holding it.
    wait_ms(300);
    if (!grab_view(g, w)) return false;
    if (!w.bin_empty) {
        say("thùng rác vẫn còn món đồ");
        keep(g, "still_in_bin");
        clear_bin();
        return false;
    }
    const ScreenRect& same = w.slots(l)[static_cast<std::size_t>(index)];
    if (!bag::is_empty(g, same, l)) {
        const int still = trash_->match(g, same, settings_.min_score);
        if (still >= 0 && &trash_->items[static_cast<std::size_t>(still)] == &item) {
            say("món đồ vẫn còn trong túi");
            keep(g, "still_there");
            return false;
        }
    }
    f = g;
    v = w;
    return true;
}

bool BagCleaner::rid(Frame& f, BagView& v, int index, const TrashList::Item& item, int count) {
    const BagLayout& l = settings_.layout;
    const auto slots = v.slots(l);
    if (index < 0 || index >= static_cast<int>(slots.size())) return false;
    if (item.action == TrashList::Action::Delete) return trash_one(f, v, index, item, count);
    const ScreenRect& s = slots[static_cast<std::size_t>(index)];
    const int fx = s.cx(), fy = s.cy();

    // Onto the ground, clear of the bag: whichever side has more room.
    const int room_left = v.strip.x;
    const int room_right = f.width - (v.strip.x + kBagWidth);
    const int tx = room_left > room_right ? std::max(40, v.strip.x - 160)
                                          : std::min(f.width - 40, v.strip.x + kBagWidth + 160);
    const int ty = std::clamp(v.strip.y + 40, 140, std::max(140, f.height - 160));

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

    // A drop has to be asked about and answered to count. An item that has
    // merely left its slot may be waiting somewhere -- which is exactly how the
    // bin behaves, and taking that for done is what made the first version
    // report items gone that were not.
    const int answered = answer_dialog(item.action, count);
    if (answered != kDone) {
        if (answered == kNoDialog) say("không thấy hộp thoại vứt đồ");
        input_.key_press(VK_ESCAPE);
        wait_ms(300);
        return false;
    }

    wait_ms(350);
    Frame g;
    BagView w;
    if (!grab_view(g, w)) return false;
    const ScreenRect& same = w.slots(l)[static_cast<std::size_t>(index)];
    if (!bag::is_empty(g, same, l)) {
        const int still = trash_->match(g, same, settings_.min_score);
        if (still >= 0 && &trash_->items[static_cast<std::size_t>(still)] == &item) {
            say("món đồ vẫn còn trong túi");
            keep(g, "still_there");
            return false;
        }
    }
    f = g;
    v = w;
    return true;
}

int BagCleaner::answer_dialog(TrashList::Action action, int count) {
    const Frame* confirm = store_.get(kConfirm);
    if (confirm == nullptr) {
        say("thiếu hình nút Xác Nhận, nên không bấm gì");
        return kFailed;
    }
    const Frame* notice = store_.get(kNotice);
    const bool deleting = action == TrashList::Action::Delete;

    // One dialog or a short run of them -- an "are you sure" and then the
    // quantity, or the other way round. Each is checked before it is answered.
    Frame f;
    for (int round = 0; round < 3; ++round) {
        TemplateMatcher::Match button;
        const int tries = round == 0 ? 14 : 6;
        for (int i = 0; i < tries && !button.found; ++i) {
            if (!wait_ms(150)) return kFailed;
            if (!grab_ || !grab_(f) || !f.valid()) continue;
            button = TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *confirm,
                                           kButtonScore);
        }
        if (!button.found) {
            if (round == 0) {
                keep(f, "no_dialog");
                return kNoDialog;
            }
            return kDone;               // the last one has closed and nothing followed
        }

        // Ours, and the right one of ours: an item meant for the trash that was
        // let go outside the bag gets asked whether to drop it, and must not be.
        const ScreenRect above = clamp_to(f, ScreenRect{ button.at.x - 210, button.at.y - 95,
                                                         button.at.w + 230, button.at.h + 105 });
        bool ours = false;
        const auto look = [&](const char* const* names, std::size_t n) {
            for (std::size_t i = 0; i < n && !ours; ++i) {
                const Frame* t = store_.get(names[i]);
                if (t == nullptr || above.w < t->width || above.h < t->height) continue;
                ours = TemplateMatcher::find(f, above, *t, kDialogScore).found;
            }
        };
        if (deleting) look(kDeleteDialogs, std::size(kDeleteDialogs));
        else          look(kDropDialogs, std::size(kDropDialogs));
        if (!ours) {
            say(deleting ? "hộp thoại này không phải hộp thoại xoá đồ, nên không bấm"
                         : "hộp thoại này không phải hộp thoại vứt đồ, nên không bấm");
            keep(f, "wrong_dialog");
            return kFailed;
        }

        // The dialogs for a stack have a quantity box left of the buttons:
        // dark, where the others are the dialog's pale grey. It takes the
        // exact number in the stack and nothing else, so an unreadable count
        // is a stop, not a guess.
        const ScreenRect box = clamp_to(f, ScreenRect{ button.at.x - 88, button.at.y + 3,
                                                       82, std::max(4, button.at.h - 6) });
        long total = 0;
        int n = 0;
        for (int y = box.y; y < box.y + box.h; ++y)
            for (int x = box.x; x < box.x + box.w; ++x) { total += lum(f, x, y); ++n; }
        if (n > 0 && total / n < 100) {
            if (count <= 0) {
                say("không đọc được số lượng trên ô đồ, nên bỏ qua món này");
                keep(f, "no_count");
                return kFailed;
            }
            input_.click_at(box.cx(), box.cy());
            wait_ms(120);
            input_.key_press(VK_END, 25);
            for (int i = 0; i < 6; ++i) input_.key_press(VK_BACK, 25);
            input_.type_text(std::to_string(count));
            wait_ms(200);
        }

        // Xác Nhận is the left half of the pair.
        input_.click_at(button.at.x + button.at.w / 4, button.at.cy());

        // Wait for it to close. A refusal -- a message with only Xác Nhận
        // under it -- is answered and counted as a failure.
        bool closed = false;
        for (int i = 0; i < 12 && !closed; ++i) {
            if (!wait_ms(150)) return kFailed;
            if (!grab_ || !grab_(f) || !f.valid()) continue;
            const ScreenRect all{ 0, 0, f.width, f.height };
            const bool pair = TemplateMatcher::find(f, all, *confirm, kButtonScore).found;
            if (notice != nullptr && !pair) {
                const auto m = TemplateMatcher::find(f, all, *notice, 0.75f);
                if (m.found) {
                    keep(f, "refused");
                    input_.click_at(m.at.x + m.at.w - 40, m.at.y + m.at.h - 14);
                    say("game không nhận, đã đóng thông báo");
                    return kFailed;
                }
            }
            closed = !pair;
        }
        if (!closed) {
            say("hộp thoại không đóng lại");
            keep(f, "stuck");
            return kFailed;
        }
    }
    return kDone;
}

} // namespace gobot
