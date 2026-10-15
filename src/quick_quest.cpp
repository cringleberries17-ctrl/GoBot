#include "gobot/quick_quest.hpp"
#include "gobot/alerts.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

#include <windows.h>

namespace gobot {

namespace {

// ---- text -------------------------------------------------------------------

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// printf into a string.
template <typename... Args>
std::string format(const char* fmt, Args... args) {
    char buf[512];
    std::snprintf(buf, sizeof buf, fmt, args...);
    return buf;
}

/// A score as a person reads it: "97%".
std::string percent(float score) {
    return format("%.0f%%", static_cast<double>(std::max(0.0f, score)) * 100.0);
}

// ---- the file ---------------------------------------------------------------

/// What a quest.ini starts as when the tab has to make one. The shipped file
/// says more; this is enough to be read back.
const char* const kNewFile =
    "# Làm nhiệm vụ nhanh: thẻ Nhiệm vụ trên màn hình đơn giản ghi file này;\n"
    "# file quest.ini đi kèm GoBot giải thích từng dòng.\n"
    "\n"
    "[Quest]\n"
    "enabled=false\n";

/// Sections of keys, both in lower case, as the file has them.
using IniMap = std::map<std::string, std::map<std::string, std::string>>;

/// Read here rather than by IniFile::load, which wants its sections and keys
/// in exactly one case and is blinded by a BOM in front of the first line --
/// Notepad's "UTF-8 with BOM" -- and a hand-edited file may have both.
bool read_ini(const std::string& path, IniMap& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (all.compare(0, 3, "\xEF\xBB\xBF") == 0) all.erase(0, 3);
    std::string section;
    std::size_t at = 0;
    while (at <= all.size()) {
        const std::size_t end = all.find('\n', at);
        const std::string line =
            trim(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
        at = end == std::string::npos ? all.size() + 1 : end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close != std::string::npos) section = lower(trim(line.substr(1, close - 1)));
            out[section];
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        if (!key.empty()) out[section][key] = trim(line.substr(eq + 1));
    }
    return true;
}

bool parse_bool(const std::string& v, bool& out) {
    const std::string s = lower(trim(v));
    if (s == "true" || s == "1" || s == "yes" || s == "on") { out = true; return true; }
    if (s == "false" || s == "0" || s == "no" || s == "off") { out = false; return true; }
    return false;
}

/// A whole number in base ten, whatever its leading zeros, and nothing after it.
bool parse_int(const std::string& v, int& out) {
    const std::string t = trim(v);
    if (t.empty()) return false;
    char* end = nullptr;
    const long n = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0' || n < -1000000 || n > 1000000) return false;
    out = static_cast<int>(n);
    return true;
}

/// A decimal with a point or a comma.
bool parse_float(const std::string& v, float& out) {
    std::string t = trim(v);
    std::replace(t.begin(), t.end(), ',', '.');
    if (t.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(d)) return false;
    out = static_cast<float>(d);
    return true;
}

/// Numbers separated by commas: "10,32,302,28".
bool parse_ints(const std::string& v, int* out, std::size_t count) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t comma = v.find(',', at);
        const bool last = i + 1 == count;
        if (last != (comma == std::string::npos)) return false;
        if (!parse_int(v.substr(at, last ? std::string::npos : comma - at), out[i])) return false;
        at = comma + 1;
    }
    return true;
}

bool parse_rect(const std::string& v, ScreenRect& out) {
    int n[4];
    if (!parse_ints(v, n, 4) || n[2] <= 0 || n[3] <= 0 || n[2] > 4000 || n[3] > 4000) return false;
    out = ScreenRect{ n[0], n[1], n[2], n[3] };
    return true;
}

bool parse_point(const std::string& v, int& x, int& y) {
    int n[2];
    if (!parse_ints(v, n, 2)) return false;
    x = n[0];
    y = n[1];
    return true;
}

/// Names separated by commas, blanks trimmed, empty ones left out.
std::vector<std::string> parse_list(const std::string& v) {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t comma = v.find(',', at);
        const std::string one = trim(v.substr(at, comma == std::string::npos ? std::string::npos
                                                                             : comma - at));
        if (!one.empty()) out.push_back(one);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return out;
}

/// A key that may open the window: never Enter, which answers whatever dialog
/// is up, nor Esc, nor the mouse's buttons.
bool usable_key(std::uint32_t vk) {
    return vk >= 0x08 && vk <= 0xFE && vk != VK_RETURN && vk != VK_ESCAPE;
}

// ---- pictures ----------------------------------------------------------------

/// The folder a picture's name is in: "quest/1" for "quest/1/Dlg_Quest_Neo".
std::string folder_of(const std::string& name) {
    const auto at = name.find_last_of('/');
    return at == std::string::npos ? std::string() : name.substr(0, at);
}

/// Part of a picture, as it is, transparency and all.
Frame cut(const Frame& f, const ScreenRect& r) {
    Frame out;
    if (!f.valid() || r.x < 0 || r.y < 0 || r.right() > f.width || r.bottom() > f.height ||
        !r.valid())
        return out;
    out.width = r.w;
    out.height = r.h;
    out.pixels.resize(static_cast<std::size_t>(r.w) * r.h * 4);
    for (int y = 0; y < r.h; ++y)
        std::copy_n(f.pixels.begin() + ((static_cast<std::size_t>(r.y + y) * f.width + r.x) * 4),
                    static_cast<std::size_t>(r.w) * 4,
                    out.pixels.begin() + static_cast<std::size_t>(y) * r.w * 4);
    return out;
}

/// The title bar: the top rows, less the corners, which are rounded and show
/// whatever is behind the window. Cut with no transparency left in it, the
/// whole screen is searched for it four times faster than with a mask.
Frame title_strip(const Frame& window, const QuestSettings& s) {
    if (window.height < 8) return {};
    const int inset = window.width > 2 * s.title_inset + 32 ? std::max(0, s.title_inset) : 0;
    const int rows = std::clamp(s.title_rows, 8, window.height);
    return cut(window, ScreenRect{ inset, 0, window.width - 2 * inset, rows });
}

/// `f` with `hole` made see-through: Tự Động without its box, which is read
/// on its own, so that finding it does not depend on whether it is ticked.
Frame with_hole(const Frame& f, const ScreenRect& hole) {
    Frame out = f;
    for (int y = std::max(0, hole.y); y < std::min(f.height, hole.bottom()); ++y)
        for (int x = std::max(0, hole.x); x < std::min(f.width, hole.right()); ++x)
            out.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4 + 3] = 0;
    return out;
}

using PictureGetter = std::function<bool(const std::string& name, Frame& out)>;

