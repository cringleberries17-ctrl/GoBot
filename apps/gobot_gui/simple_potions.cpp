// Potion stock -- "Còn N bình" -- as the GUI does it: a look at the potion
// keys' hotbar slots every few seconds of a run, on the frames the HP reader
// has taken already, and the line under each potion key -- each tier of each
// potion row -- on the Đánh quái tab.
//
// What a slot says, and when that is worth a message, is the library's
// (gobot/potion_stock.hpp). Here is what only the window knows: which keys are
// potion keys, whether a run is on, the HP reader, the alerts. Look only:
// nothing here presses, clicks or holds anything, nor takes the mouse.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/alerts.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/logger.hpp"
#include "gobot/potion_stock.hpp"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gobot {
namespace {

using namespace simple_ui;

/// The newest frames kept in potion_debug: what a slot did when its count went
/// up -- the next stack -- or it went empty, the evidence for whether the game
/// moves the next stack in by itself; and the frame of a count read wild and
/// then back again (PotionWatch::Event::Misread), the evidence of a misread.
constexpr std::size_t kKeptFrames = 20;

/// A key to read, and what it was believed to hold before, for telling what
/// is worth keeping a frame of.
struct KeyAsk {
    std::uint32_t vk = 0;
    bool          had = false;
    SlotReading   before;
};

/// What one look came back with.
struct PotionLook {
    HotbarSpot spot;
    /// Each key's two readings, in the order taken.
    std::vector<std::pair<std::uint32_t, std::vector<SlotReading>>> readings;
    std::string kept;               ///< a frame kept in potion_debug, by name
    /// The look's last frame: held in memory while a key's count read wild,
    /// and written to potion_debug if that turns out a misread.
    Frame frame;
};

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

/// The oldest PNGs in `dir` deleted until `keep` are left. Named by when they
/// were taken, they sort oldest first.
void prune(const std::string& dir, std::size_t keep) {
    std::vector<std::string> names = files_in(dir, "*.png");
    std::sort(names.begin(), names.end());
    for (std::size_t i = 0; i + keep < names.size(); ++i) remove_file(dir + "\\" + names[i]);
}

/// "slot 7 row 1", counted from 1 as a player would.
std::string where(const SlotReading& r) {
    return "slot " + std::to_string(r.col + 1) + " row " + std::to_string(r.row + 1);
}

std::string said(const SlotReading& r) {
    return r.has_count() ? std::to_string(r.count) : std::string(slot_state_name(r.state));
}

/// "key 1 at slot 7 row 1", or "key Q" for one not on the hotbar.
std::string key_at(std::uint32_t vk, const std::string& key) {
    SlotReading r;
    if (!hotbar_slot_of(vk, r.row, r.col)) return "key " + key;
    return "key " + key + " at " + where(r);
}

/// Round a slot whose picture is kept, this much of the bar's frame: the
/// slot's own edge and the label's corner, nothing of the screen beyond.
constexpr int kSlotCutMargin = 3;

/// One hotbar slot cut out of a frame, kSlotCutMargin round it (2026.11.04d):
/// the slot's icon and its count, never a name, a chat line or a bar. Empty
/// when it is not wholly in the frame.
Frame slot_cut(const Frame& f, const ScreenRect& slot) {
    Frame out;
    const ScreenRect r{ slot.x - kSlotCutMargin, slot.y - kSlotCutMargin, slot.w + 2 * kSlotCutMargin,
                        slot.h + 2 * kSlotCutMargin };
    if (!f.valid() || r.x < 0 || r.y < 0 || r.right() > f.width || r.bottom() > f.height || !r.valid()) return out;
    out.width = r.w;
    out.height = r.h;
    out.pixels.resize(static_cast<std::size_t>(r.w) * r.h * 4);
    for (int y = 0; y < r.h; ++y)
        std::copy_n(f.pixels.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(r.y + y) * f.width + r.x) * 4),
                    static_cast<std::size_t>(r.w) * 4,
                    out.pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * r.w * 4));
    return out;
}

/// A key's name as it can go in a file's name: its letters and digits.
std::string file_part(const std::string& key) {
    std::string out;
    for (const char c : key)
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) out += c;
    return out.empty() ? std::string("key") : out;
}

} // namespace

