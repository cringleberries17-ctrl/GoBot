// Quick quest -- "Làm nhiệm vụ nhanh" -- as the GUI does it: the worker that
// runs a round every few minutes of a run, and the Nhiệm vụ tab.
//
// What a round does, its pictures and its timing are the library's
// (gobot/quick_quest.hpp). Here is what only the GUI knows: what the rest of
// the bot is doing -- the holds, the trash cleaner, Tự chat, the keeper, the HP
// reader -- and the game's own input. Everything else that presses keys is
// held off for the few seconds a round takes, as for a trash pass.

#include "gui_app.hpp"
#include "simple_ui.hpp"

#include "gobot/alerts.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/quiet_second.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

/// The hold a round is made under, by name.
const char* const kQuestHold = "quick quest";

/// How long nobody may touch the mouse or keyboard, with everything else held
/// off, before a round starts moving the pointer about -- as long as Tự chat
/// waits before closing a chat line. With the hold on GoBot presses nothing,
/// so whatever Windows counts meanwhile is a person's.
constexpr int kQuietMs = 1000;

/// Runs `fn` on the way out of a scope, however it is left.
struct OnExit {
    std::function<void()> fn;
    ~OnExit() { if (fn) fn(); }
};

std::string clock_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

DWORD last_input() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof info;
    return GetLastInputInfo(&info) ? info.dwTime : 0;
}

/// GoBot's own switches -- the start/stop, panic and hunting keys, and the
/// two one-job keys, "Dọn túi ngay" and "Vào game ngay" -- as the window's
/// thread has them, for the worker to check the quest key against.
constexpr int kOwnKeys = 5;

void set_own_keys(std::atomic<std::uint32_t> (&keys)[kOwnKeys], const MacroConfig& macro,
                  const HunterConfig& hunt, const HotkeyConfig& hot) {
    keys[0] = macro.toggle_key;
    keys[1] = macro.panic_key;
    keys[2] = hunt.toggle_key;
    keys[3] = hot.trash_now;
    keys[4] = hot.login_now;
}

std::vector<std::uint32_t> own_keys_of(const std::atomic<std::uint32_t> (&keys)[kOwnKeys]) {
    std::vector<std::uint32_t> out;
    for (const auto& k : keys) out.push_back(k.load());
    return out;
}

} // namespace

/// Everything the GUI keeps for quick quest.
struct GuiApp::QuestUi {
    /// The tab's copy of configs/quest.ini.
    QuestSettings settings;
    bool          loaded = false;
    int           interval = 5;
    /// Whether the pictures are there, looked at once when the tab is first
    /// shown, and what to say about it.
    bool          pictures_checked = false;
    bool          pictures_ok = false;
    std::string   pictures_line;

    // What the worker says, for the tab; guarded by worker_mutex_.
    std::vector<std::string> log;
    std::string   last;             ///< "Lần gần nhất lúc HH:MM: ..."
    std::string   stopped;          ///< why it stopped itself, in red
    std::uint64_t next_ms = 0;      ///< the next timed round, on now_ms()'s clock

    /// The round under way, to cancel.
    std::mutex    round_mutex;
    QuestRound*   round = nullptr;

    /// A window a round opened may still be open -- QuestRun keeps this for a
    /// run, and it is kept here from one worker to the next: a stop in the
    /// middle of a round ends the worker with the window open.
    std::atomic<bool> ours_open{false};
    /// GoBot's own switches -- start/stop, panic, hunting, the one-job keys --
    /// which the quest key must not be. Copied from the window's thread, whose
    /// settings they are, for the worker to read.
    std::atomic<std::uint32_t> own_keys[kOwnKeys] = {};
};

void GuiApp::QuestUiFree::operator()(QuestUi* ui) const { delete ui; }

void GuiApp::say_quest(const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        std::vector<std::string>& log = quest_ui_->log;
        log.push_back(line);
        if (log.size() > 200) log.erase(log.begin());
    }
    request_redraw();       // shown at once, as say_party's
}

// ------------------------------------------------------------------ worker