void fill(QuestPictures& p, const QuestSettings& s, const PictureGetter& get) {
    p = QuestPictures{};
    // Needed: missing ones are named. The English client's are only for
    // telling its window apart, and are not.
    const auto need = [&](const std::string& name, Frame& out) {
        if (get(name, out) && out.valid()) return true;
        p.missing.push_back(name);
        out = Frame{};
        return false;
    };
    const auto may = [&](const std::string& name, Frame& out) {
        if (get(name, out) && out.valid()) return true;
        out = Frame{};
        return false;
    };

    Frame window;
    if (need(s.window, window)) {
        p.window_w = window.width;
        p.window_h = window.height;
        p.title = title_strip(window, s);
    }
    const std::string folder = folder_of(s.window);
    for (const std::string& tab : s.tabs) {
        std::array<Frame, 2> looks;
        need(folder + "/" + tab + "/0", looks[0]);
        need(folder + "/" + tab + "/1", looks[1]);
        p.tabs.push_back(std::move(looks));
    }
    Frame option;
    if (need(s.auto_option, option)) p.auto_option = with_hole(option, s.auto_box);
    need(s.box_on, p.box_on);
    need(s.box_off, p.box_off);

    Frame english;
    if (!s.window_en.empty() && may(s.window_en, english) && english.width == window.width &&
        english.height == window.height) {
        std::vector<std::array<Frame, 2>> tabs;
        bool all = true;
        const std::string en_folder = folder_of(s.window_en);
        for (const std::string& tab : s.tabs) {
            std::array<Frame, 2> looks;
            all = may(en_folder + "/" + tab + "/0", looks[0]) && all;
            all = may(en_folder + "/" + tab + "/1", looks[1]) && all;
            tabs.push_back(std::move(looks));
        }
        if (all) {
            p.title_en = title_strip(english, s);
            p.tabs_en = std::move(tabs);
        }
    }
}

// ---- likeness ------------------------------------------------------------------

/// Grey as OpenCV makes it, so a score here means what a TemplateMatcher score
/// means.
inline double grey_at(const std::uint8_t* px) {
    return 0.114 * px[0] + 0.587 * px[1] + 0.299 * px[2];
}

/// Zero-mean normalised correlation of `tpl` with the frame at `x`, `y`, in
/// grey, over the template's opaque pixels; -1 when it does not fit or either
/// side is flat -- a flat patch correlates with everything and nothing.
float likeness_at(const Frame& f, int x, int y, const Frame& tpl) {
    if (!f.valid() || !tpl.valid()) return -1.0f;
    if (x < 0 || y < 0 || x + tpl.width > f.width || y + tpl.height > f.height) return -1.0f;
    double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
    long long n = 0;
    for (int ty = 0; ty < tpl.height; ++ty) {
        const std::uint8_t* trow = tpl.pixels.data() + static_cast<std::size_t>(ty) * tpl.width * 4;
        const std::uint8_t* frow =
            f.pixels.data() + (static_cast<std::size_t>(y + ty) * f.width + x) * 4;
        for (int tx = 0; tx < tpl.width; ++tx) {
            const std::uint8_t* t = trow + tx * 4;
            if (t[3] < TemplateMatcher::kOpaque) continue;
            const double a = grey_at(frow + tx * 4);
            const double b = grey_at(t);
            sa += a;
            sb += b;
            saa += a * a;
            sbb += b * b;
            sab += a * b;
            ++n;
        }
    }
    if (n < 24) return -1.0f;
    const double dn = static_cast<double>(n);
    const double va = saa - sa * sa / dn;
    const double vb = sbb - sb * sb / dn;
    // Roughly two grey levels of deviation is the floor, as for the matcher.
    if (va < dn * 4.0 || vb < dn * 4.0) return -1.0f;
    const double s = (sab - sa * sb / dn) / std::sqrt(va * vb);
    return std::isfinite(s) ? static_cast<float>(s) : -1.0f;
}

/// The tab row at `window`: the worst of its tabs, each in its better look.
float tab_row(const Frame& f, const ScreenRect& window,
              const std::vector<std::array<Frame, 2>>& tabs, const QuestSettings& s) {
    if (tabs.empty()) return -1.0f;
    float worst = 2.0f;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const int x = window.x + s.tabs_area.x + static_cast<int>(i) * s.tab_pitch;
        const int y = window.y + s.tabs_area.y;
        float best = -1.0f;
        for (const Frame& look : tabs[i]) best = std::max(best, quest_likeness(f, x, y, look, 2));
        worst = std::min(worst, best);
    }
    return worst;
}

ScreenRect clipped(const ScreenRect& r, const Frame& f) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.right()), y1 = std::min(f.height, r.bottom());
    return ScreenRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

ScreenRect inside(const ScreenRect& window, const ScreenRect& r) {
    return ScreenRect{ window.x + r.x, window.y + r.y, r.w, r.h };
}

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

/// The oldest PNGs of one kind in `dir` -- a probe's, or the rest -- deleted
/// until `keep` are left. Named by when they were taken, they sort oldest
/// first.
void prune_pngs(const std::string& dir, bool probes, std::size_t keep) {
    std::vector<std::string> old;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const bool probe = std::string(fd.cFileName).rfind("probe_", 0) == 0;
            if (probe == probes) old.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(old.begin(), old.end());
    for (std::size_t i = 0; i + keep < old.size(); ++i) DeleteFileA((dir + "\\" + old[i]).c_str());
}

constexpr std::size_t kKeptFrames = 20;

const char* const kEnglishVi = "Bản game tiếng Anh: chưa hỗ trợ (cần chọn nhiệm vụ như GoC)";

} // namespace

// ----------------------------------------------------------------- settings

int QuestSettings::clamp_interval(int minutes) {
    return std::clamp(minutes, kMinInterval, kMaxInterval);
}

