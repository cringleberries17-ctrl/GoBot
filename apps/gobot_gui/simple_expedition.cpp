// Đào khoáng and phó bản Cổ Thụ / Cổ Long (2026.11.19), as the GUI runs them:
// the worker that hands the planners (gobot/expedition.hpp) what the packets
// and the screen say, and does the job each one asks for -- a walk by the
// hunter's trip, a key, a click on a menu's row or a box's Xác Nhận -- and the
// "Đào khoáng" and "Phó bản" tabs.
//
// Everything is GoC 0.6.5.0's own way of doing it, read from its code with no
// live capture (the owner: "Không, làm theo GoC"). Each job says in the log
// what it did, and each one that did not work keeps the screen it saw in
// expedition_debug\, for the first live run to be read by.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/bag.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/expedition.hpp"
#include "gobot/load_meter.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_book.hpp"
#include "gobot/menu_text.hpp"
#include "gobot/party_hud.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace gobot {
namespace {

using namespace simple_ui;
using namespace expedition;

/// The hold a job's keys and clicks are made under, by name.
const char* const kTripHold = "expedition";

/// How long a walk may take before it is given up (GoC: 60 s asked of its
/// server, 65 s before it cancels).
constexpr std::uint64_t kWalkMs = 65000;
/// A walk's clock counts only the time the hunter could walk (2026.11.21, the
/// audit's B5: a trash pass with its shop ride, charms, a chat line, the game
/// behind another window -- held, the hunter stands, and three 65 s walks run
/// out under one long hold made Đào khoáng stop "thất bại 3 lần" for the
/// night); a walk is given up all the same after this, held or not.
constexpr std::uint64_t kWalkMostMs = 10 * 60 * 1000;
/// A map change the expedition makes or waits for is its own, not the last
/// drop's or jump's, for this long (Hunter::expect_map_change, 2026.11.21,
/// B3): a press is watched kPressWatchMs, and a job's own wait comes on top.
constexpr std::uint64_t kExpectMapMs = Hunter::kPressWatchMs + 5000;
/// The boss's area either side of its spot, and above and below.
constexpr int kFightHalfW = 500, kFightHalfH = 200;

/// The Xác Nhận pictures, GoC's (inven/1/ok matches this client at 1.000).
const char* const kOkPictures[] = { "inven/1/ok", "inven/1/ok1", "inven/1/ok2", "inven/0/ok" };
constexpr float kOkScore = 0.85f;
/// The quest window's complete button, as GoC's pictures have it.
/// First the NPC talk box's own orange Hoàn Thành (cut from the 2026.11.19
/// Cổ Long run, CậnVệJatsan's box; GoC's quest pictures never matched it).
const char* const kCompletePictures[] = { "grabbed/npc_complete",
                                          "quest/1/btn_LQuest_Complete.spr/0",
                                          "quest/1/Btn_NewQuestDlg_CompleteQuest.spr/0",
                                          "quest/1/btn_LQuest_Complete.spr/1" };
constexpr float kCompleteScore = 0.80f;

/// An NPC's shop (2026.11.24, do_buy): looks kShopLookMs apart; the pointer
/// off the list kShopSettleMs before a look, for the description of the item
/// it was over to go; the item's name looked for kShopNameMs once the shop is
/// seen -- the list comes with its window; purchases tried in one visit of the
/// shop before it is shut and the planner asked again; how long the bag's
/// frames are waited for once the question has gone; the quantity box watched
/// over a blink of its caret, kShopBoxAgree looks in a row alike, as the
/// potion shop reads it (BagCleaner::shop_box); the gap after a typed key.
constexpr int kShopLookMs = 150;
constexpr int kShopSettleMs = 300;
constexpr std::uint64_t kShopNameMs = 5000;
constexpr int kShopTries = 3;
constexpr std::uint64_t kShopBagFrameMs = 5000;
constexpr int kShopBoxWatchMs = 1200, kShopBoxLookMs = 100, kShopBoxAgree = 3;
constexpr int kShopKeyGapMs = 80;
constexpr float kShopButtonsScore = 0.80f;
/// What read_box says of a question no longer there.
constexpr int kBoxGone = -3;

struct OnExit {
    std::function<void()> fn;
    ~OnExit() { if (fn) fn(); }
};

std::string clock_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

/// Everything the GUI keeps for the tab and the worker.
struct GuiApp::ExpeditionUi {
    ExpeditionSettings settings;
    bool               loaded = false;
    // What the worker says, under worker_mutex_.
    std::vector<std::string> log;
    std::string        mining_status;
    std::string        cothu_status;
    std::string        colong_status;
    // No red "stopped" line since 2026.11.24: no planner stops for good (a
    // gap of kTripRetryMs, its line the planner's status).
    int                runs = 0;
    int                colong_runs = 0;
};

void GuiApp::ExpeditionUiFree::operator()(ExpeditionUi* ui) const { delete ui; }

void GuiApp::say_expedition(const std::string& line) {
    GB_INFO("Expedition: %s", line.c_str());
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        std::vector<std::string>& log = expedition_ui_->log;
        log.push_back(clock_text() + "  " + line);
        if (log.size() > 200) log.erase(log.begin());
    }
    request_redraw();
}

// ------------------------------------------------------------------ worker

void GuiApp::start_expedition() {
    if (!expedition_ui_) expedition_ui_.reset(new ExpeditionUi);
    if (expedition_running_ && !expedition_stop_) return;
    join_pumping(expedition_thread_);
    expedition_stop_ = false;
    expedition_running_ = true;
    expedition_thread_ = std::thread([this] { expedition_loop(); });
}

void GuiApp::stop_expedition(bool wait) {
    expedition_stop_ = true;
    trip_quiet_.clear();        // the other features' jobs wait for nothing now (B6)
    if (!wait) return;
    join_pumping(expedition_thread_);
    expedition_running_ = false;
    if (hunter_) hunter_->end_trip();
}