void GuiApp::start_quest() {
    vitals_for_job();       // HP and MP at the run's pace before any key
    if (!quest_ui_) quest_ui_.reset(new QuestUi);
    set_own_keys(quest_ui_->own_keys, macro_config_, hunt_config_, hotkeys_);
    {
        // A worker here already for a button -- a look, or one round -- becomes
        // this run, started from the first round's wait. Under the lock the
        // worker leaves by, so that it is not on its way out with the run lost.
        std::lock_guard<std::mutex> lock(quest_life_mutex_);
        if (quest_running_ && !quest_stop_) {
            quest_oneshot_ = false;
            quest_restart_ = true;
            return;
        }
    }
    join_pumping(quest_thread_);
    {
        // A new run, so whatever stopped the last one is old news.
        std::lock_guard<std::mutex> lock(worker_mutex_);
        quest_ui_->stopped.clear();
    }
    quest_stop_ = false;
    quest_oneshot_ = false;
    quest_restart_ = false;
    quest_request_ = 0;
    quest_running_ = true;
    quest_thread_ = std::thread([this] { quest_loop(); });
}

void GuiApp::request_quest(bool look_only) {
    vitals_for_job();       // a one-shot round watches HP too: at the run's pace before any key
    if (!quest_ui_) quest_ui_.reset(new QuestUi);
    set_own_keys(quest_ui_->own_keys, macro_config_, hunt_config_, hotkeys_);
    {
        std::lock_guard<std::mutex> lock(quest_life_mutex_);
        if (quest_running_ && !quest_stop_) {
            quest_request_ = look_only ? 2 : 1;
            return;
        }
    }
    join_pumping(quest_thread_);
    quest_request_ = look_only ? 2 : 1;
    quest_stop_ = false;
    quest_oneshot_ = true;
    quest_restart_ = false;
    quest_running_ = true;
    quest_thread_ = std::thread([this] { quest_loop(); });
}

void GuiApp::stop_quest(bool wait) {
    quest_stop_ = true;
    if (quest_ui_) {
        // A round under way stops at its next key or click, and presses
        // nothing more -- not even the key that would close the window.
        std::lock_guard<std::mutex> lock(quest_ui_->round_mutex);
        if (quest_ui_->round) quest_ui_->round->cancel();
    }
    // Told, and not waited for: stop_jobs tells every job first.
    if (!wait) return;
    join_pumping(quest_thread_);
    quest_running_ = false;
    quest_request_ = 0;
}