/// Everything the GUI keeps for potion stock.
struct GuiApp::PotionsUi {
    PotionSettings settings;
    bool           loaded = false;
    int            warn_box = 20;           ///< the tab's box, before it is written
    Frame          picture;                 ///< grabbed/hotbar_frame
    bool           picture_tried = false;
    CountReader    digits;
    bool           digits_tried = false;
    PotionWatch    watch;
    HotbarSpot     spot;                    ///< where the hotbar was last found
    std::uint64_t  next_ms = 0;
    std::uint64_t  reads_seen = 0;          ///< the HP reader's reads at the last look
    bool           was_running = false;
    std::map<std::uint32_t, SlotReading> logged;    ///< what the log last said, per key
    std::string    trouble;                 ///< why nothing can be read, in Vietnamese
    /// Per key, the frame of the newest look that read its count wild: held
    /// until the count comes back (a misread: written to potion_debug) or the
    /// drop is believed (dropped).
    std::map<std::uint32_t, Frame> wild_frames;
    /// Per key, whether "Low" by the bag has been said: once, until the bag
    /// holds kBagLowRearm more than warn_bag_below again, or a new run.
    std::map<std::uint32_t, bool> bag_low_said;
    /// Misread frames being written to potion_debug, off this thread: each
    /// comes back with its line for the log.
    std::vector<std::future<std::string>> saving;
    /// The look under way. Last, so that it is the first thing to go: a look
    /// still running is waited for while the pictures it reads are still here.
    std::future<PotionLook> look;
};

void GuiApp::PotionsUiFree::operator()(PotionsUi* ui) const { delete ui; }