void GuiApp::expedition_loop() {
    name_this_thread("expedition");
    ExpeditionUi& ui = *expedition_ui_;
    const std::string config = exe_relative("configs/expedition.ini");
    ExpeditionSettings settings;
    settings.load(config);
    std::uint64_t read_ms = now_ms();

    std::shared_ptr<const TemplateStore> store = TemplateStore::shared_best();
    PartyHudReader party_reader;
    std::string party_missing;
    const bool party_ok = party_reader.load(*store, &party_missing);
    if (!party_ok) GB_WARN("Expedition: the party HUD's pictures are missing (%s)", party_missing.c_str());
    CleanSettings bag_settings;
    bag_settings.load(exe_relative("configs/trash.ini"));

    ScreenCapture capture;
    capture.set_label("expedition");
    const auto grab = [&](Frame& f) {
        return input_.window_valid() && capture.grab(input_.target_window(), f) && f.valid();
    };
    const std::string debug_dir = exe_relative("expedition_debug");
    const auto keep = [&](const Frame& f, const std::string& what) {
        if (!f.valid()) return;
        make_folder(debug_dir);
        std::string name = clock_text();
        std::replace(name.begin(), name.end(), ':', '-');
        const std::string path = debug_dir + "\\" + name + "_" + what + ".png";
        if (save_png(f, path)) GB_INFO("Expedition: the screen kept as %s", path.c_str());
    };
    const auto halted = [this] { return expedition_stop_.load(); };
    const auto held_ok = [this] { return !expedition_stop_.load() && coord_.critical_reason() == kTripHold; };

    // ---- what the planners see ----
    int party_size = -1;
    std::uint64_t party_ms = 0;
    // Part of the members' panels hidden on the last party look (PartyHud::
    // covered) -- the bag left open over them, most often (2026.11.24 review).
    bool party_covered = false;
    std::uint64_t bag_fix_ms = 0;
    const auto snap = [&](bool want_party) {
        TripSnap s;
        const PacketWorld::Place place = packet_world_.place();
        const std::uint64_t now = now_ms();
        const PacketWorld::View view = packet_world_.view(now);
        s.world = view.me.known && place.since_ms != 0 && place.map != 0;
        if (s.world) s.stem = GameMapBook::stem_of(place.map, place.area);
        s.epoch = place.epoch;
        // The pet's HP food on reaching TổCổLong (2026.11.24): an arrival this
        // look sees before the window's tick does is taken here, as the tick
        // takes it -- whichever is first -- and the lobby's steps wait while
        // its round is owed (DungeonPlanner, PetRestock::kWaitMostMs at most).
        pet_restock_.look(s.stem, s.epoch, pet_armed_.load(), now);
        s.pet_restock = pet_restock_.waits(s.epoch, now);
        s.x = view.me.x;
        s.y = view.me.y;
        s.settled = view.me.known && !view.me.airborne && !view.me.uncertain;
        const PacketSelf::Vitals v = packet_self_.vitals();
        s.hp_known = v.hp_ms != 0 && v.hp_epoch == packet_self_.epoch();
        s.hp = v.hp;
        s.worn = trip_packets_.worn();
        s.equip1 = trip_packets_.equip1();
        s.other = trip_packets_.other();
        s.dig = trip_packets_.dig(packet_world_.own_id());
        for (const PacketWorld::Mob& m : view.mobs) {
            TripMob t;
            t.type = m.type_id;
            t.hp = m.hp;
            t.dead = m.dead;
            t.x = m.x;
            t.y = m.y;
            s.mobs.push_back(t);
        }
        if (want_party && party_ok && now - party_ms > 3000) {
            party_ms = now;
            Frame f;
            if (grab(f)) {
                const PartyHud hud = party_reader.read(f);
                party_size = hud.state == PartyState::Unknown ? -1 : std::max(1, hud.size);
                party_covered = hud.covered;
            }
        }
        s.party = party_size;
        if (!s.stem.empty()) {
            const auto page = game_maps_.get(s.stem);
            // On no floor the file has (a step's lip, a reading in the air):
            // nothing to judge by, and anywhere goes, as with no map.
            if (page && page->floors.w_px() > 0)
                s.same_floor = [page](int x, int y, int x2) {
                    if (!page->floor_near(x, y)) return true;
                    return page->follow_to(x, y, x2).has_value();
                };
        }
        return s;
    };

    // ---- the hold, for a job's keys and clicks ----
    const auto with_hold = [&](const std::function<TripResult()>& fn) -> TripResult {
        if (!input_.window_focused()) return TripResult::Busy;
        std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::try_to_lock);
        if (!mouse.owns_lock() || trash_pass_) return TripResult::Busy;
        if (!take_hold(kTripHold)) return TripResult::Busy;
        OnExit end{ [this] { end_hold(kTripHold); } };
        for (int i = 0; i < 20 && coord_.held() && !halted(); ++i) sleep_ms(50);
        input_.release_all();
        return fn();
    };
    const auto press = [&](std::uint32_t vk, int hold_ms) {
        return held_ok() && input_.window_focused() && input_.key_press(vk, hold_ms);
    };
    const auto click = [&](int x, int y, int presses = 1) {
        return held_ok() && input_.click_on_game(x, y, false, [&] { return !held_ok(); }, nullptr, true, presses);
    };
    const auto find_ok = [&](const Frame& f, ScreenRect* at) {
        for (const char* name : kOkPictures) {
            if (!store->has(name)) continue;
            const TemplateMatcher::Match m = store->find(name, f, ScreenRect{ 0, 0, f.width, f.height }, kOkScore);
            if (m.found) {
                if (at) *at = m.at;
                return true;
            }
        }
        return false;
    };
    // A notice's one Xác Nhận (2026.11.21, the audit's B7): one of a box with
    // [Xác Nhận][Hủy Bỏ] -- a party invitation, a trade, the bin's question
    // -- is no notice. The entry's watch took one for a refusal and clicked
    // it, which joined a party party.ini says no to; the broken pickaxe's
    // notice the same. The same Xác Nhận picture, the two buttons the
    // dialogs' own (bag::find_dialogs).
    const auto find_notice_ok = [&](const Frame& f, ScreenRect* at) {
        ScreenRect ok;
        if (!find_ok(f, &ok)) return false;
        for (const bag::Dialog& d : bag::find_dialogs(f, *store))
            if (d.pair.overlaps(ok)) {
                GB_DEBUG("Expedition: a Xác Nhận at %d,%d is a two-button box's (an invitation, a question) -- not "
                         "a notice, not clicked",
                         ok.x, ok.y);
                return false;
            }
        if (at) *at = ok;
        return true;
    };
    const auto epoch_now = [this] { return packet_world_.place().epoch; };

    // ---- the jobs ----
    const auto do_walk = [&](const TripJob& j, bool fight) -> TripResult {
        if (!hunter_ || !hunter_->running()) return TripResult::Busy;
        const std::uint32_t epoch = epoch_now();
        const std::uint64_t start = now_ms();
        std::uint64_t sent = 0;
        // The time the hunter could not walk (B5): another feature's hold, the
        // game behind another window. Not counted against the walk -- but for
        // a walk with its own limit (the reward room's 12 s, out within
        // 30 s), which stays as it is; and never past kWalkMostMs.
        std::uint64_t blocked = 0;
        std::uint64_t last = start;
        while (!halted()) {
            const std::uint64_t now = now_ms();
            // Tự đánh quái stopped under the walk: nothing walks it now, and
            // no failure of the walk's (Busy, the planner asks again).
            if (!hunter_->running()) {
                say_expedition("Tự đánh quái đã dừng khi đang đi");
                return TripResult::Busy;
            }
            const bool other_hold = coord_.critical_active() && coord_.critical_reason() != kTripHold;
            if (other_hold || !input_.window_focused()) blocked += std::min<std::uint64_t>(now - last, 1000);
            last = now;
            if (now - sent > 3000) {
                // The boss: its spot the area's home, the area wide -- the
                // hunter fights only inside it, and the boss need not stand
                // within GoC's 48 x 31 of the spot (GoC fights anything, and
                // walks back to the spot after 5 s away).
                const int ax = fight ? kFightHalfW : j.tol_x;
                const int ay = fight ? kFightHalfH : j.tol_y;
                hunter_->set_trip(j.stem, epoch_now(), j.x, j.y, ax, ay, !fight);
                sent = now;
            }
            if (fight) {
                // Into the boss room the character stands at the entry, a
                // little above the floor (Cổ Long 64,620 over 632), and no
                // report puts it down until it moves: the hunter stood 24 s
                // there on 2026-11-19, taking every monster for a drop away.
                // One short step towards the spot puts it on the floor.
                const TripSnap s = snap(false);
                if (s.world && s.stem == j.stem && std::abs(s.x - j.x) < 400.0f &&
                    std::abs(s.y - j.y) > 6.0f && std::abs(s.y - j.y) < 60.0f) {
                    const std::uint32_t arrow = s.x < j.x ? VK_RIGHT : VK_LEFT;
                    with_hold([&] { return press(arrow, 250) ? TripResult::Ok : TripResult::Busy; });
                }
                return TripResult::Ok;
            }
            const TripSnap s = snap(false);
            // Down: no walk's failure either -- the planner waits for the
            // revive (Busy, B5).
            if (s.hp_known && s.hp == 0) return TripResult::Busy;
            if (epoch_now() != epoch) {
                say_expedition("Đã sang map khác khi đang đi -- tính lại");
                return TripResult::Retry;
            }
            if (s.world && s.stem == j.stem && s.settled && std::abs(s.x - j.x) <= static_cast<float>(j.tol_x) &&
                std::abs(s.y - j.y) <= static_cast<float>(j.tol_y))
                return TripResult::Ok;
            const std::uint64_t walked = now - start - (j.max_ms ? 0 : std::min(blocked, now - start));
            if (walked > (j.max_ms ? j.max_ms : kWalkMs) || now - start > kWalkMostMs) {
                char b[200];
                std::snprintf(b, sizeof b, "Đi quá lâu chưa tới %.0f,%.0f (đang ở %.0f,%.0f; %llu s bị chức năng khác giữ)",
                              j.x, j.y, s.x, s.y, static_cast<unsigned long long>(blocked / 1000));
                say_expedition(b);
                return TripResult::Retry;
            }
            sleep_ms(200);
        }
        return TripResult::Busy;
    };

    const auto do_gate = [&](const TripJob& j) -> TripResult {
        const std::uint32_t epoch = epoch_now();
        return with_hold([&] {
            for (int i = 0; i < std::max(1, j.count) && held_ok(); ++i) {
                if (epoch_now() != epoch) return TripResult::Ok;
                if (input_.window_focused()) input_.gate_down(80);
                for (int t = 0; t < 6 && held_ok(); ++t) {
                    sleep_ms(50);
                    if (epoch_now() != epoch) return TripResult::Ok;
                }
            }
            if (epoch_now() != epoch) return TripResult::Ok;
            say_expedition("Bấm ↓ " + std::to_string(j.count) + " lần mà chưa qua cổng");
            return TripResult::Retry;
        });
    };

    // An NPC by a click on it (TripJob::by_click): where it is on screen from
    // your own name plate (under your feet) and how far it stands from you on
    // the map -- the camera moves both alike. Its body is clicked, above its
    // feet. False when that cannot be worked out (no plate seen since the map
    // came, not in that map).
    constexpr int kNpcBodyAboveFeet = 35;
    const auto click_npc = [&](const TripJob& j) -> bool {
        if (!j.by_click) return false;
        Nameplate me;
        bool fresh = false;
        const std::uint32_t epoch = epoch_now();
        for (int i = 0; i < 20 && !halted(); ++i) {
            if (detector_.player(me, &fresh) && fresh && me.plate.valid()) break;
            fresh = false;
            sleep_ms(150);
        }
        const TripSnap s = snap(false);
        if (!fresh || !s.world || s.stem != j.stem || epoch_now() != epoch) return false;
        const int x = me.plate.cx() + static_cast<int>(std::lround(j.x - s.x));
        const int y = me.plate.y + static_cast<int>(std::lround(j.y - s.y)) - kNpcBodyAboveFeet;
        Frame f;
        if (grab(f) && (x < 0 || y < 0 || x >= f.width || y >= f.height)) {
            GB_INFO("Expedition: the NPC at %.0f,%.0f is off the screen (%d,%d) -- you at %.0f,%.0f", j.x, j.y, x, y,
                    s.x, s.y);
            return false;
        }
        GB_INFO("Expedition: clicking the NPC at %.0f,%.0f -- on screen %d,%d (you at %.0f,%.0f, your name at %d,%d)",
                j.x, j.y, x, y, s.x, s.y, me.plate.cx(), me.plate.y);
        return click(x, y);
    };

    const auto do_talk = [&](const TripJob& j) -> TripResult {
        const std::uint32_t key = j.item ? j.item : 0x20;
        return with_hold([&] {
            if (j.by_click) {
                if (click_npc(j)) {
                    sleep_ms(600);
                    return TripResult::Ok;
                }
                // The key too, but a retry: three of them and the NPC is
                // walked to (the room's steps from the start).
                say_expedition("Chưa bấm được vào NPC (chưa thấy tên nhân vật trên màn hình); thử phím nói chuyện");
                press(key, 50);
                sleep_ms(600);
                return TripResult::Retry;
            }
            if (!press(key, 50)) return TripResult::Busy;
            sleep_ms(600);
            return TripResult::Ok;
        });
    };

    const auto do_pick_row = [&](const TripJob& j) -> TripResult {
        return with_hold([&] {
            Frame f;
            MenuText::Hit best;
            for (int i = 0; i < 12 && held_ok(); ++i) {
                if (!grab(f)) {
                    sleep_ms(150);
                    continue;
                }
                const ScreenRect area{ f.width / 10, f.height / 20, f.width * 8 / 10, f.height * 17 / 20 };
                const MenuText::Hit h = MenuText::find(f, area, j.text);
                if (h.score > best.score) best = h;
                if (h.found) {
                    GB_INFO("Expedition: '%s' found at %d,%d (%.2f, %s %d px%s%s)", j.text.c_str(), h.at.x, h.at.y,
                            static_cast<double>(h.score), h.face.c_str(), h.px, h.bold ? " bold" : "",
                            h.dark ? ", dark on light" : "");
                    if (!click(h.at.cx(), h.at.cy())) return TripResult::Busy;
                    if (!j.confirm) {
                        sleep_ms(400);
                        return TripResult::Ok;
                    }
                    // GoC waits up to 3 s for the box after the row, and clicks it.
                    for (int k = 0; k < 20 && held_ok(); ++k) {
                        sleep_ms(150);
                        Frame g;
                        ScreenRect ok;
                        if (grab(g) && find_ok(g, &ok)) {
                            if (!click(ok.cx(), ok.cy())) return TripResult::Busy;
                            sleep_ms(300);
                            return TripResult::Ok;
                        }
                    }
                    say_expedition("Chưa thấy hộp xác nhận sau khi chọn '" + j.text + "'");
                    keep(f, "no_confirm");
                    return TripResult::Retry;
                }
                sleep_ms(150);
            }
            char b[200];
            std::snprintf(b, sizeof b, "Chưa thấy dòng '%s' (gần nhất %.2f, cần %.2f)", j.text.c_str(),
                          static_cast<double>(best.score), static_cast<double>(MenuText::kMinScore));
            say_expedition(b);
            keep(f, "no_row");
            press(VK_ESCAPE, 50);
            return TripResult::Retry;
        });
    };

    const auto do_click_ok = [&](const TripJob& j) -> TripResult {
        return with_hold([&] {
            Frame f;
            for (int i = 0; i < std::max(1, j.count) && held_ok(); ++i) {
                ScreenRect ok;
                if (grab(f) && find_ok(f, &ok)) {
                    if (!click(ok.cx(), ok.cy())) return TripResult::Busy;
                    sleep_ms(300);
                    return TripResult::Ok;
                }
                sleep_ms(150);
            }
            say_expedition("Chưa thấy nút Xác Nhận");
            keep(f, "no_ok");
            return TripResult::Retry;
        });
    };

    const auto do_watch_refusal = [&](const TripJob&) -> TripResult {
        const std::uint32_t epoch = epoch_now();
        ScreenRect last{};
        int same = 0;
        for (int i = 0; i < 34 && !halted(); ++i) {
            sleep_ms(150);
            if (epoch_now() != epoch) return TripResult::Ok;
            Frame f;
            ScreenRect ok;
            if (!grab(f) || !find_notice_ok(f, &ok)) {
                same = 0;
                continue;
            }
            same = std::abs(ok.x - last.x) <= 3 && std::abs(ok.y - last.y) <= 3 ? same + 1 : 1;
            last = ok;
            if (same >= 2) {
                keep(f, "refused");
                const TripResult r = with_hold([&] {
                    return click(ok.cx(), ok.cy()) ? TripResult::Refused : TripResult::Busy;
                });
                return r == TripResult::Busy ? TripResult::Refused : r;
            }
        }
        return TripResult::Ok;
    };

    const auto do_start_dig = [&](const TripJob& j) -> TripResult {
        const std::uint64_t seq = trip_packets_.dig(packet_world_.own_id()).seq;
        const TripResult pressed = with_hold([&] {
            return press(j.item ? j.item : 0x20, 50) ? TripResult::Ok : TripResult::Busy;
        });
        if (pressed != TripResult::Ok) return pressed;
        for (int i = 0; i < 30 && !halted(); ++i) {
            sleep_ms(100);
            const TripPackets::Dig d = trip_packets_.dig(packet_world_.own_id());
            if (d.state == 2 && d.seq > seq) return TripResult::Ok;
        }
        say_expedition("Chưa nhận xác nhận bắt đầu đào (gói 0x0132); sẽ thử lại");
        return TripResult::Retry;
    };

    const auto do_exit_dig = [&](const TripJob&) -> TripResult {
        return with_hold([&] {
            if (trip_packets_.dig(packet_world_.own_id()).state == 1) return TripResult::Ok;
            press(VK_ESCAPE, 50);
            for (int i = 0; i < 15 && held_ok(); ++i) {
                sleep_ms(300);
                if (trip_packets_.dig(packet_world_.own_id()).state == 1) return TripResult::Ok;
                Frame f;
                ScreenRect ok;
                if (grab(f) && find_ok(f, &ok)) {
                    click(ok.cx(), ok.cy());
                    for (int k = 0; k < 30 && held_ok(); ++k) {
                        sleep_ms(100);
                        if (trip_packets_.dig(packet_world_.own_id()).state == 1) return TripResult::Ok;
                    }
                    say_expedition("Chưa nhận xác nhận tắt đào; sẽ thử lại");
                    return TripResult::Retry;
                }
            }
            press(VK_ESCAPE, 50);
            say_expedition("Không thấy hộp xác nhận thoát đào khoáng");
            return TripResult::Retry;
        });
    };

    const auto do_close_notice = [&](const TripJob&) -> TripResult {
        return with_hold([&] {
            int gone = 0;
            for (int i = 0; i < 24 && held_ok(); ++i) {
                Frame f;
                ScreenRect ok;
                if (grab(f) && find_notice_ok(f, &ok)) {
                    gone = 0;
                    if (!click(ok.cx(), ok.cy())) press(0x20, 50);
                } else if (++gone >= 3) {
                    return TripResult::Ok;
                }
                sleep_ms(250);
            }
            say_expedition("Chưa đóng được thông báo cuốc bị phá hủy; sẽ thử lại");
            return TripResult::Retry;
        });
    };

    const auto do_equip = [&](const TripJob& j) -> TripResult {
        const BagLayout& l = bag_settings.layout;
        const int col = j.slot % std::max(1, l.cols);
        const int row = j.slot / std::max(1, l.cols);
        return with_hold([&] {
            Frame f;
            BagView v;
            for (int i = 0; i < 2 && held_ok(); ++i) {
                if (grab(f)) v = bag::find(f, *store, l);
                if (v.found) break;
                press(bag_settings.bag_key, 50);
                sleep_ms(500);
            }
            if (!v.found) {
                say_expedition("Không mở được túi đồ để trang bị cuốc");
                keep(f, "no_bag");
                return TripResult::Retry;
            }
            int tx = 0, ty = 0;
            v.tab_point(l, 0, tx, ty);
            if (!click(tx, ty)) return TripResult::Busy;
            sleep_ms(400);
            if (grab(f)) v = bag::find(f, *store, l);
            const int first = v.first_row > 0 ? v.first_row : 0;
            if (!v.found || row < first || row >= first + l.rows) {
                char b[160];
                std::snprintf(b, sizeof b,
                              "Cuốc nằm ở hàng %d của Trang bị 1, ngoài phần túi đang hiện -- hãy để cuốc ở 4 hàng đầu",
                              row + 1);
                say_expedition(b);
                keep(f, "pickaxe_row");
                press(bag_settings.bag_key, 50);
                return TripResult::Retry;
            }
            const ScreenRect cell = v.slot(l, col, row - first);
            const std::uint64_t seq = trip_packets_.worn().seq;
            if (!click(cell.cx(), cell.cy(), 2)) return TripResult::Busy;
            for (int i = 0; i < 30 && held_ok(); ++i) {
                sleep_ms(100);
                const TripPackets::Tab w = trip_packets_.worn();
                if (w.seq > seq && std::any_of(w.slots.begin(), w.slots.end(),
                                               [](const TripSlot& t) { return is_pickaxe(t.item); })) {
                    press(bag_settings.bag_key, 50);
                    return TripResult::Ok;
                }
            }
            keep(f, "equip");
            press(bag_settings.bag_key, 50);
            say_expedition("Chưa xác nhận trang bị cuốc (gói 0x0065)");
            return TripResult::Retry;
        });
    };

    // ---- an NPC's shop: Lư Hương Long Mỹ, a pickaxe (2026.11.24) ----
    //
    // The owner, 2026-11-24: "khi làm đội trưởng để đi Cổ Long thì không mua
    // được Lư Hương. Cụ thể là khi ấn vào Lư Hương thì không ấn mua mà lại tắt
    // luôn shop mua". The purchase was one click on the item's name, Mua 250 ms
    // later, a Xác Nhận looked for once 500 ms after that, 2 s for the bag --
    // and on any miss ESC three times, which shut the shop. Mua is not the way
    // this client sells by: the box it brings up took no key on the live client
    // (2026-10-31), its Hủy Bỏ drawn pressed instead, and the player's way is a
    // double-click on the item's cell, which brings up the quantity question
    // with the keys going into it. Nor was a box come later than half a second
    // ever answered on a slow PC. And the pointer, left where the "Shop" row was
    // clicked, rested over the list as the shop opened: the game draws the
    // description of the item under it, the item's name in it, over the list --
    // which MenuText could find and click in place of the list's own name.
    //
    // Now as the potion shop has bought live since 2026-10-31 (BagCleaner::
    // shop_purchase): the pointer off the list; the name looked for in the list
    // alone (bag::shop_list), as the list draws it with no spaces and as
    // written; its icon box double-clicked (bag::shop_cell) -- never Mua, never
    // anywhere near Thoát; what comes up waited for kReactMs (the owner's 30 s
    // for slow PCs); the quantity question's box read over a blink of its caret
    // and only what is needed for one typed and read back, then Xác Nhận; a
    // question with no box answered Xác Nhận; the item seen come in the bag's
    // frames. A purchase that did not go is tried again in the same shop,
    // kShopTries in all; a message of the game's (the bag full, the money
    // short) is closed and the shop left; and the shop is shut as the potion
    // shop's is (shop_close): one ESC, Thoát only on a look that shows it still
    // up. Each step that does not go says so on the card and keeps its screen
    // in expedition_debug\ as shop_*.png.
    CountReader digits;
    const bool digits_ok = digits.load(GlyphFont::default_dir());
    if (!digits_ok) GB_WARN("Expedition: the game's digits are not there -- an NPC shop's quantity box cannot be read");
    // Where the item comes -- Trang bị 1 (a pickaxe) or Khác (Lư Hương) -- and
    // how many of it the bag's frames show.
    const auto tab_of = [this](const TripJob& j) { return j.other_tab ? trip_packets_.other() : trip_packets_.equip1(); };
    const auto held_of = [](const TripJob& j, const TripPackets::Tab& tab) {
        int c = 0;
        for (const TripSlot& t : tab.slots)
            if (t.item != 0 && (t.item == j.item || t.item == j.item2)) c += std::max<int>(1, t.count);
        return c;
    };
    const auto whole = [](const Frame& f) { return ScreenRect{ 0, 0, f.width, f.height }; };
    // The shop's Mua | Thoát on `f`: anywhere, or within a few pixels of where
    // they were (`was`). Not valid when not there.
    const auto shop_buttons = [&](const Frame& f, const ScreenRect* was) {
        if (!f.valid() || !store->has(bag::kShopButtons)) return ScreenRect{};
        ScreenRect area = whole(f);
        if (was != nullptr) {
            const int x0 = std::max(0, was->x - 8), y0 = std::max(0, was->y - 8);
            area = ScreenRect{ x0, y0, std::min(f.width, was->right() + 8) - x0,
                               std::min(f.height, was->bottom() + 8) - y0 };
        }
        const TemplateMatcher::Match m = store->find(bag::kShopButtons, f, area, kShopButtonsScore);
        return m.found ? m.at : ScreenRect{};
    };
    // The dialogs of two buttons on `f` but the shop's own Mua | Thoát at `b`,
    // which are two buttons side by side as well (BagCleaner::shop_filter).
    const auto dialogs_over = [&](const Frame& f, const ScreenRect& b) {
        std::vector<bag::Dialog> out;
        const ScreenRect own{ b.x - 4, b.y - 4, b.w + 8, b.h + 8 };
        for (const bag::Dialog& d : bag::find_dialogs(f, *store))
            if (!own.overlaps(d.pair)) out.push_back(d);
        return out;
    };
    // The question whose two buttons were at `pair` still there on `f`: found
    // there, or either of its buttons alone there (bag::pair_still).
    const auto question_at = [&](const Frame& f, const ScreenRect& pair, const ScreenRect& b) {
        for (const bag::Dialog& d : dialogs_over(f, b))
            if (std::abs(d.pair.x - pair.x) <= 3 && std::abs(d.pair.y - pair.y) <= 3) return true;
        return bag::pair_still(f, *store, pair);
    };
    // The pointer onto the bare panel left of Mua -- a move, no click: an item
    // under it has its description drawn over the list, and over where the
    // question comes up (BagCleaner::shop_purchase).
    const auto off_list = [&](const ScreenRect& b) {
        if (held_ok()) input_.move_pointer(b.x - 30, b.y + bag::kShopButtonY);
    };
    // A message of the game's with one Xác Nhận, its label at `label` on `f`:
    // its screen kept as `what`, its Xác Nhận clicked and the pointer taken off
    // it (a lit button is not seen for one), seen gone on two looks in a row
    // within half of kReactMs -- clicked once more when it is not. False when
    // it was never seen gone.
    const auto close_message = [&](Frame& f, ScreenRect label, const ScreenRect& b, const std::string& what) {
        keep(f, what);
        for (int n = 0; n < 2 && held_ok(); ++n) {
            if (!click(label.cx(), label.cy())) return false;
            off_list(b);
            const std::uint64_t clicked = now_ms();
            int gone = 0;
            while (held_ok() && now_ms() - clicked < static_cast<std::uint64_t>(kReactMs / 2)) {
                sleep_ms(kShopLookMs);
                if (!grab(f)) continue;
                ScreenRect again;
                if (bag::lone_ok(f, *store, whole(f), &again)) {
                    gone = 0;
                    label = again;
                } else if (++gone >= 2) {
                    return true;
                }
            }
        }
        if (held_ok()) say_expedition("Chưa đóng được thông báo của game (ảnh " + what + ")");
        return false;
    };
    // Hủy Bỏ on the question at `d`, on a look that shows it still there.
    const auto cancel = [&](const bag::Dialog& d, const ScreenRect& b) {
        Frame g;
        if (!grab(g) || !question_at(g, d.pair, b)) return;
        click(d.cancel_x(), d.y());
        off_list(b);
        sleep_ms(kShopSettleMs);
    };
    // What the quantity box of the question at `pair` holds, over a blink of
    // its caret (BagCleaner::shop_box): the number kShopBoxAgree looks in a row
    // read alike; kBoxGone with the question no longer there on two looks;
    // CountReader::kUnreadable when no number held still for kShopBoxWatchMs.
    const auto read_box = [&](const ScreenRect& pair, const ScreenRect& b, Frame& last) {
        int agreed = CountReader::kUnreadable, same = 0, missing = 0;
        for (int waited = 0; waited <= kShopBoxWatchMs && held_ok(); waited += kShopBoxLookMs) {
            if (grab(last)) {
                if (!question_at(last, pair, b)) {
                    if (++missing >= 2) return kBoxGone;
                } else {
                    missing = 0;
                    const int n = digits.read_field(last, bag::quantity_box(pair));
                    if (n != CountReader::kUnreadable) {
                        same = n == agreed ? same + 1 : 1;
                        agreed = n;
                        if (same >= kShopBoxAgree) return agreed;
                    }
                }
            }
            sleep_ms(kShopBoxLookMs);
        }
        return static_cast<int>(CountReader::kUnreadable);
    };

    // One purchase of `j`'s item in the shop whose Mua | Thoát are at `b`
    // (moved to where each look finds them): its name found in the list, its
    // icon double-clicked, what came up answered, the item seen in the bag's
    // frames. Again: not bought, tried again in this shop; Over: the game said
    // no, or the shop is gone -- nothing more here; Unsure: the question went
    // with nothing said and no frame of the bag came to say either way --
    // counted as bought, as the potion shop counts one (never bought twice).
    // `why` says what did not go.
    enum class ShopTry { Bought, Unsure, Again, Over, Busy };
    const auto buy_once = [&](const TripJob& j, ScreenRect& b, std::string& why) -> ShopTry {
        // 1. The pointer off the list and a moment for the description it lit
        //    to go; then the item's name in the list, as the list draws it --
        //    with no spaces -- and as written: the better of the two, a name in
        //    an item's place first, for up to kShopNameMs.
        off_list(b);
        sleep_ms(kShopSettleMs);
        std::vector<std::string> texts{ shop_list_text(j.text) };
        if (texts.front() != j.text) texts.push_back(j.text);
        Frame f;
        MenuText::Hit hit;
        bag::ShopCell cell;
        int missing = 0;
        const std::uint64_t from = now_ms();
        while (held_ok()) {
            if (grab(f)) {
                const ScreenRect now = shop_buttons(f, &b);
                if (!now.valid()) {
                    if (++missing >= 2) {
                        keep(f, "shop_gone");
                        why = "Shop không còn mở";
                        return ShopTry::Over;
                    }
                } else {
                    missing = 0;
                    b = now;
                    hit = MenuText::Hit{};
                    cell = bag::ShopCell{};
                    for (const std::string& text : texts) {
                        const MenuText::Hit h = MenuText::find(f, bag::shop_list(b), text);
                        const bag::ShopCell c = h.found ? bag::shop_cell(h.at, b) : bag::ShopCell{};
                        if ((c.ok != cell.ok) ? c.ok : (h.score > hit.score)) {
                            hit = h;
                            cell = c;
                        }
                    }
                    if (cell.ok) break;
                }
            }
            if (now_ms() - from >= kShopNameMs) break;
            sleep_ms(kShopLookMs);
        }
        if (!held_ok()) return ShopTry::Busy;
        if (!cell.ok) {
            char line[256];
            if (!hit.found)
                std::snprintf(line, sizeof line, "chưa tìm thấy %s trong danh sách của Shop (gần nhất %.2f, cần %.2f)",
                              j.text.c_str(), static_cast<double>(hit.score), static_cast<double>(MenuText::kMinScore));
            else
                std::snprintf(line, sizeof line,
                              "thấy chữ %s ở %d,%d nhưng không đúng chỗ tên một món của Shop (%s) -- không bấm vào đó",
                              j.text.c_str(), hit.at.x, hit.at.y, cell.why);
            GB_INFO("Expedition: '%s' not taken in the shop at %d,%d -- best %.2f at %d,%d %dx%d (%s), %s", j.text.c_str(),
                    b.x, b.y, static_cast<double>(hit.score), hit.at.x, hit.at.y, hit.at.w, hit.at.h, hit.face.c_str(),
                    hit.found ? cell.why : "under the score");
            why = line;
            keep(f, hit.found ? "shop_item_place" : "shop_no_item");
            return ShopTry::Again;
        }
        GB_INFO("Expedition: '%s' in the shop at %d,%d (%.2f, %s %d px%s) -- column %d, its icon at %d,%d (the name's end "
                "puts it at %d); Mua | Thoát at %d,%d",
                j.text.c_str(), hit.at.x, hit.at.y, static_cast<double>(hit.score), hit.face.c_str(), hit.px,
                hit.bold ? " bold" : "", cell.column, cell.x, cell.y, cell.by_name_x, b.x, b.y);

        // 2. Nothing up over the shop: a question left from the last try would
        //    take the double-click, a message hide the list.
        {
            const std::vector<bag::Dialog> up = dialogs_over(f, b);
            ScreenRect label;
            if (!up.empty()) {
                keep(f, "shop_dialog_up");
                cancel(up.front(), b);
                why = "đang có một hộp thoại mở trên Shop trước khi chọn món: đã bấm Hủy Bỏ";
                return ShopTry::Again;
            }
            if (bag::lone_ok(f, *store, whole(f), &label)) {
                close_message(f, label, b, "shop_message_before");
                why = "đang có một thông báo của game trên Shop trước khi chọn món: đã bấm Xác Nhận để tắt";
                return ShopTry::Again;
            }
        }

        // 3. The item's icon double-clicked, as the player buys -- the quantity
        //    question comes up with the keys going into it -- and the pointer
        //    off the list at once: the item's description would come up over
        //    where the question comes.
        const int before = held_of(j, tab_of(j));
        // The tab as it was, too (2026.11.24 review): the item may come under
        // an id GoBot does not know -- VTC's Lư Hương may not be GoC's
        // 8990188/8991014, a pickaxe has its variant ids -- and held_of counts
        // j.item / j.item2 only. Counted as bought as well when that tab, read
        // before the click, gained anything (tab_gained) -- else the "not in
        // the bag" path tried again and paid for a second one, and a third.
        const TripPackets::Tab before_tab = tab_of(j);
        const auto got_more = [&] {
            const TripPackets::Tab now = tab_of(j);
            return held_of(j, now) > before || (before_tab.known && tab_gained(before_tab, now));
        };
        say_expedition("Bấm đúp vào " + j.text + " trong Shop");
        if (!click(cell.x, cell.y, 2)) return ShopTry::Busy;
        off_list(b);

        // 4. What that brings up, a look every kShopLookMs for kReactMs (the
        //    owner's 30 s for slow PCs): the quantity question, a question with
        //    no box, the item in the bag at once, or the game's no.
        const std::uint64_t clicked = now_ms();
        BuyLook seen = BuyLook::Nothing, last = BuyLook::Nothing;
        int same = 0;
        bag::Dialog q;
        ScreenRect label, full_at;
        Frame g;
        while (held_ok() && now_ms() - clicked < static_cast<std::uint64_t>(kReactMs)) {
            sleep_ms(kShopLookMs);
            if (!grab(g)) continue;
            BuyLookIn in;
            in.more_in_bag = got_more();
            in.full = bag::shop_full_up(g, *store, &full_at);
            const std::vector<bag::Dialog> up = dialogs_over(g, b);
            in.dialogs = static_cast<int>(up.size());
            if (up.size() == 1) {
                const bag::Dialog& d = up.front();
                const float field = bag::quantity_field(g, d.pair);
                in.box = field >= bag::kFieldBox;
                in.bare = field <= bag::kFieldNone;
                in.known = d.ours() || d.registers || (in.bare && bag::other_question_up(g, *store, d));
                // The game's question frame, the sell question's; with a box,
                // the box reading as well.
                in.question = (in.box || in.bare) &&
                              bag::shop_question(g, *store, d, digits, bag_settings.buy_frame_score, in.box);
                q = d;
            } else if (up.empty() && !in.full) {
                in.message = bag::lone_ok(g, *store, whole(g), &label);
            }
            const BuyLook l = buy_look(in);
            same = l == last ? same + 1 : 1;
            last = l;
            if (l == BuyLook::Nothing) continue;
            // At once: the bag, the full-bag message, the question (its box is
            // read over looks of its own). The rest on two looks in a row.
            if (l == BuyLook::Bought || l == BuyLook::Full || l == BuyLook::Question || same >= 2) {
                seen = l;
                break;
            }
        }
        if (!held_ok()) return ShopTry::Busy;
        GB_INFO("Expedition: after the double-click on '%s': %s, %llu ms", j.text.c_str(), buy_look_name(seen),
                static_cast<unsigned long long>(now_ms() - clicked));
        switch (seen) {
            case BuyLook::Bought:
                say_expedition("Đã mua 1 " + j.text + " (túi đồ đã có thêm, không có hộp hỏi)");
                return ShopTry::Bought;
            case BuyLook::Full: {
                ScreenRect ok = label;
                if (!bag::lone_ok(g, *store, whole(g), &ok))
                    ok = ScreenRect{ full_at.x + bag::kShopFullOkX - 4, full_at.y + bag::kShopFullOkY - 4, 8, 8 };
                close_message(g, ok, b, "shop_full");
                why = "game báo túi đồ đã đầy";
                return ShopTry::Over;
            }
            case BuyLook::Message:
                close_message(g, label, b, "shop_message");
                why = "game hiện một thông báo thay cho hộp nhập số lượng (có thể thiếu tiền hoặc túi đầy; ảnh "
                      "shop_message): đã bấm Xác Nhận để tắt";
                return ShopTry::Over;
            case BuyLook::Other:
                keep(g, "shop_other_dialog");
                for (const bag::Dialog& d : dialogs_over(g, b)) cancel(d, b);
                why = "hiện ra một hộp thoại không giống hộp mua hàng (ảnh shop_other_dialog): đã bấm Hủy Bỏ";
                return ShopTry::Again;
            case BuyLook::Nothing:
                // Over, not Again (2026.11.24 review): each such try held the
                // trip 30 s, three in a row, then 30 s later the same -- the
                // trash, charms, shop and the leader's party invites starved
                // ("không để các chức năng block nhau"). The visit ends, the
                // shop is shut, the planner talks to the NPC again after its
                // 30 s with the other jobs having had their turn meanwhile.
                keep(g, "shop_no_question");
                why = "đã bấm đúp vào " + j.text + " mà 30 giây không thấy hộp nhập số lượng (ảnh shop_no_question)";
                return ShopTry::Over;
            case BuyLook::Question:
            case BuyLook::Confirm:
                break;
        }

        // 5. The quantity question: what its box holds, as the game filled it
        //    in -- a 1 in the potion shop's -- and only what is needed for one
        //    typed, each key on a look that shows the question still there,
        //    then read back: up to kReactMs after the keys on a slow game.
        if (seen == BuyLook::Question) {
            Frame box_look;
            const int have = read_box(q.pair, b, box_look);
            if (!held_ok()) return ShopTry::Busy;
            const QuantityKeys k = quantity_keys(have, 1);
            if (!k.ok) {
                keep(box_look, "shop_box");
                cancel(q, b);
                why = have == kBoxGone ? "hộp nhập số lượng biến mất trước khi đọc được số"
                                       : "không đọc được số trong hộp nhập số lượng (ảnh shop_box): đã bấm Hủy Bỏ";
                return ShopTry::Again;
            }
            say_expedition("Hộp nhập số lượng đang chứa " + std::to_string(have));
            if (k.erase > 0 || !k.digits.empty()) {
                bool sent = true;
                Frame kf;
                for (int i = 0; i < k.erase && sent; ++i) {
                    sent = grab(kf) && question_at(kf, q.pair, b) && press(VK_BACK, 50);
                    sleep_ms(kShopKeyGapMs);
                }
                for (std::size_t i = 0; i < k.digits.size() && sent; ++i) {
                    // A digit's key code is its character.
                    sent = grab(kf) && question_at(kf, q.pair, b) && press(static_cast<std::uint32_t>(k.digits[i]), 50);
                    sleep_ms(kShopKeyGapMs);
                }
                if (!held_ok()) return ShopTry::Busy;
                // Read again, three looks alike each time, until it shows
                // something else than before the keys -- a slow game may not
                // have drawn them yet -- or kReactMs after them.
                int now_has = have;
                const std::uint64_t typed = now_ms();
                while (sent && held_ok() && now_ms() - typed < static_cast<std::uint64_t>(kReactMs)) {
                    now_has = read_box(q.pair, b, box_look);
                    if (now_has != have && now_has != CountReader::kUnreadable) break;
                }
                if (!held_ok()) return ShopTry::Busy;
                if (!sent || now_has != 1) {
                    keep(box_look, "shop_keys");
                    cancel(q, b);
                    why = "gõ số 1 vào hộp nhập số lượng không được (đọc lại được " +
                          (now_has >= 0 ? std::to_string(now_has) : std::string("không rõ")) +
                          ", ảnh shop_keys): đã bấm Hủy Bỏ";
                    return ShopTry::Again;
                }
                say_expedition("Đã nhập 1 vào hộp nhập số lượng");
            }
        }

        // 6. Xác Nhận, on a look that shows the question still where it was --
        //    then the pointer off it: on the button it would light a message
        //    come in its place.
        if (!grab(g) || !question_at(g, q.pair, b)) {
            keep(g, "shop_changed");
            why = "hộp thoại không còn ở chỗ cũ lúc sắp bấm Xác Nhận";
            return ShopTry::Again;
        }
        const std::uint64_t seq = tab_of(j).seq;
        if (!click(q.confirm_x(), q.y())) return ShopTry::Busy;
        off_list(b);

        // 7. What came of it, a look every kShopLookMs for kReactMs: the item
        //    in the bag's frames; the game's no in a message, closed; the
        //    question gone with nothing after it, and the bag's frames waited
        //    for kShopBagFrameMs more; the question still up at the end,
        //    answered Hủy Bỏ.
        const std::uint64_t confirmed = now_ms();
        std::uint64_t gone_at = 0;
        int away = 0, lone = 0;
        while (held_ok() && now_ms() - confirmed < static_cast<std::uint64_t>(kReactMs)) {
            sleep_ms(kShopLookMs);
            if (got_more()) {
                say_expedition("Đã mua 1 " + j.text);
                return ShopTry::Bought;
            }
            if (!grab(g)) continue;
            if (bag::shop_full_up(g, *store, &full_at)) {
                ScreenRect ok;
                if (!bag::lone_ok(g, *store, whole(g), &ok))
                    ok = ScreenRect{ full_at.x + bag::kShopFullOkX - 4, full_at.y + bag::kShopFullOkY - 4, 8, 8 };
                close_message(g, ok, b, "shop_full");
                why = "game báo túi đồ đã đầy";
                return ShopTry::Over;
            }
            if (question_at(g, q.pair, b)) {
                away = lone = 0;
                gone_at = 0;
                continue;
            }
            if (dialogs_over(g, b).empty() && bag::lone_ok(g, *store, whole(g), &label)) {
                if (++lone >= 2) {
                    close_message(g, label, b, "shop_message_after");
                    if (got_more()) {
                        say_expedition("Đã mua 1 " + j.text + " (game có hiện thêm một thông báo, đã tắt)");
                        return ShopTry::Bought;
                    }
                    why = "sau khi bấm Xác Nhận game hiện một thông báo (có thể thiếu tiền hoặc túi đầy; ảnh "
                          "shop_message_after): đã bấm Xác Nhận để tắt";
                    return ShopTry::Over;
                }
                continue;
            }
            lone = 0;
            if (++away >= 2 && gone_at == 0) gone_at = now_ms();
            if (gone_at != 0 && now_ms() - gone_at >= kShopBagFrameMs) break;
        }
        if (!held_ok()) return ShopTry::Busy;
        if (gone_at == 0) {
            keep(g, "shop_question_stays");
            cancel(q, b);
            why = "đã bấm Xác Nhận mà 30 giây hộp thoại vẫn chưa đóng (ảnh shop_question_stays): đã bấm Hủy Bỏ";
            return ShopTry::Again;
        }
        // Gone with nothing said, and no more of the item in the bag's frames:
        // with no frame of that tab since the click, not known either way; with
        // one, not bought.
        if (tab_of(j).seq == seq) {
            say_expedition("Đã bấm Xác Nhận mua " + j.text + "; chưa có gói tin túi đồ để xác nhận -- tính là đã mua");
            return ShopTry::Unsure;
        }
        if (got_more()) {
            say_expedition("Đã mua 1 " + j.text + " (túi đồ có thêm một món)");
            return ShopTry::Bought;
        }
        keep(g, "shop_not_in_bag");
        why = "đã bấm Xác Nhận, hộp thoại đã đóng mà túi đồ chưa có thêm " + j.text + " (ảnh shop_not_in_bag)";
        return ShopTry::Again;
    };

    // The shop shut as the potion shop's is (BagCleaner::shop_close): ESC once
    // -- the game closes the window in front -- and looked at for kReactMs, on
    // at the second look without it; still up, its Thoát on a look that shows
    // it, and looked at again. ESC is never pressed twice: with nothing left to
    // close it brings up the game's own menu. Something of the game's in front
    // of the shop before the ESC -- the bag, the check-in board, a dialog, a
    // message -- seen gone after it with the shop still up took that ESC: Thoát
    // at once, not after the whole wait. A shop the check-in board lies over is
    // not taken for gone.
    const auto close_shop = [&](ScreenRect b) {
        Frame f;
        const auto in_front = [&](const Frame& g) {
            ScreenRect label;
            return bag::find(g, *store, bag_settings.layout).found || checkin_board_title(g, *store) ||
                   !dialogs_over(g, b).empty() || bag::lone_ok(g, *store, whole(g), &label);
        };
        // A look before the ESC (2026.11.24 review): after buy_once saw the
        // shop gone (shop_gone) the ESC would land on nothing and bring up the
        // game's own menu over the lobby. No shop (Mua | Thoát anywhere) and no
        // check-in board on two looks: nothing to shut, nothing pressed.
        {
            int none = 0;
            for (int k = 0; k < 2 && held_ok(); ++k) {
                if (k > 0) sleep_ms(kShopLookMs);
                if (grab(f) && !shop_buttons(f, nullptr).valid() && !checkin_board_title(f, *store)) ++none;
            }
            if (none >= 2) return true;
            if (!held_ok()) return false;
        }
        const bool front_before = grab(f) && in_front(f);
        say_expedition("Bấm ESC để đóng Shop");
        if (!press(VK_ESCAPE, 50)) return false;
        for (int step = 0; step < 2 && held_ok(); ++step) {
            const std::uint64_t from = now_ms();
            int gone = 0;
            bool front_went = false;
            while (held_ok() && now_ms() - from < static_cast<std::uint64_t>(kReactMs)) {
                sleep_ms(kShopLookMs);
                if (!grab(f)) continue;
                const ScreenRect now = shop_buttons(f, &b);
                if (!now.valid()) {
                    if (!checkin_board_title(f, *store) && ++gone >= 2) return true;
                    continue;
                }
                gone = 0;
                b = now;
                if (step == 0 && front_before && !in_front(f)) {
                    front_went = true;
                    break;
                }
            }
            if (!held_ok() || step == 1) break;
            // Still up: its Thoát, where a look now shows it -- and only there.
            if (!grab(f)) break;
            const ScreenRect now = shop_buttons(f, &b);
            if (!now.valid()) continue;
            b = now;
            say_expedition(front_went ? "ESC đã đóng cửa sổ khác trước Shop, bấm Thoát"
                                      : "ESC chưa đóng được Shop, bấm Thoát");
            if (!click(b.x + bag::kShopThoatX, b.y + bag::kShopButtonY)) break;
        }
        if (!held_ok()) return false;
        keep(f, "shop_not_closed");
        say_expedition("Chưa đóng được Shop (ESC rồi nút Thoát) -- hãy đóng giúp trong game");
        return false;
    };

    // The bag the game opens beside the shop (2026.11.24, the owner: "1 số lượt
    // cuối mua Lư Hương xong không tự tắt túi đồ bằng esc dẫn tới không rõ đã đủ
    // thành viên hay chưa"). The shop's ESC took the shop when the shop was in
    // front, the bag stayed over the party panels, and the party read "at least
    // 2/6, some of the panels hidden" for minutes (live log 21:46 and 22:11; at
    // 21:23 the bag was in front, took the ESC, and Thoát shut the shop -- both
    // gone). So, with the shop gone: the bag seen up gets one ESC -- it is what is
    // in front then, so the ESC never brings up the game's menu -- and is looked
    // at for kReactMs; still up, one more. Not seen: nothing pressed.
    const auto close_bag_left = [&] {
        Frame f;
        const auto bag_up = [&](const Frame& g) { return bag::find(g, *store, bag_settings.layout).found; };
        for (int n = 0; n < 2 && held_ok(); ++n) {
            // A look that failed is "not known", not "closed" (2026.11.24
            // review): looked again, a few times.
            bool looked = false;
            for (int k = 0; k < 3 && !looked && held_ok(); ++k) {
                if (k > 0) sleep_ms(kShopLookMs);
                looked = grab(f);
            }
            if (!looked) return false;
            if (!bag_up(f)) return true;
            say_expedition("Túi đồ còn mở sau khi đóng Shop -- bấm ESC để tắt");
            if (!press(VK_ESCAPE, 50)) return false;
            const std::uint64_t from = now_ms();
            int gone = 0;
            while (held_ok() && now_ms() - from < static_cast<std::uint64_t>(kReactMs)) {
                sleep_ms(kShopLookMs);
                if (!grab(f)) continue;
                if (bag_up(f)) {
                    gone = 0;
                    continue;
                }
                if (++gone >= 2) return true;
            }
        }
        if (!held_ok()) return false;
        keep(f, "bag_not_closed");
        say_expedition("Chưa tắt được túi đồ sau khi mua (ảnh bag_not_closed) -- GoBot thử lại khi chờ ở sảnh");
        return false;
    };

    const auto do_buy = [&](const TripJob& j) -> TripResult {
        // With no digits of the game's no quantity box is read, and with no
        // pictures of the shop's questions none is told from another: nothing
        // is clicked.
        if (!digits_ok || !bag::can_check_shop(*store)) {
            say_expedition("Không mua được " + j.text + ": GoBot thiếu " +
                           (digits_ok ? std::string("hình Shop và hộp thoại của game") : std::string("bộ chữ số của game")));
            return TripResult::Retry;
        }
        return with_hold([&] {
            // 1. The shop, for kReactMs: its "Shop" row was clicked just before.
            Frame f;
            ScreenRect b;
            const std::uint64_t asked = now_ms();
            while (held_ok()) {
                if (grab(f)) {
                    b = shop_buttons(f, nullptr);
                    if (b.valid()) break;
                }
                if (now_ms() - asked >= static_cast<std::uint64_t>(kReactMs)) break;
                sleep_ms(kShopLookMs);
            }
            if (!held_ok()) return TripResult::Busy;
            if (!b.valid()) {
                say_expedition(std::string("Chưa thấy Shop của ") + (j.other_tab ? "NPC" : "Thợ Mỏ") +
                               " sau 30 giây (nút Mua | Thoát, ảnh shop_not_seen) -- sẽ nói chuyện lại");
                keep(f, "shop_not_seen");
                // The NPC's talk box, when that is what is up: one ESC -- only
                // when a fresh look shows something to shut (2026.11.24
                // review): with nothing up the ESC brings up the game's own
                // menu, and with the visit retried every 30 s the next ESC
                // shut it and the one after opened it again. A dialog of two
                // buttons, a lone message, or the NPC's talk rows ("Shop").
                Frame g;
                if (grab(g)) {
                    ScreenRect label;
                    const ScreenRect area{ g.width / 10, g.height / 20, g.width * 8 / 10, g.height * 17 / 20 };
                    if (!bag::find_dialogs(g, *store).empty() || bag::lone_ok(g, *store, whole(g), &label) ||
                        MenuText::find(g, area, kShopRowText).found)
                        press(VK_ESCAPE, 50);
                }
                return TripResult::Retry;
            }
            GB_INFO("Expedition: the shop's Mua | Thoát at %d,%d -- buying %d '%s'", b.x, b.y, std::max(1, j.count),
                    j.text.c_str());
            // 2. Each one wanted, kShopTries tries in this shop; the game's no
            //    or the shop gone ends the visit.
            const int want = std::max(1, j.count);
            int got = 0, unsure = 0;
            bool over = false;
            for (int n = 0; n < want && !over; ++n) {
                bool bought = false;
                for (int t = 0; t < kShopTries && !bought && !over; ++t) {
                    std::string why;
                    const ShopTry r = buy_once(j, b, why);
                    if (r == ShopTry::Busy) return TripResult::Busy;
                    if (r == ShopTry::Bought || r == ShopTry::Unsure) {
                        bought = true;
                        ++got;
                        if (r == ShopTry::Unsure) ++unsure;
                        continue;
                    }
                    say_expedition("Mua " + j.text + " lần " + std::to_string(t + 1) + "/" +
                                   std::to_string(kShopTries) + " chưa được: " + why);
                    over = r == ShopTry::Over;
                }
                if (!bought) over = true;
            }
            // 3. The shop shut, the proven way.
            if (!held_ok()) return TripResult::Busy;
            // The bag looked at whatever close_shop said (2026.11.24 review):
            // left over the party panels it held the lobby without end.
            close_shop(b);
            close_bag_left();
            std::string done = "Đã mua " + std::to_string(got) + "/" + std::to_string(want) + " " + j.text;
            if (unsure > 0)
                done += " (" + std::to_string(unsure) + " lần chưa có gói tin túi đồ để xác nhận, vẫn tính là đã mua)";
            say_expedition(done);
            return got > 0 ? TripResult::Ok : TripResult::Retry;
        });
    };

    const auto do_loot = [&](const TripJob& j) -> TripResult {
        std::uint32_t key = j.item ? j.item : hunt_config_.loot_key;
        if (key == 0) key = VK_CONTROL;
        return with_hold([&] {
            for (int i = 0; i < std::max(1, j.count) && held_ok(); ++i) {
                press(key, 50);
                sleep_ms(250);
            }
            return TripResult::Ok;
        });
    };

    const auto do_complete = [&](const TripJob& j) -> TripResult {
        return with_hold([&] {
            Frame f;
            for (int i = 0; i < 12 && held_ok(); ++i) {
                if (grab(f)) {
                    for (const char* name : kCompletePictures) {
                        if (!store->has(name)) continue;
                        const TemplateMatcher::Match m =
                            store->find(name, f, ScreenRect{ 0, 0, f.width, f.height }, kCompleteScore);
                        if (m.found) {
                            if (!click(m.at.cx(), m.at.cy())) return TripResult::Busy;
                            sleep_ms(500);
                            return TripResult::Ok;
                        }
                    }
                }
                sleep_ms(150);
            }
            // Not seen: the talk key, which takes the box's Hoàn Thành (the
            // 2026.11.19 run: the Space of the talk after it completed the
            // quest), and on to leaving -- the next talk offers the way out.
            say_expedition("Chưa thấy nút Hoàn Thành; bấm phím nói chuyện để nhận thưởng");
            keep(f, "no_complete");
            press(j.item ? j.item : 0x20, 50);
            sleep_ms(800);
            return TripResult::Ok;
        });
    };

    const auto do_exit_room = [&](const TripJob& j) -> TripResult {
        // Out quickly (the owner: within 30 s of the reward room): each look
        // the way-out row by its text first, then a Xác Nhận, then the talk
        // box's Hoàn Thành, else the talk key -- which opens the box, takes
        // its Hoàn Thành and picks its row as well.
        const std::uint32_t epoch = epoch_now();
        const std::uint64_t start = now_ms();
        const std::uint64_t most = j.max_ms ? j.max_ms : 20000;
        int rounds = 0;
        // Rounds the hold could not be had for (2026.11.21, the audit's B6):
        // nothing was looked at or pressed, so none of them is a try. Every
        // one so: Busy, asked again in kBusyMs -- not "not out after N tries".
        int busy = 0;
        while (!halted() && rounds < std::max(1, j.count) && now_ms() - start < most) {
            if (epoch_now() != epoch) return TripResult::Ok;
            ++rounds;
            const TripResult tried = with_hold([&] {
                Frame f;
                ScreenRect ok;
                if (!grab(f)) return TripResult::Ok;
                if (!j.text.empty()) {
                    const ScreenRect area{ f.width / 10, f.height / 20, f.width * 8 / 10, f.height * 17 / 20 };
                    const MenuText::Hit h = MenuText::find(f, area, j.text);
                    if (h.found) {
                        GB_INFO("Expedition: '%s' found at %d,%d (%.2f) -- clicked", j.text.c_str(), h.at.x, h.at.y,
                                static_cast<double>(h.score));
                        click(h.at.cx(), h.at.cy());
                        // Then its Xác Nhận, as soon as it shows.
                        for (int k = 0; k < 13 && held_ok(); ++k) {
                            sleep_ms(150);
                            if (epoch_now() != epoch) return TripResult::Ok;
                            Frame g;
                            if (grab(g) && find_ok(g, &ok)) {
                                click(ok.cx(), ok.cy());
                                return TripResult::Ok;
                            }
                        }
                        return TripResult::Ok;
                    }
                }
                if (find_ok(f, &ok)) {
                    click(ok.cx(), ok.cy());
                    return TripResult::Ok;
                }
                if (store->has(kCompletePictures[0])) {
                    const TemplateMatcher::Match m = store->find(kCompletePictures[0], f,
                                                                 ScreenRect{ 0, 0, f.width, f.height }, kCompleteScore);
                    if (m.found) {
                        click(m.at.cx(), m.at.cy());
                        return TripResult::Ok;
                    }
                }
                // Nothing up: talk to the NPC -- a click on it, else the key.
                if (click_npc(j)) {
                    sleep_ms(450);
                    return TripResult::Ok;
                }
                press(j.item ? j.item : 0x20, 50);
                return TripResult::Ok;
            });
            if (tried == TripResult::Busy) ++busy;
            for (int t = 0; t < 7 && !halted(); ++t) {
                sleep_ms(50);
                if (epoch_now() != epoch) return TripResult::Ok;
            }
        }
        if (epoch_now() != epoch) return TripResult::Ok;
        if (rounds > 0 && busy == rounds) {
            say_expedition("Chưa rời phó bản: chức năng khác đang giữ game (" +
                           (coord_.critical_active() ? coord_.critical_reason() : std::string("game không ở trên cùng")) +
                           ")");
            return TripResult::Busy;
        }
        char b[160];
        std::snprintf(b, sizeof b, "Chưa rời phó bản sau %d lần thử (%.0f s)", rounds,
                      static_cast<double>(now_ms() - start) / 1000.0);
        say_expedition(b);
        return TripResult::Retry;
    };

    const auto run_job = [&](const TripJob& j) -> TripResult {
        switch (j.kind) {
            case TripJobKind::Walk:          return do_walk(j, false);
            case TripJobKind::Fight:         return do_walk(j, true);
            case TripJobKind::Gate:          return do_gate(j);
            case TripJobKind::Talk:          return do_talk(j);
            case TripJobKind::PickRow:       return do_pick_row(j);
            case TripJobKind::ClickOk:       return do_click_ok(j);
            case TripJobKind::WatchRefusal:  return do_watch_refusal(j);
            case TripJobKind::StartDig:      return do_start_dig(j);
            case TripJobKind::ExitDig:       return do_exit_dig(j);
            case TripJobKind::Equip:         return do_equip(j);
            case TripJobKind::Buy:           return do_buy(j);
            case TripJobKind::CloseNotice:   return do_close_notice(j);
            case TripJobKind::Loot:          return do_loot(j);
            case TripJobKind::CompleteQuest: return do_complete(j);
            case TripJobKind::ExitRoom:      return do_exit_room(j);
            case TripJobKind::None:          return TripResult::Ok;
        }
        return TripResult::Ok;
    };

    MiningPlanner mining;
    DungeonPlanner cothu(kCoThu);
    DungeonPlanner colong(kCoLong);
    std::string said_mining, said_cothu, said_colong;
    bool said_hunter = false;
    bool stopped_mid_trip = false;    ///< Tự đánh quái stopped with a trip set
    TripQuiet said_quiet = TripQuiet::None;     ///< the last trip_quiet_ logged (B6)
    say_expedition("Bắt đầu");

    while (!expedition_stop_) {
        sleep_ms(200);
        const std::uint64_t now = now_ms();
        if (now - read_ms > 1000) {
            ExpeditionSettings fresh;
            if (fresh.load(config)) settings = fresh;
            read_ms = now;
            mining_on_ = settings.mining.enabled;
            dungeon_on_ = settings.cothu.enabled || settings.colong.enabled;
        }
        if (!settings.mining.enabled && !settings.cothu.enabled && !settings.colong.enabled) {
            if (hunter_) hunter_->end_trip();
            trip_quiet_.clear();
            std::lock_guard<std::mutex> lock(worker_mutex_);
            ui.mining_status = "Đào khoáng: chưa bật";
            ui.cothu_status = "Cổ Thụ: chưa bật";
            ui.colong_status = "Cổ Long: chưa bật";
            continue;
        }
        // GoC: "Cần bật Enable, chạy Auto": the walks are the hunter's.
        // Stopped under a trip (2026.11.21, the audit's B3): the trip ended
        // -- not left set for the next start -- and why the hunt stopped said
        // on the card, where before it said only "Chờ Tự đánh quái chạy".
        if (!hunter_ || !hunter_->running()) {
            // Nothing of the run's goes on with the hunt stopped: the other
            // features' jobs no longer wait for it (B6).
            trip_quiet_.clear();
            std::string why = "Chờ Tự đánh quái chạy";
            if (hunter_) {
                if (hunter_->trip_set()) stopped_mid_trip = true;
                const bool trip = stopped_mid_trip;
                // Not in the boss's room (2026.11.21, the review of B20;
                // DungeonPlanner::trip_outlives_stop): the Fight trip is set
                // once, by the Spot job, and nothing sends it again -- a stop
                // of GoBot's own (the bin's hold, a chat doubt) or F11 that
                // ended it left the rest of the fight on the card's area, and
                // outside that area the hunter stops fighting. Kept there, it
                // is the boss's area again as Tự đánh quái goes on.
                const PacketWorld::Place here = packet_world_.place();
                const std::string stem_here = here.map ? GameMapBook::stem_of(here.map, here.area) : std::string();
                const bool boss_room =
                    (settings.cothu.enabled && DungeonPlanner::trip_outlives_stop(cothu.zone(stem_here))) ||
                    (settings.colong.enabled && DungeonPlanner::trip_outlives_stop(colong.zone(stem_here)));
                if (!boss_room) hunter_->end_trip();
                // A drop or a jump never stops the hunt since 2026.11.24 -- a
                // map change is learned as a gate, a key-up owed lets every key
                // go -- so the hunt stopped mid-trip is the player's own stop
                // (DỪNG, F11, unticked) or another of GoBot's (the bin's hold,
                // a chat doubt). Until then a press's map change or a key-up
                // stuck stopped it, and the card said so.
                if (trip) why = "Tự đánh quái đã dừng giữa chuyến đi -- bấm BẮT ĐẦU để đi tiếp";
                if (trip && !said_hunter) say_expedition(why);
            }
            if (!said_hunter) say_expedition("Cần bật Tự đánh quái và bấm BẮT ĐẦU: GoBot đi bằng phần đánh quái");
            said_hunter = true;
            std::lock_guard<std::mutex> lock(worker_mutex_);
            ui.mining_status = ui.cothu_status = ui.colong_status = why;
            continue;
        }
        said_hunter = false;
        stopped_mid_trip = false;

        // The party HUD is read only for a leader in a lobby.
        const PacketWorld::Place place = packet_world_.place();
        const std::string stem_now = place.map ? GameMapBook::stem_of(place.map, place.area) : std::string();
        const auto leads_here = [&](const CoThuConfig& c, const DungeonPlanner& p) {
            return c.enabled && c.role == 0 && p.zone(stem_now) == DungeonPlanner::Zone::Lobby;
        };
        TripSnap s = snap(leads_here(settings.cothu, cothu) || leads_here(settings.colong, colong));
        // Mining in its zone, each dungeon in its own; with none, each says why.
        TripJob j;
        DungeonPlanner* dungeon = nullptr;
        const CoThuConfig* dungeon_cfg = nullptr;
        bool mine_job = false;
        if (settings.mining.enabled && in_mining_zone(s.stem)) {
            j = mining.next(s, settings.mining, now);
            mine_job = true;
        } else if (settings.cothu.enabled && cothu.zone(s.stem) != DungeonPlanner::Zone::None) {
            dungeon = &cothu;
            dungeon_cfg = &settings.cothu;
        } else if (settings.colong.enabled && colong.zone(s.stem) != DungeonPlanner::Zone::None) {
            dungeon = &colong;
            dungeon_cfg = &settings.colong;
        } else {
            if (settings.mining.enabled) mining.next(s, settings.mining, now);
            if (settings.cothu.enabled) cothu.next(s, settings.cothu, now);
            if (settings.colong.enabled) colong.next(s, settings.colong, now);
            if (hunter_) hunter_->end_trip();
        }
        if (dungeon) j = dungeon->next(s, *dungeon_cfg, now);
        // The boss's room and the reward's (2026.11.21, the audit's B6; the
        // owner: "không để các chức năng block nhau"): the other features'
        // timed jobs -- a trash pass with its shop ride, the charms, a quest
        // round, a Tự chat line, a party invite -- wait while it is on, put
        // off, not dropped (TripQuietFlag, capped). Each of them took the game
        // in the boss fight, the hunter swinging at nothing under its hold,
        // and in the reward room the NPC's 15 s and the 30 s to be out went
        // by under it. Not with the map unknown for a moment: left as it was,
        // and off by itself after TripQuietFlag::kStaleMs. No planner stops
        // for good since 2026.11.24: one in its gap of kTripRetryMs in the
        // boss's room still fights there, and keeps it.
        if (s.world) {
            const TripQuiet q = dungeon ? DungeonPlanner::quiet_in(dungeon->zone(s.stem)) : TripQuiet::None;
            if (q != said_quiet) {
                GB_INFO("Expedition: %s -- the other features' timed jobs (trash, charms, shop, quest, chat, party "
                        "invites) %s",
                        trip_quiet_name(q == TripQuiet::None ? said_quiet : q),
                        q == TripQuiet::None ? "over: they go again" : "on: they wait for it");
                said_quiet = q;
            }
            trip_quiet_.set(q, now);
        }
        if (dungeon && hunter_) {
            const DungeonPlanner::Zone z = dungeon->zone(s.stem);
            const DungeonPlanner::Step st = dungeon->step();
            // Where the game moves the party with no key of GoBot's (B3): in
            // after the entry's Xác Nhận, from the boss to the reward, a death
            // back to the waiting room, out of the reward room. A map change
            // there is the dungeon's, not the last drop's or jump's. A member
            // waiting in the lobby too (the review of 2026.11.21): the game
            // moves it in when its leader enters, with no key or job of its
            // own GoBot's, its hunter hunting there meanwhile.
            if (z != DungeonPlanner::Zone::Lobby || st == DungeonPlanner::Step::Watch ||
                st == DungeonPlanner::Step::WaitEntry || st == DungeonPlanner::Step::Member)
                hunter_->expect_map_change(kExpectMapMs);
            // Waiting in the lobby -- the party short, a member for its leader,
            // the runs all made -- with no walk to come soon: the trip ended
            // (2026.11.21, the audit's B20). The reward room's quiet walk
            // stayed set there for hours, the hunter fighting nothing and the
            // macro's skills held. The countdown and the retry's wait keep it:
            // the walk to the NPC comes next.
            if (j.kind == TripJobKind::None && z == DungeonPlanner::Zone::Lobby &&
                (st == DungeonPlanner::Step::Ready || st == DungeonPlanner::Step::Member))
                hunter_->end_trip();
        }
        // A planner in its gap of kTripRetryMs (2026.11.24, the owner: "Kiểm
        // tra các phần khác xem có phần nào không làm được thì dừng không, cần
        // thay cơ chế này bằng 1 cơ chế khác" -- the other useful thing first,
        // then again): the trip ended, so that the hunter fights where the
        // character stands -- in the mining maps and the waiting room too --
        // and the next job sets it again. Not in the boss's room: its Fight
        // trip is the fight's own area, and the fight goes on in the gap. Where
        // a planner stopped for good before, the trip ended with it and the
        // card said so in red until the tab was unticked.
        if (hunter_ && j.kind == TripJobKind::None &&
            ((mine_job && mining.resting(now)) ||
             (dungeon && dungeon->resting(now) && dungeon->zone(s.stem) != DungeonPlanner::Zone::Boss)))
            hunter_->end_trip();
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            ui.mining_status = settings.mining.enabled ? mining.status() : "Đào khoáng: chưa bật";
            ui.cothu_status = settings.cothu.enabled ? cothu.status() : "Cổ Thụ: chưa bật";
            ui.colong_status = settings.colong.enabled ? colong.status() : "Cổ Long: chưa bật";
            ui.runs = cothu.runs();
            ui.colong_runs = colong.runs();
        }
        if (mining.status() != said_mining && settings.mining.enabled) {
            said_mining = mining.status();
            GB_INFO("Mining: %s", said_mining.c_str());
        }
        if (cothu.status() != said_cothu && settings.cothu.enabled) {
            said_cothu = cothu.status();
            GB_INFO("CoThu: %s", said_cothu.c_str());
        }
        if (colong.status() != said_colong && settings.colong.enabled) {
            said_colong = colong.status();
            GB_INFO("CoLong: %s", said_colong.c_str());
        }
        // The bag left over the party panels in the lobby (2026.11.24 review):
        // the leader waited without end on "some of the panels hidden" when
        // close_bag_left after a purchase could not shut it. No asking the
        // player: the bag seen up on a fresh look gets one ESC -- it is what is
        // in front, so the ESC never brings up the game's menu -- at most once
        // every kTripRetryMs, with no limit.
        if (j.kind == TripJobKind::None && dungeon && party_covered &&
            dungeon->zone(s.stem) == DungeonPlanner::Zone::Lobby && dungeon->step() == DungeonPlanner::Step::Ready &&
            now - bag_fix_ms >= static_cast<std::uint64_t>(kTripRetryMs)) {
            bag_fix_ms = now;
            with_hold([&] {
                Frame f;
                if (grab(f) && bag::find(f, *store, bag_settings.layout).found) {
                    say_expedition("Túi đồ đang che bảng tổ đội -- bấm ESC để tắt");
                    press(VK_ESCAPE, 50);
                    party_ms = 0;
                }
                return TripResult::Ok;
            });
        }
        if (j.kind == TripJobKind::None) continue;
        say_expedition(j.label);
        // A job of keys and clicks may change the map -- the gate's ↓, the
        // Teleporter's row and its box, the dungeon's NPC and door, the way
        // out -- and the change may come after the job is over: neither is
        // the last drop's or jump's (Hunter::expect_map_change, B3). The walks
        // are the hunter's own, and judged as ever.
        const bool keys_job = j.kind != TripJobKind::Walk && j.kind != TripJobKind::Fight;
        if (keys_job && hunter_) hunter_->expect_map_change(kExpectMapMs);
        const TripResult r = run_job(j);
        if (keys_job && hunter_) hunter_->expect_map_change(kExpectMapMs);
        if (expedition_stop_) break;
        const TripSnap after = snap(false);
        const std::uint64_t over = now_ms();
        if (mine_job) mining.done(j, r, after, over);
        else if (dungeon) dungeon->done(j, r, after, over);
        // A gap begun by this job (2026.11.24): its line on the card's log --
        // where a stop's line was, in red -- and the planner asked again when
        // it is over, with no limit.
        if (mine_job ? mining.resting(over) : (dungeon && dungeon->resting(over)))
            say_expedition(mine_job ? mining.status() : dungeon->status());
    }
    if (hunter_) hunter_->end_trip();
    trip_quiet_.clear();
    say_expedition("Đã dừng");
    expedition_running_ = false;
}