/// The timed rounds, and the ones asked for.
void GuiApp::quest_loop() {
    name_this_thread("quest");
    QuestUi& ui = *quest_ui_;
    // A round put off ("Chờ: ...") goes to the log as well, once for each
    // change of what it waits for: only the tab had it, and the farm of
    // 2026-11-03 had two rounds 364 s apart with nothing to say why. Anything
    // else said -- a round, a start -- ends the wait, and the same reason
    // after it is news again. A not-now after the hold is logged where it
    // is made (held_not_now), and not twice, nor again with the back-off's
    // next look added.
    std::string waited_for;
    const auto tell = [this, &waited_for](const std::string& vi) {
        if (vi.rfind("Chờ:", 0) != 0) {
            waited_for.clear();
        } else if (vi != waited_for) {
            // A spoiled quiet second's wait only adds when the next look is
            // ("; thử lại sau ...") to the not-now held_not_now has just
            // logged: that one is not logged again.
            const std::string noted =
                waited_for.empty() ? std::string() : waited_for.substr(0, waited_for.size() - 1) + ";";
            const bool known = !noted.empty() && vi.rfind(noted, 0) == 0;
            waited_for = vi;
            if (!known) GB_INFO("Quest: put off -- %s", vi.c_str());
        }
        say_quest(vi);
    };
    const auto leave = [&](const std::string& why) {
        if (!why.empty()) tell(why);
        std::lock_guard<std::mutex> lock(quest_life_mutex_);
        quest_running_ = false;
    };

    // Read again every second, and kept from one reading that worked to the
    // next: the tab writes the file key by key as things change.
    const std::string config = exe_relative("configs/quest.ini");
    QuestSettings settings;
    settings.load(config);
    std::uint64_t read_ms = now_ms();

    // Only the dozen pictures a round needs, not the whole set: this runs for
    // hours.
    QuestPictures pictures;
    std::string from;
    if (!pictures.load_best(settings, &from)) {
        const std::string why = quest_missing_vi(pictures);
        GB_WARN("Quest: %zu of its pictures are missing, the first %s -- not started",
                pictures.missing.size(), pictures.missing.empty() ? "?" : pictures.missing[0].c_str());
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            ui.stopped = why;
        }
        leave(why);
        return;
    }
    const std::string learned = pictures.box_learned.valid()
                                    ? "; the ticked box as learned here, from " + QuestPictures::learned_file()
                                    : std::string();
    GB_INFO("Quest: pictures from %s%s", from.c_str(), learned.c_str());

    ScreenCapture capture;
    capture.set_label("quest");
    const auto grab = [&](Frame& f) {
        return input_.window_valid() && capture.grab(input_.target_window(), f) && f.valid();
    };
    const std::string debug_dir = exe_relative("quest_debug");
    const auto crossing = [this] {
        std::lock_guard<std::mutex> lock(keeper_flow_mutex_);
        return keeper_flow_ != nullptr;
    };

    // F12, DỪNG, unticking; the keeper starting a crossing; the hold taken
    // over by something that outranks it.
    const auto halted = [this, crossing] {
        return quest_stop_.load() || crossing() || coord_.critical_reason() != kQuestHold;
    };

    // The game's own input, each checked first. A click lands on whatever is
    // on top at that point, and the frames come from PrintWindow, which shows
    // the game even when it is covered: so only with the game in front and
    // the game itself under the point -- click_on_game asks both, and asks
    // them again after the pointer's pause there. And asked about a stop once
    // more, as the very last thing before it goes: the round asks before each
    // step, and a stop that came between its asking and the key going down
    // still let that one key or click through.
    QuestRound::Hands hands;
    hands.grab = grab;
    hands.key = [this, halted](std::uint32_t vk, int hold_ms) {
        return !halted() && input_.window_focused() && input_.key_press(vk, hold_ms);
    };
    hands.click = [this, halted](int x, int y) {
        return !halted() && input_.click_on_game(x, y, false, halted);
    };
    hands.move = [this, halted](int x, int y) { return !halted() && input_.move_pointer(x, y); };
    hands.now = [] { return now_ms(); };
    hands.sleep = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    hands.stop = halted;
    // The round's hold keeps the HP potion off as well: under the potion's
    // mark it ends where it is, so the potion is not kept waiting for the rest
    // of it, as Tự chat ends a line. Not known while the bars are unread.
    hands.low_hp = [this] {
        const float hp = hp_percent_now();
        return hp >= 0.0f && hp < static_cast<float>(chat_hp_floor_.load());
    };
    // The player gone to another window part way: not now, not a failure.
    hands.behind = [this] { return !input_.window_focused(); };

    const auto gates = [this, crossing] {
        QuestGateInput in;
        in.window = input_.window_valid();
        in.focused = input_.window_focused();
        in.keeper = crossing();
        in.hp_pct = hp_percent_now();
        // Tự chat's rule: some way over what the HP potion fires at, so that
        // HP hovering at the mark in a fight does not start a round only for
        // the guard above to end it.
        const int floor = chat_hp_floor_.load();
        in.hp_floor = std::max(floor, std::min(95, floor + 10));
        const std::string reason = coord_.critical_active() ? coord_.critical_reason() : std::string();
        in.held_by = reason == kQuestHold ? std::string() : reason;
        in.trash = trash_pass_ || trash_hold_ || trash_left_held_;
        in.chat = chat_doubt_.load();
        return in;
    };

    // Why a round waits for the check-in board, and what would close it: with
    // "Tự đóng thông báo" off nothing does. app.ini read here, on this
    // worker's thread, as the trash worker reads it.
    const auto board_closes = [] {
        IniFile app;
        return app.load(exe_relative("configs/app.ini")) && app.get_bool("Simple", "close_popups", false);
    };
    const auto board_why = [](bool closes) {
        return std::string("đang có bảng Sự kiện và điểm danh") +
               (closes ? "" : "; bật Tự đóng thông báo để GoBot tự tắt bảng điểm danh");
    };
    bool board_said = false;            // the board's put-off logged, until a look without it

    // One round, with the mouse to itself and everything else held off.
    const auto one_round = [&](const QuestCall& call) -> QuestResult {
        QuestResult not_now;
        not_now.outcome = QuestOutcome::NotNow;
        // Never one of GoBot's own switches: GoBot would take its own press
        // for the player's. The tab refuses them; quest.ini is edited by hand.
        const std::string clash = quest_key_clash_vi(settings.quest_key, own_keys_of(ui.own_keys));
        if (!clash.empty()) {
            not_now.why = clash;
            return not_now;
        }
        // The check-in board up: nothing of the round goes into it, and the
        // keys are not held for a round that cannot run -- one look first,
        // with no hold. The round looks again on its own first frame, under
        // the hold (QuestRound::run). Put off QuestRun::kBoardMs, not trouble.
        if (pictures.board_title.valid()) {
            Frame seen;
            ScreenRect title;
            float score = 0.0f;
            if (grab(seen) && checkin_board_title(seen, pictures.board_title, &title, &score)) {
                const bool closes = board_closes();
                not_now.board = true;
                not_now.why = board_why(closes);
                if (!board_said)
                    GB_INFO("Quest: the check-in board is up at %d,%d (title %.2f) -- quest put off %d s%s", title.x,
                            title.y, static_cast<double>(score), static_cast<int>(QuestRun::kBoardMs / 1000),
                            closes ? "" : " (close_popups is off: nothing closes it)");
                board_said = true;
                return not_now;
            }
            board_said = false;
        }
        // Not while the trash cleaner, the revive sequence or the party
        // worker is clicking, nor they while this is.
        std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::try_to_lock);
        if (!mouse.owns_lock() || trash_pass_) {
            not_now.why = trash_pass_ ? "đang dọn rác" : "bot đang bấm chuột trong game";
            return not_now;
        }
        if (!take_hold(kQuestHold)) {
            not_now.why = "bot đang tạm dừng (" + coord_.critical_reason() + ")";
            return not_now;
        }
        OnExit hold{ [this] { end_hold(kQuestHold); } };
        // A not-now from here on held the keys off for a moment: said in the
        // log with its reason, which only the tab had before.
        const auto held_not_now = [&not_now, &waited_for](const std::string& why) {
            not_now.why = why;
            GB_INFO("Quest: not now -- %s", not_now.why.c_str());
            waited_for = "Chờ: " + why + ".";       // as QuestRun::wait says it
            return not_now;
        };
        // A key going down as the hold began finishes first, and nothing is
        // left held down.
        for (int i = 0; i < 40 && coord_.held() && !quest_stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        QuestResult stopped;
        stopped.outcome = QuestOutcome::Cancelled;
        if (quest_stop_) return stopped;
        if (coord_.held()) return held_not_now("bot đang bấm phím");
        input_.release_all();
        // The pointer, its buttons and the window in front, either side of the
        // quiet second: what spoiled it, for the log (2026-11-08, the owner's
        // "who is at the mouse?"). Read only.
        const InputGlancer glancer = InputGlancer::real();
        const InputGlance seen_before = glancer.glance();
        const DWORD quiet = last_input();
        for (int slept = 0; slept < kQuietMs && !quest_stop_; slept += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (quest_stop_) return stopped;
        if (last_input() != quiet) {
            // Further off each time, as QuestRun has it (QuietBackOff).
            not_now.touched = true;
            GB_INFO("Quest: the quiet second was not quiet -- %s",
                    quiet_spoiled_text(seen_before, glancer.glance()).c_str());
            return held_not_now("bạn đang dùng chuột hoặc bàn phím");
        }
        not_now.quiet = true;
        // Looked at again with the hold on: something may have begun since.
        QuestGateInput now_in = gates();
        now_in.asked = call.asked;
        const QuestGate again = quest_gate(now_in);
        if (!again.go) return held_not_now(again.why);
        // The game to the front only for a round a person asked for with the
        // tab's button, as the trash tab's buttons bring it. A timed round has
        // waited for the game to be in front, and one that has gone behind
        // since waits again: whoever is in another window keeps their keys.
        if (!input_.window_focused()) {
            if (!call.asked) return held_not_now("game không ở trên cùng");
            input_.focus_window();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (!input_.window_focused()) return held_not_now("không đưa được game lên trước");
        }

        QuestRound round(pictures, settings, hands);
        round.adopt_open_window(call.ours_open);
        {
            std::lock_guard<std::mutex> lock(ui.round_mutex);
            ui.round = &round;
        }
        OnExit forget{ [&ui] {
            std::lock_guard<std::mutex> lock(ui.round_mutex);
            ui.round = nullptr;
        } };
        // A stop that came while all of the above was done found no round to
        // cancel; it is passed on now instead.
        if (quest_stop_) round.cancel();
        QuestResult r = round.run();
        // Only run after its quiet second passed: the back-off from its first
        // step, whatever became of the round (a halt, a stop) -- QuestRun::step.
        r.quiet = true;
        // The board came up during the quiet second: said with what would
        // close it, as the look before the hold says it.
        if (r.board) r.why = board_why(board_closes());
        GB_INFO("Quest: %s -- %s", quest_outcome_name(r.outcome), r.log.c_str());
        if (r.by_change) {
            // Taken for ticked by the change GoBot's click made, no picture
            // fitting it: the frame kept, and the look learned for the rounds
            // after, beside the other pictures cut from the live client.
            const std::string kept = keep_quest_frame(debug_dir, r.ticked_frame, "autotick_on", false);
            if (r.learned_on.valid()) {
                const std::string file = QuestPictures::learned_file();
                const bool saved = QuestPictures::save_learned(file, r.learned_on);
                pictures.learn(r.learned_on, settings);
                GB_INFO("Quest: learned the ticked box%s%s; the frame kept as %s",
                        saved ? ", saved as " : " -- could not save it as ", file.c_str(),
                        kept.empty() ? "(none)" : kept.c_str());
                tell(saved ? "Đã học hình ô Tự Động đã bật (lưu ở templates\\grabbed\\quest_auto_on.png)."
                           : "Đã học hình ô Tự Động đã bật, nhưng không lưu được vào thư mục templates\\grabbed.");
            } else {
                GB_INFO("Quest: the ticked box not learned; the frame kept as %s",
                        kept.empty() ? "(none)" : kept.c_str());
            }
        }
        if (r.failed()) {
            const std::string kept =
                keep_quest_frame(debug_dir, r.last, quest_outcome_name(r.outcome), false);
            if (!kept.empty()) GB_INFO("Quest: kept the screen as %s", kept.c_str());
        }
        if (r.outcome != QuestOutcome::Cancelled) {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            ui.last = "Lần gần nhất lúc " + clock_now() + ": " +
                      quest_result_vi(r, InputManager::key_name(settings.quest_key)) + ".";
        }
        return r;
    };

    QuestRun run;
    run.set_ours_open(ui.ours_open.load());
    QuestRun::Hooks hooks;
    hooks.gates = gates;
    hooks.round = one_round;
    hooks.alert = [this](const std::string& event, const std::string& text) { alert(event.c_str(), text); };
    hooks.say = tell;
    hooks.now = [] { return now_ms(); };
    run.set_hooks(hooks);
    const auto begin_run = [&] {
        run.start(now_ms(), settings);
        tell("Bắt đầu: lần đầu sau " + std::to_string(settings.first_delay_s) + " giây, rồi mỗi " +
             std::to_string(settings.interval_min) + " phút một lần.");
    };
    if (!quest_oneshot_) begin_run();

    // What a probe sees, said, with the frame kept.
    const auto probe = [&] {
        Frame f;
        if (!grab(f)) {
            tell("Chưa chụp được màn hình game.");
            return;
        }
        const QuestProbe p = probe_quest(f, pictures, settings);
        const std::string kept = keep_quest_frame(debug_dir, f, "", true);
        GB_INFO("Quest: probe: %s%s%s", p.log.c_str(), kept.empty() ? "" : " -- kept as ",
                kept.c_str());
        tell(p.vi);
    };

    bool was_crossing = crossing();
    int revived = revive_count_.load();
    for (;;) {
        while (!quest_stop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // A one-off turned into a run: from the start, as a run of its own.
            if (quest_restart_.exchange(false)) begin_run();
            if (now_ms() - read_ms > 1000) {
                QuestSettings fresh;
                if (fresh.load(config)) {
                    settings = fresh;
                    run.update(settings);
                }
                read_ms = now_ms();
            }
            // Back in the world after a crossing, or revived: a round soon --
            // a new session may well have the option off.
            const bool now_crossing = crossing();
            if (was_crossing && !now_crossing) run.soon(now_ms());
            was_crossing = now_crossing;
            const int revives = revive_count_.load();
            if (revives != revived) {
                revived = revives;
                run.soon(now_ms());
            }

            const int request = quest_request_.exchange(0);
            if (request == 2) probe();
            if (request == 1) run.ask_once(now_ms());
            run.step(now_ms());
            ui.ours_open = run.ours_open();
            {
                std::lock_guard<std::mutex> lock(worker_mutex_);
                ui.next_ms = run.next_at();
            }
            if (run.stopped_itself()) {
                GB_WARN("Quest: stopped itself -- %s", run.stopped_log().c_str());
                std::lock_guard<std::mutex> lock(worker_mutex_);
                ui.stopped = run.stopped_why();
                break;
            }
            if (quest_oneshot_ && !run.asked() && quest_request_ == 0) break;
        }
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            ui.next_ms = 0;
        }
        // The way out, under the lock start_quest and request_quest find a
        // worker under: a run, or a button, asked for meanwhile keeps it.
        std::lock_guard<std::mutex> lock(quest_life_mutex_);
        if ((quest_restart_ || quest_request_ != 0) && !quest_stop_ && !run.stopped_itself()) continue;
        quest_running_ = false;
        break;
    }
}