bool QuestSettings::load(const std::string& path) {
    IniMap ini;
    if (!read_ini(path, ini) || ini.find("quest") == ini.end()) return false;
    QuestSettings s;
    const auto value = [&](const char* section, const char* key, std::string& out) {
        const auto sec = ini.find(section);
        if (sec == ini.end()) return false;
        const auto it = sec->second.find(key);
        if (it == sec->second.end()) return false;
        out = it->second;
        return true;
    };
    std::string v;
    const auto get_bool = [&](const char* sec, const char* key, bool& out) {
        bool b = out;
        if (value(sec, key, v) && parse_bool(v, b)) out = b;
    };
    const auto get_int = [&](const char* sec, const char* key, int& out, int lo, int hi) {
        int n = out;
        if (value(sec, key, v) && parse_int(v, n)) out = std::clamp(n, lo, hi);
    };
    const auto get_float = [&](const char* sec, const char* key, float& out, float lo, float hi) {
        float x = out;
        if (value(sec, key, v) && parse_float(v, x)) out = std::clamp(x, lo, hi);
    };
    const auto get_rect = [&](const char* sec, const char* key, ScreenRect& out) {
        ScreenRect r;
        if (value(sec, key, v) && parse_rect(v, r)) out = r;
    };
    const auto get_point = [&](const char* sec, const char* key, int& x, int& y) {
        int a = x, b = y;
        if (value(sec, key, v) && parse_point(v, a, b)) { x = a; y = b; }
    };
    const auto get_name = [&](const char* sec, const char* key, std::string& out) {
        if (value(sec, key, v) && !trim(v).empty()) out = trim(v);
    };

    get_bool("quest", "enabled", s.enabled);
    get_int("quest", "interval_min", s.interval_min, kMinInterval, kMaxInterval);
    get_int("quest", "first_delay_s", s.first_delay_s, 0, 600);
    if (value("quest", "quest_key", v)) {
        // A number, as the other files keep keys, or a key's name.
        const std::uint32_t vk = InputManager::parse_key(trim(v));
        if (usable_key(vk)) s.quest_key = vk;
    }
    get_int("quest", "max_failures", s.max_failures, 1, 20);
    get_bool("quest", "back_off", s.back_off);
    if (value("quest", "skin", v)) {
        const std::string which = lower(trim(v));
        if (which == "vi") s.skin = Skin::Vi;
        else if (which == "en") s.skin = Skin::En;
        else s.skin = Skin::Auto;
    }

    get_int("layout", "window_title_rows", s.title_rows, 8, 64);
    get_int("layout", "title_inset", s.title_inset, 0, 60);
    get_rect("layout", "tabs_area", s.tabs_area);
    get_int("layout", "tab_pitch", s.tab_pitch, 1, 400);
    get_point("layout", "tab_click", s.tab_click_x, s.tab_click_y);
    get_rect("layout", "auto_area", s.auto_area);
    get_point("layout", "auto_click", s.auto_click_x, s.auto_click_y);
    get_rect("layout", "auto_box", s.auto_box);
    get_int("layout", "hover_dx", s.hover_dx, 0, 100);
    get_rect("layout", "forbid_abandon", s.forbid_abandon);
    get_rect("layout", "forbid_hide_low", s.forbid_hide_low);
    get_rect("layout", "forbid_buttons", s.forbid_buttons);

    get_name("pictures", "window", s.window);
    get_name("pictures", "window_en", s.window_en);
    get_name("pictures", "auto", s.auto_option);
    get_name("pictures", "on", s.box_on);
    get_name("pictures", "off", s.box_off);
    if (value("pictures", "tabs", v)) {
        const auto names = parse_list(v);
        if (names.size() >= 2) s.tabs = names;
    }

    // Not below 0.60: GoC itself goes by 0.70, on edges.
    get_float("scores", "window", s.window_score, 0.60f, 0.99f);
    get_float("scores", "tabs", s.tab_score, 0.60f, 0.99f);
    get_float("scores", "auto", s.auto_score, 0.60f, 0.99f);
    get_float("scores", "state", s.state_score, 0.60f, 0.99f);
    get_float("scores", "state_margin", s.state_margin, 0.02f, 0.50f);

    get_int("timing", "open_hold_ms", s.open_hold_ms, 10, 500);
    get_int("timing", "close_hold_ms", s.close_hold_ms, 10, 500);
    get_int("timing", "open_wait_ms", s.open_wait_ms, 0, 5000);
    get_int("timing", "open_look_ms", s.open_look_ms, 0, 5000);
    get_int("timing", "tab_wait_ms", s.tab_wait_ms, 0, 5000);
    get_int("timing", "click_wait_ms", s.click_wait_ms, 0, 5000);
    get_int("timing", "hover_wait_ms", s.hover_wait_ms, 0, 5000);
    get_int("timing", "close_wait_ms", s.close_wait_ms, 0, 5000);
    get_int("timing", "round_limit_ms", s.round_limit_ms, 3000, 30000);

    *this = std::move(s);
    return true;
}

bool QuestSettings::save_key(const std::string& path, const std::string& section,
                             const std::string& key, const std::string& value) {
    {
        // Opened to add to, which makes the file when it is not there and
        // never empties one that is -- one a virus scan holds for a moment
        // included. Asked the same way the change below reads it, so a path
        // the two would read differently cannot pass for a missing file.
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) return false;
        out.seekp(0, std::ios::end);
        if (out.tellp() == std::streampos(0)) out << kNewFile;
        if (!out) return false;
    }
    return IniFile::set_in_place(path, section, key, value);
}

// ----------------------------------------------------------------- pictures

bool QuestPictures::ready() const {
    if (!missing.empty() || !title.valid() || tabs.empty() || !auto_option.valid() ||
        !box_on.valid() || !box_off.valid())
        return false;
    for (const auto& looks : tabs)
        if (!looks[0].valid() || !looks[1].valid()) return false;
    return true;
}

bool QuestPictures::knows_en() const {
    return title_en.valid() && !tabs_en.empty() && tabs_en.size() == tabs.size();
}

void QuestPictures::load(const TemplateStore& store, const QuestSettings& s) {
    fill(*this, s, [&](const std::string& name, Frame& out) {
        const Frame* f = store.get(name);
        if (f == nullptr) return false;
        out = *f;
        return true;
    });
}

bool QuestPictures::load_dir(const std::string& dir, const QuestSettings& s) {
    fill(*this, s, [&](const std::string& name, Frame& out) {
        std::string file = name;
        std::replace(file.begin(), file.end(), '/', '\\');
        return load_png(dir + "\\" + file + ".png", out);
    });
    return ready();
}