void GuiApp::potions_tick() {
    if (!potions_ui_) potions_ui_.reset(new PotionsUi);
    PotionsUi& ui = *potions_ui_;
    if (!ui.loaded) {
        ui.loaded = true;
        IniFile ini;
        if (ini.load(exe_relative("configs/app.ini"))) ui.settings.load(ini);
        ui.warn_box = ui.settings.warn_below;
    }
    ui.watch.set_warn_below(ui.settings.warn_below);

    // The packets' bag kept up with the macro's presses: which bag slot each
    // potion key drinks from, checked against the hotbar's counts read below
    // (packets_bag.cpp). It reads PacketSelf's copies only.
    bag_check_tick(now_ms());

    // A new run: every key may warn again.
    const bool running = simple_running();
    if (running && !ui.was_running) {
        ui.watch.reset();
        ui.logged.clear();
        ui.wild_frames.clear();
        ui.bag_low_said.clear();
        ui.next_ms = now_ms() + 3000;
        std::lock_guard<std::mutex> lock(potion_slots_mutex_);
        potion_slots_.clear();
    }
    ui.was_running = running;

    // Misread frames written meanwhile: their lines for the log.
    for (auto it = ui.saving.begin(); it != ui.saving.end();) {
        if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        GB_INFO("%s", it->get().c_str());
        it = ui.saving.erase(it);
    }

    // A look that has come back.
    if (ui.look.valid() && ui.look.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        PotionLook got = ui.look.get();
        if (got.spot.found) ui.spot = got.spot;
        const std::uint64_t looked = now_ms();
        for (const auto& [vk, readings] : got.readings) {
            bool hp = true;
            for (const MacroKey& k : macro_config_.keys)
                if (k.keycode == vk && macro_key_kind(k) == MacroKeyKind::MpPotion) hp = false;
            const std::string key = InputManager::key_name(vk);
            // One reading per look, from its two frames: a look that got one
            // frame only says nothing.
            if (readings.size() < 2) continue;
            const SlotReading r = PotionWatch::look_of(readings[0], readings[1]);
            // A key the packets' bag has been checked for against this very
            // slot: "Low" goes by the bag's total of what it drinks, and its
            // slot's own Low is not said. "Out" is always the slot's.
            int bag_total = -1;
            std::uint32_t bag_item = 0;
            const bool by_bag = bag_low_count(vk, bag_total, bag_item);
            for (const PotionWatch::Event& ev : ui.watch.feed(vk, r)) {
                using Kind = PotionWatch::Event::Kind;
                switch (ev.kind) {
                    case Kind::Low: {
                        if (by_bag) {
                            GB_INFO("Potions: key %s's slot reads %d -- not said: the bag holds %d of %u (packets)",
                                    key.c_str(), ev.count, bag_total, static_cast<unsigned>(bag_item));
                            break;
                        }
                        // The slot's number is the stack in use; the bag's
                        // total beside it, once the packets have learned what
                        // the key drinks but before they are trusted for it
                        // (2026.11.04d: "còn 5" with 100 more next to it).
                        int unchecked = -1;
                        std::uint32_t unchecked_item = 0;
                        if (!bag_learned_count(vk, unchecked, unchecked_item)) unchecked = -1;
                        if (unchecked >= 0)
                            GB_INFO("Potions: key %s is low -- %d left in its slot, the stack in use; the bag holds %d "
                                    "of %u (packets, not checked yet)",
                                    key.c_str(), ev.count, unchecked, static_cast<unsigned>(unchecked_item));
                        else
                            GB_INFO("Potions: key %s is low -- %d left in its slot, the stack in use", key.c_str(),
                                    ev.count);
                        alert("potion_low", alert_text::potion_low(hp, key, ev.count, unchecked));
                        break;
                    }
                    case Kind::Out:
                        GB_WARN("Potions: key %s's slot is empty, and the key is still pressed", key.c_str());
                        alert("potion_out", alert_text::potion_out(hp, key));
                        break;
                    case Kind::Wild:
                        // Not believed yet: the frame is held, in case the
                        // count comes back -- the newest look's.
                        if (got.frame.valid()) ui.wild_frames[vk] = got.frame;
                        break;
                    case Kind::WildBelieved:
                        ui.wild_frames.erase(vk);
                        GB_INFO("Potions: %s read %d %d looks running (was %d) -- believed, the stack is that low",
                                key_at(vk, key).c_str(), ev.count, PotionWatch::kWildLooks, ev.was);
                        break;
                    case Kind::Misread: {
                        const std::string text = "Potions: " + key_at(vk, key) + " read " + std::to_string(ev.count) +
                                                 " (was " + std::to_string(ev.was) + "), then " +
                                                 std::to_string(ev.back) + " -- a misread, not believed";
                        const auto held = ui.wild_frames.find(vk);
                        if (held == ui.wild_frames.end() || !held->second.valid()) {
                            GB_INFO("%s", text.c_str());
                            break;
                        }
                        // Written off this thread, as the look's own keep is.
                        Frame f = std::move(held->second);
                        ui.wild_frames.erase(held);
                        const std::string name = stamp() + "_" + file_part(key) + "_misread.png";
                        ui.saving.push_back(std::async(std::launch::async, [f = std::move(f), name, text] {
                            const std::string dir = exe_relative("potion_debug");
                            if (!make_folder(dir) || !save_png(f, dir + "\\" + name))
                                return text + "; the frame could not be kept";
                            prune(dir, kKeptFrames);
                            return text + "; kept the frame as potion_debug\\" + name;
                        }));
                        break;
                    }
                    case Kind::Unreadable: {
                        // Two looks running, once a run: a picture of that
                        // slot alone (slot_cut), written off this thread as a
                        // misread's frame is (2026.11.04d: key G of 2026-11-04).
                        const std::string text = "Potions: " + key_at(vk, key) + " unreadable on two looks running";
                        int row = -1, col = -1;
                        Frame f;
                        if (got.spot.found && hotbar_slot_of(vk, row, col))
                            f = slot_cut(got.frame, got.spot.slot(row, col));
                        if (!f.valid()) {
                            GB_INFO("%s -- no picture of the slot to keep", text.c_str());
                            break;
                        }
                        const std::string name = stamp() + "_" + file_part(key) + "_unreadable.png";
                        ui.saving.push_back(std::async(std::launch::async, [f = std::move(f), name, text] {
                            const std::string dir = exe_relative("potion_debug");
                            if (!make_folder(dir) || !save_png(f, dir + "\\" + name))
                                return text + "; its picture could not be kept";
                            prune(dir, kKeptFrames);
                            return text + "; kept a picture of the slot alone as potion_debug\\" + name;
                        }));
                        break;
                    }
                }
            }
            // No wild drop held any more -- come back, believed, or ended by
            // the slot emptied -- no frame of it either. A slot covered for a
            // while holds both: the count that comes back after is a misread
            // all the same, and its frame is kept then.
            if (!ui.watch.holding(vk)) ui.wild_frames.erase(vk);
            // The log: only when what is believed changes.
            SlotReading now;
            const bool have = ui.watch.believed(vk, now);
            {
                // For the black box after a death: what the slot was last
                // believed to hold, and when a look last read it so. A look
                // that read something else -- the hotbar covered, the slot
                // unreadable, a wild drop held -- leaves that time as it was,
                // and is told beside it (R8R-4).
                std::lock_guard<std::mutex> lock(potion_slots_mutex_);
                potion_slots_.note(key, r, have, now, looked);
            }
            // The slot's count for the packets' bag to be checked against:
            // only a look that read what is believed -- a count, or the slot
            // empty.
            if (have && r.same(now)) {
                if (now.has_count())
                    bag_note_hotbar(vk, now.count, looked);
                else if (now.state == SlotState::Empty)
                    bag_note_hotbar(vk, 0, looked);
            }
            // "Low" by the bag: its total under warn_bag_below -- its own mark,
            // the whole bag's, not a slot's warn_below -- said once, and not
            // again until it is kBagLowRearm over that, or a new run.
            if (by_bag && ui.settings.warn_bag_below > 0 &&
                bag_low_now(bag_total, ui.settings.warn_bag_below, ui.bag_low_said[vk])) {
                GB_INFO("Potions: key %s is low -- %d of %u left in the bag (packets), under %d", key.c_str(),
                        bag_total, static_cast<unsigned>(bag_item), ui.settings.warn_bag_below);
                alert("potion_low", alert_text::potion_low_bag(hp, key, bag_total));
            }
            if (!have) continue;
            const auto was = ui.logged.find(vk);
            if (was != ui.logged.end() && was->second.same(now)) continue;
            const std::string before = was == ui.logged.end() ? std::string{} : " (was " + said(was->second) + ")";
            if (now.row >= 0)
                GB_INFO("Potions: key %s at %s reads %s%s", key.c_str(), where(now).c_str(), said(now).c_str(),
                        before.c_str());
            else
                GB_INFO("Potions: key %s reads %s%s", key.c_str(), said(now).c_str(), before.c_str());
            ui.logged[vk] = now;
        }
        if (!got.kept.empty()) GB_INFO("Potions: kept the frame as potion_debug\\%s", got.kept.c_str());
    }
    if (!running || !vitals_running_ || ui.look.valid() || now_ms() < ui.next_ms) return;

    // The potion keys switched on, each once: two potion rows on one key --
    // as GoC's own file has for HP -- are one slot.
    std::vector<KeyAsk> keys;
    for (const MacroKey& k : macro_config_.keys) {
        const MacroKeyKind kind = macro_key_kind(k);
        if (!k.enabled || k.keycode == 0 ||
            (kind != MacroKeyKind::HpPotion && kind != MacroKeyKind::MpPotion))
            continue;
        if (std::any_of(keys.begin(), keys.end(), [&](const KeyAsk& a) { return a.vk == k.keycode; })) continue;
        KeyAsk ask;
        ask.vk = k.keycode;
        ask.had = ui.watch.believed(k.keycode, ask.before);
        keys.push_back(ask);
    }
    if (keys.empty()) return;

    if (!ui.picture_tried) {
        ui.picture_tried = true;
        if (!TemplateStore::load_one(hotbar::kPicture, exe_relative("configs/templates.ini"), ui.picture)) {
            ui.trouble = "Thiếu hình thanh phím (grabbed/hotbar_frame.png), nên không đọc được số bình.";
            GB_WARN("Potions: no picture of the hotbar (grabbed/hotbar_frame.png) -- nothing is read");
        }
    }
    if (!ui.picture.valid()) return;
    if (!ui.digits_tried) {
        ui.digits_tried = true;
        if (!ui.digits.load(GlyphFont::default_dir()))
            GB_WARN("Potions: no digits of the game's font, so only empty slots can be told");
    }

    // A frame the reader has taken since the last look, or none: a frozen
    // reader's last frame says nothing new, and read again it would agree
    // with itself.
    const std::uint64_t reads = vitals_.stats().reads;
    if (reads == ui.reads_seen) {
        ui.next_ms = now_ms() + 1000;
        return;
    }
    ui.reads_seen = reads;
    ui.next_ms = now_ms() + static_cast<std::uint64_t>(ui.settings.read_every_s) * 1000;

    // Off the drawing thread: a search along the bottom of the screen takes a
    // couple of dozen milliseconds when the hotbar is not where it was.
    const HotbarSpot was = ui.spot;
    const Frame* picture = &ui.picture;
    const CountReader* digits = &ui.digits;
    // The second look of a pair, after this long: soon enough that the stack
    // has not moved in between, late enough that the reader has a new frame
    // at the pace it reads at now -- 400 ms at 150 and at 250 alike.
    const int pair_gap_ms = potion_pair_gap_ms(vitals_.period_ms());
    ui.look = std::async(std::launch::async, [this, keys, was, picture, digits, pair_gap_ms] {
        PotionLook out;
        HotbarSpot spot = was;
        Frame last;
        for (int look = 0; look < 2; ++look) {
            if (look == 1) std::this_thread::sleep_for(std::chrono::milliseconds(pair_gap_ms));
            Frame f;
            if (!vitals_.latest_frame(f)) break;
            const HotbarSpot found = find_hotbar(f, *picture, spot.found ? &spot : nullptr);
            if (found.found) spot = found;
            out.spot = found;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (out.readings.size() <= i) out.readings.push_back({ keys[i].vk, {} });
                out.readings[i].second.push_back(read_slot(f, found, keys[i].vk, *digits, *picture));
            }
            last = std::move(f);
        }

        // A slot gone empty, or a count gone up -- the next stack moved in,
        // or one bought: the frame is kept, the newest few dozen of them.
        bool keep = false;
        for (std::size_t i = 0; i < keys.size() && i < out.readings.size(); ++i) {
            const auto& rs = out.readings[i].second;
            if (rs.size() < 2 || !rs[0].same(rs[1]) || !keys[i].had) continue;
            const SlotReading& now = rs[1];
            const SlotReading& before = keys[i].before;
            keep = keep || (now.state == SlotState::Empty && before.state != SlotState::Empty) ||
                   (now.has_count() && before.has_count() && now.count > before.count) ||
                   (now.has_count() && before.state == SlotState::Empty);
        }
        if (keep && last.valid()) {
            const std::string dir = exe_relative("potion_debug");
            const std::string name = stamp() + "_hotbar.png";
            if (make_folder(dir) && save_png(last, dir + "\\" + name)) {
                out.kept = name;
                prune(dir, kKeptFrames);
            }
        }
        out.frame = std::move(last);
        return out;
    });
}