// --------------------------------------------------------------------- tab

void GuiApp::simple_quest_tab() {
    trial_line(kTrialQuest);
    if (!quest_ui_) quest_ui_.reset(new QuestUi);
    QuestUi& ui = *quest_ui_;
    // Kept fresh for a run under way: they may have been changed meanwhile.
    set_own_keys(ui.own_keys, macro_config_, hunt_config_, hotkeys_);
    const std::string cfg = exe_relative("configs/quest.ini");
    if (!ui.loaded) {
        ui.loaded = true;
        // No file is quick quest off, which is what the defaults say.
        if (!ui.settings.load(cfg) && path_exists(cfg))
            simple_note("Không đọc được configs/quest.ini.", 8000);
        ui.interval = ui.settings.interval_min;
    }
    if (!ui.pictures_checked) {
        // Once, when the tab is first shown: a dozen small files.
        ui.pictures_checked = true;
        QuestPictures p;
        ui.pictures_ok = p.load_best(ui.settings);
        ui.pictures_line = ui.pictures_ok
            ? std::string(p.knows_en() ? "Hình nhận diện: đủ (bản tiếng Việt)."
                                       : "Hình nhận diện: đủ (bản tiếng Việt; thiếu hình bản tiếng "
                                         "Anh nên không nhận ra được bản đó).")
            : quest_missing_vi(p);
    }
    const auto put = [&](const char* section, const char* key, const std::string& value) {
        if (!QuestSettings::save_key(cfg, section, key, value))
            simple_note("Không lưu được configs/quest.ini.", 8000);
    };

    std::vector<std::string> lines;
    std::string last, stopped;
    std::uint64_t next = 0;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        lines = ui.log;
        last = ui.last;
        stopped = ui.stopped;
        next = ui.next_ms;
    }

    // ---- on or off ----
    if (!begin_card("##quest")) { end_card(); return; }
    heading(strong_font_, "Làm nhiệm vụ nhanh");
    if (ImGui::Checkbox("Làm nhiệm vụ nhanh", &ui.settings.enabled)) {
        put("Quest", "enabled", ui.settings.enabled ? "true" : "false");
        // Not while BẮT ĐẦU is still opening the game: what is ticked then
        // starts once the character is in.
        if (ui.settings.enabled && simple_running() && !simple_cold_start_ && keys_may_start())
            start_quest();
        if (!ui.settings.enabled && quest_running_ && !quest_oneshot_) stop_quest();
    }
    hint("Tự động nhận và trả nhiệm vụ nhanh như GoC: khi bot chạy, cứ vài phút GoBot mở cửa sổ "
         "Nhiệm Vụ, sang thẻ NV Phụ, bật ô Tự Động nếu nó đang tắt, rồi đóng cửa sổ. Đánh quái, bình "
         "và buff dừng vài giây lúc đó.");

    ImGui::Dummy(ImVec2(0, 4));
    label("Phím mở cửa sổ Nhiệm Vụ");
    ImGui::SameLine();
    std::uint32_t key = ui.settings.quest_key;
    if (simple_key_box("questkey", key, false, false, nullptr)) {
        if (key == VK_RETURN || key == VK_ESCAPE) {
            simple_note("Không dùng Enter hay Esc để mở cửa sổ Nhiệm Vụ: hai phím đó trả lời hộp thoại "
                        "trong game.", 8000);
        } else {
            ui.settings.quest_key = key;
            // By its name, and the digit keys by their code in hex: a lone
            // digit in the file is the code GoBot wrote before it saved names
            // (9 was Tab), so neither may be written as one.
            put("Quest", "quest_key", QuestSettings::key_text(key));
        }
    }
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    hint("Phím mặc định của GoC là U.");
    // The box refuses GoBot's own switches; a hand-edited file may not.
    {
        const std::string clash = quest_key_clash_vi(ui.settings.quest_key, own_keys_of(ui.own_keys));
        if (!clash.empty()) coloured(kBad, ("P" + clash.substr(1) + ".").c_str());
    }

    label("Kiểm tra mỗi");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputInt("##questevery", &ui.interval, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ui.interval = QuestSettings::clamp_interval(ui.interval);
        ui.settings.interval_min = ui.interval;
        put("Quest", "interval_min", std::to_string(ui.interval));
    }
    ImGui::SameLine();
    label("phút (1-60)");
    end_card();

    // ---- now ----
    if (!begin_card("##questnow")) { end_card(); return; }
    heading(strong_font_, "Bây giờ");
    const bool busy = quest_running_ && (quest_oneshot_ || quest_request_ != 0);
    const bool can = !busy && input_.window_valid() && !chat_doubt_ && ui.pictures_ok;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Thử xem (không bấm gì)")) request_quest(true);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Mở cửa sổ Nhiệm Vụ bằng tay ở thẻ NV Phụ rồi bấm: GoBot chỉ nhìn và cho "
                          "biết có thấy cửa sổ, thẻ NV Phụ và ô Tự Động, đang bật hay tắt.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Làm ngay 1 lần") && keys_may_start()) request_quest(false);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Một lần, đúng các bước như khi bot chạy: mở cửa sổ Nhiệm Vụ, sang NV "
                          "Phụ, bật ô Tự Động nếu đang tắt, đóng cửa sổ. Ngay sau khi bấm, GoBot đưa "
                          "game lên trước; nếu phải chờ lâu hơn vài giây, GoBot chờ bạn tự đưa game "
                          "lên.\nĐừng đụng chuột và bàn phím một hai giây sau khi bấm.");
    if (chat_doubt_)
        coloured(kWait, "Ô chat trong game có thể đang mở: chưa làm được cho đến khi ô chat đóng (xem "
                        "thẻ Tự chat).");
    if (input_.window_valid() && overlap_fraction() > 0.02f) {
        coloured(kWait, "Cửa sổ GoBot đang che game: GoBot không bấm chuột vào chỗ bị che.");
        ImGui::SameLine();
        if (ImGui::Button("Dời GoBot sang bên##quest")) move_clear_of_game();
    }

    coloured(ui.pictures_ok ? kDim : kBad, ui.pictures_line.c_str());
    if (!stopped.empty()) coloured(kBad, stopped.c_str());
    if (!last.empty()) coloured(kDim, last.c_str());
    if (quest_running_ && !quest_oneshot_) {
        if (next != 0) {
            const std::uint64_t now = now_ms();
            const std::uint64_t left = next > now ? next - now : 0;
            coloured(kGood, (left >= 60'000
                                 ? "Đang chạy. Lần tới sau " + std::to_string((left + 59'999) / 60'000) +
                                       " phút."
                                 : "Đang chạy. Lần tới sau " + std::to_string((left + 999) / 1000) +
                                       " giây.")
                                .c_str());
        } else {
            coloured(kGood, "Đang chạy.");
        }
    } else if (ui.settings.enabled) {
        coloured(kDim, "Sẽ chạy khi bấm BẮT ĐẦU.");
    } else {
        coloured(kDim, "Đang tắt.");
    }
    hint("Khi một lần làm không được, GoBot giữ lại ảnh màn hình lúc đó trong thư mục quest_debug.");
    last_lines(lines, 4);
    end_card();
}

} // namespace gobot