bool QuestPictures::load_best(const QuestSettings& s, std::string* from) {
    // The same order as TemplateStore::load_best: what the build staged
    // beside the program, then GoC's own folder. Only the dozen files needed,
    // rather than the thousand in either.
    QuestPictures first;
    bool tried = false;
    const std::string dirs[] = { TemplateStore::default_dir(), TemplateStore::client_images() };
    for (const std::string& dir : dirs) {
        if (dir.empty() || GetFileAttributesA(dir.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        QuestPictures p;
        if (p.load_dir(dir, s)) {
            *this = std::move(p);
            if (from) *from = dir;
            return true;
        }
        if (!tried) {
            first = std::move(p);
            tried = true;
            if (from) *from = dir;
        }
    }
    if (tried) {
        *this = std::move(first);
    } else {
        *this = QuestPictures{};
        missing.push_back(s.window);
    }
    return false;
}

std::string quest_missing_vi(const QuestPictures& p) {
    if (p.missing.empty()) return "Thiếu hình nhận diện cửa sổ Nhiệm Vụ — cài lại GoBot.";
    std::string list = "templates/" + p.missing[0] + ".png";
    if (p.missing.size() > 1) list += ", templates/" + p.missing[1] + ".png";
    if (p.missing.size() > 2) list += "…";
    return "Thiếu hình " + list + " — cài lại GoBot.";
}

// ------------------------------------------------------------------ looking

float quest_likeness(const Frame& f, int x, int y, const Frame& tpl, int radius) {
    float best = -1.0f;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
            best = std::max(best, likeness_at(f, x + dx, y + dy, tpl));
    return best;
}

QuestWindow find_quest_window(const Frame& f, const QuestPictures& p, const QuestSettings& s) {
    QuestWindow w;
    if (!f.valid() || !p.title.valid() || p.tabs.empty()) return w;
    const int inset = (p.window_w - p.title.width) / 2;
    const bool en_too = s.skin != QuestSettings::Skin::Vi && p.knows_en();
    // Every place the title bar fits, strongest first, from well under the
    // bar's own score: the best of them is worth saying even when it is not
    // the window.
    const auto bars = TemplateMatcher::find_all(f, ScreenRect{ 0, 0, f.width, f.height }, p.title,
                                                std::min(0.5f, s.window_score), 6);
    bool judged = false;
    for (const auto& bar : bars) {
        w.best_title = std::max(w.best_title, bar.score);
        if (bar.score < s.window_score) continue;
        ScreenRect at{ bar.at.x - inset, bar.at.y, p.window_w, p.window_h };
        // Whose tabs these are. Told apart by the tabs because the two
        // clients' title bars are one picture with other words, 0.96 alike.
        float vi = tab_row(f, at, p.tabs, s);
        float en = en_too ? tab_row(f, at, p.tabs_en, s) : -1.0f;
        const bool english = s.skin == QuestSettings::Skin::En || en > vi;
        // And where exactly, by that client's own bar: the other client's
        // matches best a few pixels off, its words being other words.
        float score = bar.score;
        const Frame& own = english && p.title_en.valid() ? p.title_en : p.title;
        int bx = bar.at.x, by = bar.at.y;
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx = -3; dx <= 3; ++dx) {
                const float here = likeness_at(f, bar.at.x + dx, bar.at.y + dy, own);
                if (here > score) {
                    score = here;
                    bx = bar.at.x + dx;
                    by = bar.at.y + dy;
                }
            }
        if (bx != bar.at.x || by != bar.at.y) {
            at.x = bx - inset;
            at.y = by;
            vi = tab_row(f, at, p.tabs, s);
            en = en_too ? tab_row(f, at, p.tabs_en, s) : -1.0f;
        }
        const float best = english ? std::max(en, vi) : vi;
        if (!judged || best >= s.tab_score) {
            // The first one judged is what a probe reports when none passes.
            judged = true;
            w.at = at;
            w.score = score;
            w.tabs = vi;
            w.tabs_en = en;
            w.english = english;
        }
        if (best >= s.tab_score) {
            w.found = true;
            return w;
        }
    }
    return w;
}

const char* auto_state_name(AutoState s) {
    switch (s) {
        case AutoState::On:  return "on";
        case AutoState::Off: return "off";
        default:             return "unknown";
    }
}

AutoState read_quest_box(const Frame& f, int x, int y, const QuestPictures& p,
                         const QuestSettings& s, float& on, float& off) {
    on = quest_likeness(f, x, y, p.box_on, 2);
    off = quest_likeness(f, x, y, p.box_off, 2);
    if (on >= s.state_score && on - off >= s.state_margin) return AutoState::On;
    if (off >= s.state_score && off - on >= s.state_margin) return AutoState::Off;
    return AutoState::Unknown;
}

AutoOption find_auto_option(const Frame& f, const ScreenRect& window, const QuestPictures& p,
                            const QuestSettings& s) {
    AutoOption o;
    if (!f.valid() || !p.auto_option.valid()) return o;
    const ScreenRect area = clipped(inside(window, s.auto_area), f);
    if (area.w < p.auto_option.width || area.h < p.auto_option.height) return o;
    // Looked for from well under its own score, so that how near it came is
    // there to report; the best of a few, not merely the first to pass.
    const auto all = TemplateMatcher::find_all(f, area, p.auto_option,
                                               std::min(0.5f, s.auto_score), 3);
    if (all.empty()) return o;
    const auto& m = all.front();
    o.at = m.at;
    o.score = m.score;
    if (m.score < s.auto_score) return o;
    o.found = true;
    o.state = read_quest_box(f, m.at.x + s.auto_box.x, m.at.y + s.auto_box.y, p, s, o.on, o.off);
    o.click_x = m.at.x + s.auto_click_x;
    o.click_y = m.at.y + s.auto_click_y;
    return o;
}

bool quest_click_ok(const ScreenRect& window, int x, int y, int client_w, int client_h,
                    const QuestSettings& s, std::string* why, std::string* why_vi) {
    const auto no = [why, why_vi](const char* text, const char* text_vi) {
        if (why) *why = text;
        if (why_vi) *why_vi = text_vi;
        return false;
    };
    if (x < 0 || y < 0 || x >= client_w || y >= client_h)
        return no("it is outside the game's client area", "ngoài khung hình của game");
    if (!window.contains(x, y)) return no("it is outside the quest window", "ngoài cửa sổ Nhiệm Vụ");
    // Two pixels round each, to spare: a click on the edge of a button is a
    // click on the button.
    const auto on = [&](const ScreenRect& r) {
        const ScreenRect at = inside(window, r);
        return ScreenRect{ at.x - 2, at.y - 2, at.w + 4, at.h + 4 }.contains(x, y);
    };
    if (on(s.forbid_abandon))
        return no("it is on Huy Nhiem Vu, which gives up a quest", "trúng nút Hủy Nhiệm Vụ");
    if (on(s.forbid_buttons))
        return no("it is where the quick quest's Accept, Give up and Complete buttons go",
                  "trúng chỗ các nút nhận, bỏ và trả nhiệm vụ");
    if (on(s.forbid_hide_low))
        return no("it is on the hide-low-level-quests checkbox",
                  "trúng ô Nhiệm Vụ Công Lực Thấp Không Hoạt Động");
    // Every tab but the last, NV Phụ.
    const int others = std::max(0, static_cast<int>(s.tabs.size()) - 1);
    if (others > 0 && on(ScreenRect{ s.tabs_area.x, s.tabs_area.y, others * s.tab_pitch - 2,
                                     s.tabs_area.h }))
        return no("it is on another tab", "trúng một thẻ khác");
    return true;
}

std::string quest_key_clash_vi(std::uint32_t quest_key, const std::vector<std::uint32_t>& own_keys) {
    for (const std::uint32_t own : own_keys)
        if (own != 0 && own == quest_key)
            return "phím mở cửa sổ Nhiệm Vụ (" + InputManager::key_name(quest_key) +
                   ") trùng phím bật/tắt của GoBot — chọn phím khác ở thẻ Nhiệm vụ";
    return {};
}

// -------------------------------------------------------------------- round

const char* quest_outcome_name(QuestOutcome o) {
    switch (o) {
        case QuestOutcome::TurnedOn:     return "turned_on";
        case QuestOutcome::AlreadyOn:    return "already_on";
        case QuestOutcome::NoWindow:     return "no_window";
        case QuestOutcome::NoAuto:       return "no_auto";
        case QuestOutcome::UnknownState: return "unknown_state";
        case QuestOutcome::ClickFailed:  return "click_failed";
        case QuestOutcome::Refused:      return "refused";
        case QuestOutcome::English:      return "english";
        case QuestOutcome::NoFrame:      return "no_frame";
        case QuestOutcome::NoPictures:   return "no_pictures";
        case QuestOutcome::Cancelled:    return "cancelled";
        case QuestOutcome::TooSlow:      return "too_slow";
        case QuestOutcome::NotNow:       return "not_now";
        case QuestOutcome::None:         break;
    }
    return "none";
}

bool QuestResult::done() const {
    return (outcome == QuestOutcome::TurnedOn || outcome == QuestOutcome::AlreadyOn) && !left_open;
}

bool QuestResult::failed() const {
    switch (outcome) {
        case QuestOutcome::NoWindow:
        case QuestOutcome::NoAuto:
        case QuestOutcome::UnknownState:
        case QuestOutcome::ClickFailed:
        case QuestOutcome::Refused:
        case QuestOutcome::TooSlow:
            return true;
        case QuestOutcome::TurnedOn:
        case QuestOutcome::AlreadyOn:
            return left_open;
        default:
            return false;
    }
}

std::string quest_result_vi(const QuestResult& r, const std::string& key) {
    std::string s;
    switch (r.outcome) {
        case QuestOutcome::TurnedOn:     s = "đã bật ô Tự Động"; break;
        case QuestOutcome::AlreadyOn:    s = "ô Tự Động đã bật sẵn"; break;
        case QuestOutcome::NoWindow:
            s = r.why.empty() ? "không mở được cửa sổ Nhiệm Vụ (đã bấm " + key + ")" : r.why;
            break;
        case QuestOutcome::NoAuto:       s = "không thấy ô Tự Động trong thẻ NV Phụ"; break;
        case QuestOutcome::UnknownState: s = "thấy ô Tự Động nhưng không rõ đang bật hay tắt"; break;
        case QuestOutcome::ClickFailed:  s = "đã bấm ô Tự Động nhưng không thấy nó bật"; break;
        case QuestOutcome::Refused:
            s = "không bấm" + (r.why.empty() ? std::string() : ": " + r.why);
            break;
        case QuestOutcome::English:      s = kEnglishVi; break;
        case QuestOutcome::NoFrame:      s = "không chụp được màn hình game"; break;
        case QuestOutcome::NoPictures:   s = "thiếu hình nhận diện cửa sổ Nhiệm Vụ"; break;
        case QuestOutcome::Cancelled:    s = "đã dừng giữa chừng"; break;
        case QuestOutcome::TooSlow:      s = "quá lâu không xong, dừng lại"; break;
        case QuestOutcome::NotNow:
            // Given up part way -- HP fell -- or never begun.
            s = std::string(r.keys_pressed + r.clicks > 0 ? "dừng giữa chừng" : "chưa làm") +
                (r.why.empty() ? std::string() : ": " + r.why);
            break;
        case QuestOutcome::None:         break;
    }
    if (r.left_open) s += "; cửa sổ Nhiệm Vụ vẫn mở";
    if (r.maybe_open) s += "; có thể đã mở một cửa sổ mà GoBot không nhận ra — xem trong game";
    return s;
}

std::string quest_trouble_why(const QuestResult& r) {
    switch (r.outcome) {
        case QuestOutcome::NoWindow:     return "không mở được cửa sổ Nhiệm Vụ";
        case QuestOutcome::NoAuto:       return "không thấy ô Tự Động trong thẻ NV Phụ";
        case QuestOutcome::UnknownState: return "không đọc được ô Tự Động đang bật hay tắt";
        case QuestOutcome::ClickFailed:  return "bấm ô Tự Động mà không thấy nó bật";
        case QuestOutcome::Refused:      return "không bấm được vào cửa sổ Nhiệm Vụ";
        case QuestOutcome::TooSlow:      return "quá lâu không xong";
        default: break;
    }
    return r.left_open ? "không đóng được cửa sổ Nhiệm Vụ" : "gặp lỗi";
}

QuestRound::QuestRound(const QuestPictures& pictures, const QuestSettings& settings, Hands hands)
    : pictures_(pictures), settings_(settings), hands_(std::move(hands)) {}

std::uint64_t QuestRound::clock() { return hands_.now ? hands_.now() : now_ms(); }

bool QuestRound::halted() {
    // A stop is asked about first: whoever stopped the round has taken over,
    // and says so themselves.
    if (halt_ == Halt::None) {
        if (cancelled_ || (hands_.stop && hands_.stop())) halt_ = Halt::Stop;
        else if (hands_.low_hp && hands_.low_hp()) halt_ = Halt::LowHp;
    }
    return halt_ != Halt::None;
}

void QuestRound::set_halted(QuestResult& r, const std::string& where) {
    if (halt_ == Halt::LowHp) {
        // Not a failure, and not the end of anything: the potion first, and
        // the round again once HP is back up.
        r.outcome = QuestOutcome::NotNow;
        r.why = "máu thấp";
        note(r, "HP fell under the potion's mark" + where + ": nothing more pressed, so that the "
                "potion can go");
    } else {
        r.outcome = QuestOutcome::Cancelled;
        note(r, "stopped" + where);
    }
    // Nothing more is pressed, so a window that was GoBot's to close may well
    // still be open.
    if (r.opened_by_us) r.left_open = true;
}

bool QuestRound::wait(int ms) {
    for (int slept = 0; slept < ms; slept += 30) {
        if (halted()) return false;
        const int step = std::min(30, ms - slept);
        if (hands_.sleep) hands_.sleep(step);
        else std::this_thread::sleep_for(std::chrono::milliseconds(step));
    }
    return !halted();
}

bool QuestRound::look(Frame& f) {
    if (!hands_.grab || !hands_.grab(f) || !f.valid()) return false;
    client_w_ = f.width;
    client_h_ = f.height;
    return true;
}

void QuestRound::note(QuestResult& r, const std::string& part) {
    r.log += (r.log.empty() ? "" : "; ") + part;
    if (report_) report_(part);
}

namespace {

std::string where(const ScreenRect& r) { return format("%d,%d", r.x, r.y); }

std::string window_words(const QuestWindow& w) {
    return format("%d,%d (title %.2f, tabs %.2f, %s)", w.at.x, w.at.y,
                  static_cast<double>(w.score),
                  static_cast<double>(w.english ? w.tabs_en : w.tabs), w.english ? "en" : "vi");
}

std::string box_words(const AutoOption& o) {
    return format("%s (on %.2f / off %.2f)", auto_state_name(o.state), static_cast<double>(o.on),
                  static_cast<double>(o.off));
}

} // namespace

bool QuestRound::press(QuestResult& r, int hold_ms) {
    if (r.keys_pressed >= 2 || !hands_.key) return false;
    if (!hands_.key(settings_.quest_key, hold_ms)) return false;
    ++r.keys_pressed;
    return true;
}

bool QuestRound::click(QuestResult& r, const ScreenRect& window, int x, int y, const char* what) {
    // Why not, in Vietnamese too: a place the game draws where GoBot will not
    // click stops every round the same way, and the tab says which.
    std::string why, why_vi;
    bool ok = false;
    if (r.clicks >= 2) {
        why = "this round has clicked twice already";
        r.why = "lần này đã bấm hai lần rồi";
    } else if (quest_click_ok(window, x, y, client_w_, client_h_, settings_, &why, &why_vi)) {
        ok = hands_.click && hands_.click(x, y);
        if (!ok) {
            why = "the click did not go -- the game is covered or not in front";
            r.why = "game bị che hoặc không ở trên cùng";
        }
    } else {
        r.why = "chỗ cần bấm " + why_vi;
    }
    if (ok) {
        ++r.clicks;
        return true;
    }
    note(r, format("refused to click %s at %d,%d: %s", what, x, y, why.c_str()));
    return false;
}

bool QuestRound::close(QuestResult& r) {
    if (!r.opened_by_us) return false;
    // A look first: the key opens the window as well as shutting it, and
    // pressed on a window that has gone by itself it would open it again.
    Frame f;
    if (!look(f)) {
        r.left_open = true;
        note(r, "could not look before closing the window, so it may be open");
        return false;
    }
    r.last = f;
    if (!find_quest_window(f, pictures_, settings_).found) {
        note(r, "the window closed by itself");
        return false;
    }
    // Nothing after a halt, not even this: whoever stopped the round has taken
    // over, and the potion goes before the window. Said as what became of
    // the round, rather than as the failure that brought it here.
    if (halted()) {
        set_halted(r, " with the window open");
        return true;
    }
    if (clock() - started_ > static_cast<std::uint64_t>(settings_.round_limit_ms)) {
        r.left_open = true;
        note(r, "out of time with the window open");
        return false;
    }
    const std::string key = InputManager::key_name(settings_.quest_key);
    if (!press(r, settings_.close_hold_ms)) {
        r.left_open = true;
        note(r, "could not press " + key + " to close the window");
        return false;
    }
    // Looked for every quarter of a second, close_wait_ms in all.
    for (int waited = 250;; waited += 250) {
        if (!wait(250)) {
            set_halted(r, " after pressing " + key + " to close, before the window was seen shut");
            return true;
        }
        if (look(f)) {
            r.last = f;
            if (!find_quest_window(f, pictures_, settings_).found) {
                note(r, "closed");
                return false;
            }
        }
        if (waited >= settings_.close_wait_ms) break;
    }
    r.left_open = true;
    note(r, "pressed " + key + " to close, and the window is still open");
    return false;
}

QuestResult QuestRound::run() {
    // Every way out returns `r` itself, so the frame kept in it is never
    // copied on the way.
    QuestResult r;
    started_ = clock();
    const std::string key = InputManager::key_name(settings_.quest_key);
    const auto set = [&](QuestOutcome o, const std::string& why_en) {
        r.outcome = o;
        if (!why_en.empty()) note(r, why_en);
    };
    // Before a key, a click or a move: a stop, HP fallen, or the round's time
    // up. Then nothing more is pressed.
    const auto may_go_on = [&]() {
        if (halted()) {
            set_halted(r, "");
            return false;
        }
        if (clock() - started_ > static_cast<std::uint64_t>(settings_.round_limit_ms)) {
            set(QuestOutcome::TooSlow, format("out of time after %d ms", settings_.round_limit_ms));
            if (r.opened_by_us) r.left_open = true;
            return false;
        }
        return true;
    };
    // A failure with a window that is GoBot's to close: closed again first --
    // unless a halt comes meanwhile, which is then what became of the round.
    const auto fail = [&](QuestOutcome o, const std::string& why_en) {
        if (!why_en.empty()) note(r, why_en);
        if (!close(r)) r.outcome = o;
    };

    if (!pictures_.ready()) {
        set(QuestOutcome::NoPictures, "the pictures are not all there");
        return r;
    }
    if (halted()) {
        set_halted(r, " before it began");
        return r;
    }

    Frame f;
    if (!look(f)) {
        set(QuestOutcome::NoFrame, "could not capture the game");
        return r;
    }
    r.looked = true;
    r.last = f;
    r.window = find_quest_window(f, pictures_, settings_);
    if (r.window.found) {
        if (adopt_) {
            // Left open by an earlier round that could not see it shut: this
            // round's to deal with and close, not the player's to keep.
            r.opened_by_us = true;
            note(r, "window still open from an earlier round, at " + window_words(r.window));
        } else {
            note(r, "window already open at " + window_words(r.window));
        }
        if (r.window.english) {
            fail(QuestOutcome::English, "it is the English client's window: nothing done");
            return r;
        }
    } else {
        // Opened with the key, once, and looked for.
        if (!may_go_on()) return r;
        if (!press(r, settings_.open_hold_ms)) {
            r.why = "phím " + key + " không vào được game";
            set(QuestOutcome::Refused, "could not press " + key + ": the game is not in front");
            return r;
        }
        r.opened_by_us = true;
        note(r, "pressed " + key + " to open the window");
        if (!wait(settings_.open_wait_ms)) {
            set_halted(r, "");
            return r;
        }
        for (int waited = 0;; waited += 250) {
            if (look(f)) {
                r.last = f;
                r.window = find_quest_window(f, pictures_, settings_);
                if (r.window.found) break;
            }
            if (waited >= settings_.open_look_ms) break;
            if (!wait(250)) {
                set_halted(r, "");
                return r;
            }
        }
        if (!r.window.found) {
            // Nothing more is pressed: whatever the key did, it was not this.
            // Something may have come up all the same -- another window, or
            // this one too slow or drawn otherwise -- and nothing has shut it.
            r.maybe_open = true;
            set(QuestOutcome::NoWindow,
                format("no quest window after pressing %s (best title %.2f); whatever the key "
                       "opened, if anything, is left as it is",
                       key.c_str(), static_cast<double>(r.window.best_title)));
            return r;
        }
        note(r, "window at " + window_words(r.window));
        if (r.window.english) {
            fail(QuestOutcome::English, "it is the English client's window: nothing done");
            return r;
        }
    }

    // NV Phụ, at GoC's point inside it.
    ScreenRect win = r.window.at;
    {
        const int x = win.x + settings_.tab_click_x, y = win.y + settings_.tab_click_y;
        if (!may_go_on()) return r;
        if (!click(r, win, x, y, "the NV Phu tab")) {
            fail(QuestOutcome::Refused, "");
            return r;
        }
        note(r, format("NV Phu tab clicked %d,%d", x, y));
    }
    if (!wait(settings_.tab_wait_ms)) {
        set_halted(r, "");
        return r;
    }
    if (!look(f)) {
        fail(QuestOutcome::NoFrame, "could not capture the game");
        return r;
    }
    r.last = f;
    {
        const QuestWindow now = find_quest_window(f, pictures_, settings_);
        if (!now.found) {
            r.why = "cửa sổ Nhiệm Vụ đóng mất giữa chừng";
            set(QuestOutcome::NoWindow, "the window went after the tab click");
            return r;
        }
        win = now.at;
    }

    // Tự Động, and its box.
    r.before = find_auto_option(f, win, pictures_, settings_);
    if (!r.before.found) {
        fail(QuestOutcome::NoAuto,
             format("no Tu Dong in NV Phu (best %.2f)", static_cast<double>(r.before.score)));
        return r;
    }
    note(r, format("Tu Dong at %s (%.2f): ", where(r.before.at).c_str(),
                   static_cast<double>(r.before.score)) +
                box_words(r.before));
    if (r.before.state == AutoState::Unknown) {
        fail(QuestOutcome::UnknownState, "its box is neither ticked nor clear: not clicked");
        return r;
    }
    if (r.before.state == AutoState::On) {
        fail(QuestOutcome::AlreadyOn, "already on: not clicked");
        return r;
    }

    // Off: one click, at GoC's point, and never a second.
    const int cx = r.before.click_x, cy = r.before.click_y;
    if (!may_go_on()) return r;
    if (!click(r, win, cx, cy, "Tu Dong")) {
        fail(QuestOutcome::Refused, "");
        return r;
    }
    note(r, format("clicked %d,%d", cx, cy));
    if (!wait(settings_.click_wait_ms)) {
        set_halted(r, "");
        return r;
    }
    // The pointer off the box, as GoC moves it, before the box is read again.
    if (!may_go_on()) return r;
    if (hands_.move) hands_.move(cx + settings_.hover_dx, cy);
    if (!wait(settings_.hover_wait_ms)) {
        set_halted(r, "");
        return r;
    }
    if (!look(f)) {
        fail(QuestOutcome::ClickFailed, "could not capture the game after the click");
        return r;
    }
    r.last = f;

    // Read again where it was -- found again first, in case it moved a pixel.
    const AutoOption again = find_auto_option(f, win, pictures_, settings_);
    if (again.found && std::abs(again.at.x - r.before.at.x) <= 4 &&
        std::abs(again.at.y - r.before.at.y) <= 4) {
        r.after = again;
    } else {
        r.after = r.before;
        r.after.state = read_quest_box(f, r.before.at.x + settings_.auto_box.x,
                                       r.before.at.y + settings_.auto_box.y, pictures_, settings_,
                                       r.after.on, r.after.off);
    }
    if (r.after.state != AutoState::On) {
        fail(QuestOutcome::ClickFailed, "-> not on: " + box_words(r.after));
        return r;
    }
    note(r, format("-> on (%.2f)", static_cast<double>(r.after.on)));
    if (!close(r)) r.outcome = QuestOutcome::TurnedOn;
    return r;
}

QuestProbe probe_quest(const Frame& f, const QuestPictures& p, const QuestSettings& s) {
    QuestProbe out;
    const std::string key = InputManager::key_name(s.quest_key);
    out.window = find_quest_window(f, p, s);
    const QuestWindow& w = out.window;
    if (!w.found) {
        out.vi = "Không thấy cửa sổ Nhiệm Vụ trên màn hình — mở nó bằng tay (phím " + key +
                 ") ở thẻ NV Phụ rồi thử lại.";
        if (w.score > 0.0f)
            out.vi += " Giống nhất: thanh tiêu đề " + percent(w.score) + ", hàng thẻ " +
                      percent(std::max(w.tabs, w.tabs_en)) + ".";
        else if (w.best_title > 0.0f)
            out.vi += " Giống nhất: thanh tiêu đề " + percent(w.best_title) + ".";
        out.log = format("no quest window (best title %.3f; at the best place over %.2f the tabs "
                         "%.3f, english tabs %.3f)",
                         static_cast<double>(w.best_title), static_cast<double>(s.window_score),
                         static_cast<double>(w.tabs), static_cast<double>(w.tabs_en));
        return out;
    }
    out.log = "window at " + window_words(w) +
              format(", vi tabs %.3f, en tabs %.3f", static_cast<double>(w.tabs),
                     static_cast<double>(w.tabs_en));
    if (w.english) {
        out.vi = "Thấy cửa sổ Nhiệm Vụ của bản game tiếng Anh ở " + where(w.at) + ": " + kEnglishVi +
                 ".";
        return out;
    }
    out.vi = "Thấy cửa sổ Nhiệm Vụ ở " + where(w.at) + " (thanh tiêu đề giống " + percent(w.score) +
             ", hàng thẻ " + percent(w.tabs) + ", bản tiếng Việt).";
    // Where a round would click, put to the same test a round puts it to,
    // against this frame as the game's client area: a probe that finds
    // everything says nothing if a round would then refuse to click.
    std::string why, why_vi;
    const int tx = w.at.x + s.tab_click_x, ty = w.at.y + s.tab_click_y;
    out.tab_ok = quest_click_ok(w.at, tx, ty, f.width, f.height, s, &why, &why_vi);
    if (out.tab_ok) {
        out.log += format("; tab click %d,%d ok", tx, ty);
    } else {
        out.vi += " GoBot sẽ không bấm vào thẻ NV Phụ ở " + format("%d,%d", tx, ty) + ": " + why_vi + ".";
        out.log += format("; tab click %d,%d refused: %s", tx, ty, why.c_str());
    }
    out.option = find_auto_option(f, w.at, p, s);
    const AutoOption& o = out.option;
    if (!o.found) {
        out.vi += " Không thấy ô Tự Động" +
                  (o.score > 0.0f ? " (giống nhất " + percent(o.score) + ")" : std::string()) +
                  " — chuyển sang thẻ NV Phụ rồi thử lại.";
        out.log += format("; no Tu Dong (best %.3f)", static_cast<double>(o.score));
        return out;
    }
    const char* state = o.state == AutoState::On    ? "đang bật"
                        : o.state == AutoState::Off ? "đang tắt"
                                                    : "không rõ bật hay tắt";
    out.vi += " Thấy ô Tự Động ở " + where(o.at) + " (giống " + percent(o.score) + "): " + state +
              " (giống ô bật " + percent(o.on) + ", ô tắt " + percent(o.off) + ").";
    out.log += format("; Tu Dong at %s (%.3f): ", where(o.at).c_str(),
                      static_cast<double>(o.score)) +
               box_words(o);
    out.click_ok = quest_click_ok(w.at, o.click_x, o.click_y, f.width, f.height, s, &why, &why_vi);
    if (out.click_ok) {
        out.log += format(", click %d,%d ok", o.click_x, o.click_y);
    } else {
        out.vi += " GoBot sẽ không bấm vào ô Tự Động ở " + format("%d,%d", o.click_x, o.click_y) +
                  ": " + why_vi + " — làm nhiệm vụ nhanh sẽ không bật được ô này.";
        out.log += format(", click %d,%d refused: %s", o.click_x, o.click_y, why.c_str());
    }
    return out;
}

std::string keep_quest_frame(const std::string& dir, const Frame& f, const std::string& what,
                             bool probe) {
    if (dir.empty() || !f.valid()) return {};
    CreateDirectoryA(dir.c_str(), nullptr);
    // The newest twenty of each kind: the latest are the ones that matter, and
    // a probe's are a record of their own.
    prune_pngs(dir, probe, kKeptFrames - 1);
    static std::atomic<int> n{0};
    std::string tag;
    for (const char c : what)
        tag += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    // Two digits, so that several in one second still sort in the order taken.
    const std::string count = format("%02d", n++ % 100);
    const std::string name = probe ? "probe_" + stamp() + "_" + count
                                   : stamp() + "_" + count + (tag.empty() ? "" : "_" + tag);
    const std::string path = dir + "\\" + name + ".png";
    return save_png(f, path) ? path : std::string();
}

// ------------------------------------------------------------------- timing

QuestSchedule::QuestSchedule(std::uint32_t seed) : rng_(seed) {}

void QuestSchedule::start(std::uint64_t now, const QuestSettings& s) {
    interval_min_ = QuestSettings::clamp_interval(s.interval_min);
    back_off_ = s.back_off;
    failures_ = 0;
    on_streak_ = 0;
    last_end_ = 0;
    due_ = now + static_cast<std::uint64_t>(std::max(0, s.first_delay_s)) * 1000;
}

void QuestSchedule::update(const QuestSettings& s) {
    const std::uint64_t before = interval_ms();
    interval_min_ = QuestSettings::clamp_interval(s.interval_min);
    back_off_ = s.back_off;
    // A shorter interval applies to the round already waiting.
    const std::uint64_t after = interval_ms();
    if (after < before && due_ != 0 && last_end_ != 0) due_ = std::min(due_, last_end_ + after);
}

void QuestSchedule::postpone(std::uint64_t now, std::uint64_t ms) { due_ = now + ms; }

void QuestSchedule::soon(std::uint64_t now, std::uint64_t ms) {
    if (due_ == 0 || now + ms < due_) due_ = now + ms;
}

void QuestSchedule::done(std::uint64_t now, const QuestResult& r) {
    last_end_ = now;
    // Only a box found ticked, again and again, is a reason to look less
    // often; anything else -- found off, or trouble -- is a reason to look as
    // often as asked.
    if (r.outcome == QuestOutcome::AlreadyOn && !r.left_open) ++on_streak_;
    else on_streak_ = 0;
    if (r.failed()) ++failures_;
    else if (r.done()) failures_ = 0;
    due_ = now + draw_gap_ms();
}

std::uint64_t QuestSchedule::interval_ms() const {
    const std::uint64_t base = static_cast<std::uint64_t>(interval_min_) * 60'000;
    if (!back_off_ || on_streak_ < 3) return base;
    std::uint64_t v = base;
    for (int i = 2; i < on_streak_ && v < kLongest; ++i) v *= 2;
    return std::max(base, std::min(kLongest, v));
}

std::uint64_t QuestSchedule::draw_gap_ms() {
    const std::uint64_t iv = interval_ms();
    const std::uint64_t spread = iv / 10;
    std::uniform_int_distribution<std::uint64_t> pick(iv - spread, iv + spread);
    return pick(rng_);
}

QuestGate quest_gate(const QuestGateInput& in) {
    QuestGate g;
    const auto no = [&](std::string why) {
        g.why = std::move(why);
        return g;
    };
    if (!in.window) return no("chưa thấy cửa sổ game");
    if (in.keeper) return no("đang tự vào lại game");
    if (in.hp_pct < 0.0f) return no("không thấy thanh HP/MP — nhân vật chưa ở trong game");
    if (in.hp_pct < static_cast<float>(in.hp_floor)) return no("máu thấp");
    if (!in.held_by.empty()) return no("bot đang tạm dừng (" + in.held_by + ")");
    if (in.trash) return no("đang dọn rác, hoặc hộp thoại thùng rác có thể đang mở");
    if (in.chat) return no("ô chat trong game có thể đang mở");
    if (in.resting) return no("vừa làm xong một lần chưa đầy 1 phút");
    if (in.user_active) return no("bạn đang dùng chuột hoặc bàn phím");
    // Only a round a person asked for brings the game forward; a timed one
    // waits for it, rather than take the keys of whoever is using another
    // window.
    if (!in.focused && !in.asked) return no("game không ở trên cùng");
    g.go = true;
    return g;
}

// ------------------------------------------------------------------ the run

QuestRun::QuestRun(std::uint32_t seed) : schedule_(seed) {}

void QuestRun::start(std::uint64_t now, const QuestSettings& s) {
    settings_ = s;
    schedule_.start(now, s);
    running_ = true;
    stopped_ = false;
    stopped_why_.clear();
    stopped_log_.clear();
    said_.clear();
    said_at_ = 0;
    if (s.skin == QuestSettings::Skin::En)
        stop_itself(std::string(kEnglishVi) + ".", "quest.ini says the English client, which is not done");
}

void QuestRun::update(const QuestSettings& s) {
    settings_ = s;
    schedule_.update(s);
}

void QuestRun::ask_once(std::uint64_t now) {
    asked_ = true;
    asked_until_ = now + kAskMs;
    retry_at_ = 0;
    waiting_for_.clear();
}

void QuestRun::soon(std::uint64_t now) {
    if (running_ && !stopped_) schedule_.soon(now, kSoonMs);
}

std::uint64_t QuestRun::next_at() const {
    return running_ && !stopped_ && settings_.enabled ? schedule_.due_at() : 0;
}

void QuestRun::say_now_and_then(std::uint64_t now, const std::string& why) {
    // Each reason once a minute at most, not every look.
    if (why == said_ && said_at_ != 0 && now - said_at_ < 60'000) return;
    said_ = why;
    said_at_ = now;
    if (hooks_.say) hooks_.say(why);
}

void QuestRun::wait(std::uint64_t now, const std::string& why) {
    waiting_for_ = why;
    if (asked_) retry_at_ = now + 1000;
    if (running_ && settings_.enabled && schedule_.due(now)) schedule_.postpone(now, kPostponeMs);
    say_now_and_then(now, "Chờ: " + why + ".");
}

void QuestRun::stop_itself(const std::string& why, const std::string& why_en) {
    stopped_ = true;
    stopped_why_ = why;
    stopped_log_ = why_en;
    asked_ = false;
    if (hooks_.say) hooks_.say(why);
}

void QuestRun::step(std::uint64_t now) {
    if (stopped_) {
        asked_ = false;
        return;
    }
    if (asked_ && now >= asked_until_) {
        asked_ = false;
        if (hooks_.say)
            hooks_.say("Không làm được lần này" +
                       (waiting_for_.empty() ? std::string() : ": " + waiting_for_) + ".");
    }
    const bool timed = running_ && settings_.enabled && schedule_.due(now);
    const bool ask = asked_ && now >= retry_at_;
    if (!timed && !ask) return;

    QuestGateInput in = hooks_.gates ? hooks_.gates() : QuestGateInput{};
    in.resting = schedule_.last_end() != 0 && now < schedule_.last_end() + kRestMs;
    in.asked = ask;
    const QuestGate gate = quest_gate(in);
    if (!gate.go) {
        wait(now, gate.why);
        return;
    }

    QuestCall call;
    call.asked = ask;
    call.ours_open = ours_open_;
    QuestResult r = hooks_.round ? hooks_.round(call) : QuestResult{};
    if (hooks_.now) now = std::max(now, hooks_.now());
    // Whether a window GoBot opened may still be open, for the next round to
    // close: news only from a round that looked at the game.
    if (r.looked) ours_open_ = r.opened_by_us && (r.left_open || r.maybe_open);
    if (r.outcome == QuestOutcome::NotNow || r.outcome == QuestOutcome::None) {
        wait(now, r.why.empty() ? std::string("bot đang bận") : r.why);
        return;
    }
    asked_ = false;
    waiting_for_.clear();
    said_.clear();
    last_ = r;
    has_last_ = true;
    if (r.outcome == QuestOutcome::Cancelled) {
        // Whoever stopped it says so, and is not counted against it. Not at
        // once again either: what stopped it -- the keeper crossing, a hold
        // taken over -- is still there for a while.
        if (running_ && settings_.enabled && schedule_.due(now)) schedule_.postpone(now, kPostponeMs);
        return;
    }
    const std::string key = InputManager::key_name(settings_.quest_key);
    if (r.outcome == QuestOutcome::NoFrame) {
        if (running_ && settings_.enabled) schedule_.postpone(now, kPostponeMs);
        if (hooks_.say) hooks_.say("Chưa chụp được màn hình game — thử lại sau.");
        return;
    }
    schedule_.done(now, r);
    if (hooks_.say)
        hooks_.say((r.done() ? "Xong: " : "Không được: ") + quest_result_vi(r, key) + ".");
    if (r.outcome == QuestOutcome::English) {
        stop_itself(std::string(kEnglishVi) + ".", "the English client's window, which is not done");
        return;
    }
    if (r.outcome == QuestOutcome::NoPictures) {
        stop_itself("Thiếu hình nhận diện cửa sổ Nhiệm Vụ — cài lại GoBot.",
                    "its pictures are not all there");
        return;
    }
    if (running_ && r.failed() && schedule_.failures() >= settings_.max_failures) {
        const int n = schedule_.failures();
        const std::string why = quest_trouble_why(r);
        stop_itself("Đã tự tắt: " + std::to_string(n) + " lần liền " + why +
                        ". Xem ảnh trong thư mục quest_debug.",
                    std::to_string(n) + " failed rounds in a row, the last " +
                        quest_outcome_name(r.outcome) +
                        (r.left_open ? " with the window left open" : ""));
        if (hooks_.alert) hooks_.alert("quest_trouble", alert_text::quest_trouble(n, why));
    }
}

} // namespace gobot