std::string GuiApp::potion_slots_line(std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(potion_slots_mutex_);
    return potion_slots_.line(now);
}

void GuiApp::simple_potion_stock(const MacroKey& key) {
    if (!potions_ui_) return;           // made by potions_tick, which runs first
    PotionsUi& ui = *potions_ui_;

    // Only a key that is switched on, as potions_tick reads only those. A
    // tier switched off has no line: its slot is not read, and "not read
    // yet" would look like a fault.
    if (!key.enabled || key.keycode == 0) return;

    const std::string name = InputManager::key_name(key.keycode);
    SlotReading r;
    int row = -1, col = -1;
    std::string line;
    ImVec4 colour = kDim;
    if (!hotbar_slot_of(key.keycode, row, col)) {
        r.state = SlotState::NotOnBar;
        line = potion_line_vi(r, name);
    } else if (!ui.trouble.empty()) {
        line = ui.trouble;
        colour = kWait;
    } else if (!simple_running()) {
        // Read only during a run: the last run's count, left up after
        // DỪNG, would look like today's.
        line = "Số bình được đọc khi bot chạy.";
    } else if (ui.watch.believed(key.keycode, r)) {
        line = potion_line_vi(r, name);
        if (r.state == SlotState::Empty) colour = kBad;
        else if (r.has_count() && ui.settings.warn_below > 0 && r.count < ui.settings.warn_below)
            colour = kWait;
        else if (r.has_count())
            colour = kGood;
    } else {
        line = "Chưa đọc được số bình";
    }
    ImGui::PushFont(nullptr, kSmall);
    coloured(colour, line.c_str());
    ImGui::PopFont();
}