// --------------------------------------------------------------------- tab

ExpeditionSettings& GuiApp::expedition_settings_for_tab() {
    if (!expedition_ui_) expedition_ui_.reset(new ExpeditionUi);
    ExpeditionUi& ui = *expedition_ui_;
    if (!ui.loaded) {
        ui.loaded = true;
        ui.settings.load(exe_relative("configs/expedition.ini"));
        mining_on_ = ui.settings.mining.enabled;
        dungeon_on_ = ui.settings.cothu.enabled || ui.settings.colong.enabled;
    }
    return ui.settings;
}

void GuiApp::expedition_settings_changed() {
    ExpeditionSettings& s = expedition_settings_for_tab();
    if (!s.save(exe_relative("configs/expedition.ini")))
        simple_note("Không lưu được configs/expedition.ini.", 8000);
    mining_on_ = s.mining.enabled;
    dungeon_on_ = s.cothu.enabled || s.colong.enabled;
    const bool any = mining_on_ || dungeon_on_;
    if (any && simple_running() && !simple_cold_start_ && keys_may_start()) start_expedition();
    if (!any && expedition_running_) stop_expedition();
}

namespace {
const char* kTripHint =
    "Làm theo GoC 0.6.5: GoBot đi bằng phần đánh quái, nên cần bật Tự đánh quái, Vị trí từ gói tin và bấm "
    "BẮT ĐẦU. Đi qua nhiều tầng cần bật Nhảy xuống bậc dưới và Nhảy lên bục trên. Màn hình chưa nhận ra được "
    "lưu ở thư mục expedition_debug.";
}