void GuiApp::simple_potion_warn_box() {
    if (!potions_ui_) return;           // made by potions_tick, which runs first
    PotionsUi& ui = *potions_ui_;

    label("Báo khi còn dưới");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##potion_warn", &ui.warn_box, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ui.warn_box = std::clamp(ui.warn_box, 0, PotionSettings::kMaxWarn);
        ui.settings.warn_below = ui.warn_box;
        if (!IniFile::set_in_place(exe_relative("configs/app.ini"), "Potions", "warn_below", ui.warn_box))
            simple_note("Không lưu được configs/app.ini.", 8000);
    }
    ImGui::SameLine();
    label("bình (0 = không báo)");
    // A key the packets' bag has been checked for warns by the whole bag, at
    // its own mark (warn_bag_below), not this box's: said here as it is set.
    const int bag = ui.settings.warn_bag_below;
    const std::string text =
        std::string("Số trên ô phím là chồng bình đang dùng; trong túi có thể còn chồng khác. Ô phím trống thì "
                    "vẫn báo \"Hết bình\". ") +
        (bag > 0 ? "Phím mà gói tin túi đồ đã so khớp thì báo theo cả túi, khi còn dưới " + std::to_string(bag) +
                       " bình (warn_bag_below trong configs/app.ini), không theo ô này."
                 : std::string("Phím mà gói tin túi đồ đã so khớp thì không báo sắp hết (warn_bag_below=0 trong "
                               "configs/app.ini)."));
    hint(trial_hint(kTrialPotions, text.c_str()).c_str());
}

} // namespace gobot