void GuiApp::simple_mining_tab() {
    trial_line(true);
    ExpeditionSettings& settings = expedition_settings_for_tab();
    ExpeditionUi& ui = *expedition_ui_;
    bool changed = false;
    std::vector<std::string> lines;
    std::string mining_status;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        lines = ui.log;
        mining_status = ui.mining_status;
    }
    hint(kTripHint);
    ImGui::Dummy(ImVec2(0, 4));

    MiningConfig& m = settings.mining;
    if (begin_card("##mining")) {
        heading(strong_font_, "Đào khoáng");
        changed |= ImGui::Checkbox("Tự đào khoáng", &m.enabled);
        hint("Đứng ở Thanh Âm Quan, Mỏ Quặng Thủy Quật hoặc trong mỏ: GoBot tự đi tới mỏ đã chọn (qua Teleporter, "
             "cổng bấm ↓), tìm chỗ đào cách cổng 192 px, bấm đánh thường để đào và đào lại khi dừng.");
        label("Mỏ");
        ImGui::SameLine();
        int mi = 0;
        for (std::size_t i = 0; i < kMines.size(); ++i)
            if (m.map_key == kMines[i].stem) mi = static_cast<int>(i);
        if (ImGui::BeginCombo("##mine", kMines[static_cast<std::size_t>(mi)].name)) {
            for (std::size_t i = 0; i < kMines.size(); ++i)
                if (ImGui::Selectable(kMines[i].name, static_cast<int>(i) == mi)) {
                    m.map_key = kMines[i].stem;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        changed |= ImGui::Checkbox("Tự động thay thế cuốc", &m.auto_replace);
        hint("Cuốc hỏng thì đóng thông báo và trang bị cuốc khác trong Trang bị 1 (để cuốc ở 4 hàng đầu).");
        changed |= ImGui::Checkbox("Tự mua cuốc", &m.auto_buy);
        if (m.auto_buy) {
            label("Loại cuốc");
            ImGui::SameLine();
            char cur[96];
            std::snprintf(cur, sizeof cur, "%s (%u)", pickaxe_name(m.buy_pickaxe), m.buy_pickaxe);
            if (ImGui::BeginCombo("##pickaxe", cur)) {
                for (const Pickaxe& p : kPickaxes) {
                    char item[96];
                    std::snprintf(item, sizeof item, "%s (%u)", p.name, p.id);
                    if (ImGui::Selectable(item, p.id == m.buy_pickaxe)) {
                        m.buy_pickaxe = p.id;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            label("Mua khi còn (cuốc)");
            ImGui::SameLine();
            changed |= ImGui::SliderInt("##buywhen", &m.buy_when, 0, 5);
            label("Số cuốc mua");
            ImGui::SameLine();
            changed |= ImGui::SliderInt("##buyqty", &m.buy_quantity, 1, 10);
            hint("Mua ở Thợ Mỏ (Mỏ Quặng Thủy Quật): 0 = chỉ mua khi đã hết cuốc. Mua chưa được thì thử lại sau 30 "
                 "giây; chưa thấy gói tin túi Trang bị 1 xác nhận lần mua trước thì không mua thêm.");
        }
        label("Phím đánh thường (nói chuyện, đào)");
        ImGui::SameLine();
        changed |= simple_key_box("minetalk", m.talk_key, false, false, nullptr);
        ImGui::Dummy(ImVec2(0, 2));
        coloured(m.enabled ? kWait : kDim, mining_status.empty() ? "Đào khoáng: chưa chạy" : mining_status.c_str());
        end_card();
    } else {
        end_card();
    }

    last_lines(lines, 8);
    if (changed) expedition_settings_changed();
}

void GuiApp::simple_expedition_tab() {
    trial_line(true);
    ExpeditionSettings& settings = expedition_settings_for_tab();
    ExpeditionUi& ui = *expedition_ui_;
    bool changed = false;
    std::vector<std::string> lines;
    std::string cothu_status, colong_status;
    int runs = 0, colong_runs = 0;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        lines = ui.log;
        cothu_status = ui.cothu_status;
        colong_status = ui.colong_status;
        runs = ui.runs;
        colong_runs = ui.colong_runs;
    }
    hint(kTripHint);
    ImGui::Dummy(ImVec2(0, 4));

    // One card per dungeon: the same settings, as GoC has them.
    const auto card = [&](const Dungeon& d, CoThuConfig& c, const std::string& status, int done_runs,
                          const char* title, const char* how, const char* ticket_hint) {
        ImGui::PushID(d.tag);
        if (begin_card("##dungeon")) {
            heading(strong_font_, title);
            changed |= ImGui::Checkbox((std::string("Tự đi ") + d.name).c_str(), &c.enabled);
            hint(how);
            static const char* kRoles[] = { "Đội trưởng", "Thành viên" };
            label("Vai trò");
            ImGui::SameLine();
            changed |= ImGui::Combo("##role", &c.role, kRoles, 2);
            if (c.role == 0) {
                label("Đủ số người (tính cả bạn)");
                ImGui::SameLine();
                changed |= ImGui::SliderInt("##minmem", &c.minimum_members, 1, 6);
                label("Chờ trước khi vào (giây)");
                ImGui::SameLine();
                changed |= ImGui::SliderInt("##wait", &c.wait_seconds, 0, 300);
                // No "Tự thử lại khi không vào được" since 2026.11.24: unticked,
                // a refusal stopped Phó bản for good. Its key stays in the file
                // (CoThuConfig::entry_retry), unread.
                label("Thử lại sau (giây)");
                ImGui::SameLine();
                changed |= ImGui::SliderInt("##retry", &c.entry_retry_seconds, 5, 300);
                hint("Không vào được phó bản thì GoBot luôn thử lại sau số giây này.");
                label("Số lượt tối đa (0 = không giới hạn)");
                ImGui::SameLine();
                changed |= ImGui::SliderInt("##runs", &c.maximum_runs, 0, 50);
                hint(ticket_hint);
            } else {
                hint("Thành viên: đứng chờ đội trưởng đưa vào, rồi tự đi tới cửa phòng boss, đánh boss, nhận thưởng.");
            }
            const char* spots[] = { d.spot_name[0], d.spot_name[1], "Chỗ vào phòng boss" };
            label("Chỗ đứng đánh boss");
            ImGui::SameLine();
            changed |= ImGui::Combo("##spot", &c.boss_position, spots, 3);
            if (d.boss_type != 0) {
                label("Số lần nhặt đồ sau boss");
                ImGui::SameLine();
                changed |= ImGui::SliderInt("##loot", &c.loot_presses, 0, 30);
            }
            label("Phím đánh thường (nói chuyện)");
            ImGui::SameLine();
            changed |= simple_key_box((std::string(d.tag) + "talk").c_str(), c.talk_key, false, false, nullptr);
            ImGui::Dummy(ImVec2(0, 2));
            const std::string idle = std::string(d.name) + ": chưa chạy";
            coloured(c.enabled ? kWait : kDim, status.empty() ? idle.c_str() : status.c_str());
            if (done_runs > 0) hint(("Đã vào phó bản " + std::to_string(done_runs) + " lượt.").c_str());
        }
        end_card();
        ImGui::PopID();
    };

    card(kCoLong, settings.colong, colong_status, colong_runs, "Phó bản Cổ Long (Ngọc Long Tổ)",
         "Bấm BẮT ĐẦU khi đang đứng ở sảnh Ngọc Long Tổ (t60_s19) hoặc trong phó bản: đội trưởng tự mua Lư Hương "
         "Long Mỹ ở NPC nếu chưa có, chờ đủ người, chọn Ngọc Long Tổ; vào map chuẩn bị thì đi tới cửa bấm ↓, đánh "
         "boss tới khi được đưa sang phòng nhận thưởng, nhận thưởng rồi rời phó bản.",
         "Đội trưởng cần Lư Hương Long Mỹ trong túi Khác; không có thì GoBot mua 1 ở Shop của NPC; mua chưa được thì "
         "thử lại sau 30 giây; chưa thấy gói tin túi Khác xác nhận lần mua trước thì không mua thêm. Số người đọc từ "
         "khung tổ đội trên màn hình.");
    card(kCoThu, settings.cothu, cothu_status, runs, "Phó bản Cổ Thụ (Ngọc Thụ Thần)",
         "Bấm BẮT ĐẦU khi đang đứng ở Trung Tâm Rừng Đại Thụ (hoặc trong phó bản): đội trưởng chờ đủ người, "
         "nói chuyện với NPC chọn Ngọc Thụ Thần; vào Phòng chờ thì đi tới cửa bấm ↓, đánh boss, nhặt đồ, nhận "
         "thưởng rồi rời phó bản.",
         "Đội trưởng cần Đèn Cấp Cao trong túi Khác. Số người đọc từ khung tổ đội trên màn hình.");
    last_lines(lines, 8);
    if (changed) expedition_settings_changed();
}

} // namespace gobot
