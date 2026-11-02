// gobot_test -- the "Load:" line (gobot/load_meter.hpp): how hard GoBot works
// the PC, written to the log once a minute. The report from two snapshots of
// made-up numbers, the counters it is made of -- the timers, the grabs owner
// by owner, the named threads' CPU on a pretend set of threads -- and where
// the window, the readers and the workers feed it, read off their source.
//
// Nothing here looks at the desktop, a window or the registry, and no input
// is sent: the one real call is the high-resolution clock.

#include "gobot/load_meter.hpp"
#include "gobot/logger.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

/// A second of CPU, as Windows counts it: in 100 ns.
constexpr std::uint64_t kCpuSecond = 10'000'000;

void test_load_counters() {
    section("The Load line's counters: timers, grabs owner by owner");

    {
        LoadTimer t;
        t.add_us(5);
        t.add_us(9);
        t.add_us(3);
        const LoadTimer::Sample a = t.take();
        const LoadTimer::Sample b = t.take();
        t.add_us(2);
        const LoadTimer::Sample c = t.take();
        check(a.count == 3 && a.total_us == 17 && a.max_us == 9,
              "a timer counts, adds up and keeps the longest: 3 of them, 17 us, the longest 9");
        check(b.count == 3 && b.total_us == 17 && b.max_us == 0 && c.count == 4 && c.total_us == 19 && c.max_us == 2,
              "and take() starts the longest again, never the counts");
    }
    {
        const std::uint64_t first = load_clock_us();
        const std::uint64_t second = load_clock_us();
        LoadTimer t;
        t.add_since(second);
        const LoadTimer::Sample s = t.take();
        check(second >= first && s.count == 1 && s.total_us < 1'000'000,
              "the high-resolution clock never runs back, and a timing from it counts one");
    }
    {
        GrabOwners owners;
        GrabOwner& vitals = owners.owner("vitals");
        GrabOwner& again = owners.owner("vitals");
        GrabOwner& detector = owners.owner("detector");
        GrabOwner& unnamed = owners.owner(nullptr);
        GrabOwner& empty = owners.owner("");
        GrabOwner& other = owners.owner("other");
        check(&vitals == &again && &vitals != &detector && &unnamed == &empty && &empty == &other &&
                  &other != &vitals,
              "one owner per label, the same each time asked; no label, or an empty one, is \"other\"");

        vitals.grabs.fetch_add(3);
        vitals.bitblt.add_us(5000);
        vitals.bitblt.add_us(9000);
        detector.print_window.add_us(40'000);
        detector.recopies.fetch_add(2);
        detector.points.fetch_add(150);
        const std::vector<GrabCounts> t = owners.take();
        const std::vector<GrabCounts> t2 = owners.take();
        check(t.size() == 3 && t[0].owner == "vitals" && t[1].owner == "detector" && t[2].owner == "other",
              "taken in the order the owners were first asked for");
        check(t.size() == 3 && t[0].grabs == 3 && t[0].bitblt.count == 2 && t[0].bitblt.total_us == 14'000 &&
                  t[0].bitblt.max_us == 9000 && t[1].print_window.count == 1 && t[1].print_window.max_us == 40'000 &&
                  t[1].recopies == 2 && t[1].points == 150,
              "each with its grabs, BitBlt and PrintWindow timed, recopies and points looked up");
        check(t2.size() == 3 && t2[0].bitblt.max_us == 0 && t2[0].bitblt.count == 2 && t2[0].grabs == 3,
              "and the next take() has the longest started again, the counts kept");
    }
    {
        // More labels than places: the rest go with "other", in the last.
        GrabOwners owners;
        std::vector<GrabOwner*> got;
        for (int i = 0; i < GrabOwners::kMaxOwners + 6; ++i) {
            const std::string name = "job " + std::to_string(i);
            got.push_back(&owners.owner(name.c_str()));
        }
        GrabOwner& other = owners.owner("other");
        const std::vector<GrabCounts> t = owners.take();
        const int last = GrabOwners::kMaxOwners - 1;
        check(t.size() == static_cast<std::size_t>(GrabOwners::kMaxOwners) && t[static_cast<std::size_t>(last)].owner == "other" &&
                  got[static_cast<std::size_t>(last)] == &other && got.back() == &other &&
                  got[static_cast<std::size_t>(last - 1)] != &other && t[static_cast<std::size_t>(last - 1)].owner == "job 22",
              "past " + std::to_string(GrabOwners::kMaxOwners - 1) + " labels, the rest are counted as \"other\", "
              "which keeps the last place");
    }
    {
        Logger& log = Logger::instance();
        const std::uint64_t lines = log.lines_written();
        const std::uint64_t bytes = log.bytes_written();
        const std::string said = "load_test: a line the Load line counts";
        log.write(log.level(), said);
        const std::uint64_t lines_after = log.lines_written();
        const std::uint64_t bytes_after = log.bytes_written();
        check(lines_after >= lines + 1 && bytes_after >= bytes + said.size() + 1,
              "the log counts its lines and their bytes, for \"log N lines/min\"");
    }
}

/// Threads of the test's own making for a ThreadLedger: each handle is a
/// number, with the CPU it has used and whether it still runs.
struct PretendThreads {
    std::map<std::uintptr_t, std::uint64_t> cpu;
    std::map<std::uintptr_t, bool>          alive;
    std::vector<std::uintptr_t>             closed;

    static void* handle(std::uintptr_t n) { return reinterpret_cast<void*>(n); }
    static std::uintptr_t number(void* h) { return reinterpret_cast<std::uintptr_t>(h); }
    ThreadLedger::Probe probe() {
        return ThreadLedger::Probe{ [this](void* h) { return cpu[number(h)]; },
                                    [this](void* h) { return alive[number(h)]; },
                                    [this](void* h) { closed.push_back(number(h)); } };
    }
    bool was_closed(std::uintptr_t n) const {
        for (const std::uintptr_t c : closed)
            if (c == n) return true;
        return false;
    }
};

const ThreadCpu* named(const std::vector<ThreadCpu>& all, const std::string& name) {
    for (const ThreadCpu& t : all)
        if (t.name == name) return &t;
    return nullptr;
}

void test_thread_ledger() {
    section("The Load line's threads: CPU under each name, ended threads and renames (pretend threads)");

    PretendThreads pretend;
    ThreadLedger ledger(pretend.probe());

    // A worker named with 100 used already: only what it uses after counts.
    pretend.alive[1] = true;
    pretend.cpu[1] = 100;
    ledger.named("vitals", 11, PretendThreads::handle(1));
    pretend.cpu[1] = 600;
    {
        const std::vector<ThreadCpu> t = ledger.totals();
        const ThreadCpu* v = named(t, "vitals");
        check(v != nullptr && v->cpu_100ns == 500 && v->alive == 1,
              "a named thread's CPU from when it was named, and it runs");
    }

    // One that ends: dropped, its CPU kept under its name.
    pretend.alive[2] = true;
    ledger.named("trash", 12, PretendThreads::handle(2));
    pretend.cpu[2] = 350;
    pretend.alive[2] = false;
    {
        const std::vector<ThreadCpu> t = ledger.totals();
        const std::vector<ThreadCpu> again = ledger.totals();
        const ThreadCpu* gone = named(t, "trash");
        const ThreadCpu* still = named(again, "trash");
        check(gone != nullptr && gone->cpu_100ns == 350 && gone->alive == 0 && still != nullptr &&
                  still->cpu_100ns == 350 && ledger.tracked() == 1 && pretend.was_closed(2),
              "a thread that has ended is dropped and its handle closed, its CPU kept under its name for good");
    }

    // The same thread named again: what it used stays with the old name.
    pretend.alive[3] = true;
    ledger.named("popups", 13, PretendThreads::handle(3));
    pretend.cpu[3] = 200;
    pretend.alive[4] = true;
    ledger.named("probe", 13, PretendThreads::handle(4));
    pretend.cpu[3] = 260;
    {
        const std::vector<ThreadCpu> t = ledger.totals();
        const ThreadCpu* before = named(t, "popups");
        const ThreadCpu* after = named(t, "probe");
        check(before != nullptr && before->cpu_100ns == 200 && before->alive == 0 && after != nullptr &&
                  after->cpu_100ns == 60 && after->alive == 1 && pretend.was_closed(4) && !pretend.was_closed(3),
              "a thread named again: its CPU so far under the old name, from then on under the new, one handle kept");
    }

    // A thread id used again by a new thread once the old one has ended.
    pretend.cpu[1] = 700;
    pretend.alive[1] = false;
    pretend.alive[5] = true;
    ledger.named("chat", 11, PretendThreads::handle(5));
    pretend.cpu[5] = 40;
    {
        const std::vector<ThreadCpu> t = ledger.totals();
        const ThreadCpu* old = named(t, "vitals");
        const ThreadCpu* now = named(t, "chat");
        check(old != nullptr && old->cpu_100ns == 600 && old->alive == 0 && now != nullptr && now->cpu_100ns == 40 &&
                  now->alive == 1,
              "a new thread with an ended one's id is a thread of its own");
        std::string order;
        for (const ThreadCpu& c : t) order += c.name + ",";
        check(order == "vitals,trash,popups,probe,chat,", "names in the order they were first given -- " + order);
    }

    // Workers started and ended by the dozen with no snapshot taken: what is
    // tracked stays bounded, and nothing they used is lost.
    for (std::uintptr_t i = 0; i < 70; ++i) {
        const std::uintptr_t h = 1000 + i;
        pretend.alive[h] = true;
        ledger.named("burst", static_cast<std::uint32_t>(h), PretendThreads::handle(h));
        pretend.cpu[h] = 10;
        pretend.alive[h] = false;
    }
    {
        const std::size_t tracked = ledger.tracked();
        const std::vector<ThreadCpu> t = ledger.totals();
        const ThreadCpu* burst = named(t, "burst");
        check(tracked <= ThreadLedger::kPruneAt && burst != nullptr && burst->cpu_100ns == 700 && burst->alive == 0,
              "seventy short workers and no snapshot: at most " + std::to_string(ThreadLedger::kPruneAt) +
                  " tracked, and all 70 x 10 of their CPU counted");
    }
}

/// Two snapshots a minute apart, every number made up.
void minute_apart(LoadSnapshot& before, LoadSnapshot& now) {
    before = LoadSnapshot{};
    now = LoadSnapshot{};
    before.at_ms = 1'000'000;
    now.at_ms = 1'060'000;
    before.logical_cpus = now.logical_cpus = 2;
    before.process.cpu_100ns = 100 * kCpuSecond;
    now.process.cpu_100ns = 130 * kCpuSecond;       // 30 s of CPU in 60 s on 2 CPUs: 25%

    before.threads = { { "gui", 10 * kCpuSecond, 1 }, { "vitals", 2 * kCpuSecond, 1 }, { "trash", 5 * kCpuSecond, 0 } };
    now.threads = { { "gui", 16 * kCpuSecond, 1 },
                    { "vitals", 5 * kCpuSecond, 1 },
                    { "trash", 5 * kCpuSecond, 0 },          // ended before, nothing since
                    { "detector", 4 * kCpuSecond, 1 } };     // named since

    before.gui = now.gui = true;
    before.frames = 1000;
    now.frames = 1300;
    before.build_ms = 2000.0;
    now.build_ms = 2600.0;
    now.build_max_ms = 18.0;
    before.present_ms = 500.0;
    now.present_ms = 800.0;
    now.present_max_ms = 40.0;
    before.passes = 10'000;
    now.passes = 13'840;
    now.pass_gap_max_ms = 31.4;
    before.passes_late = 3;
    now.passes_late = 5;
    before.occluded_skips = 5;
    now.occluded_skips = 7;

    GrabCounts vitals_was;
    vitals_was.owner = "vitals";
    vitals_was.grabs = 1000;
    vitals_was.bitblt = { 1000, 7'000'000, 0 };
    vitals_was.print_window = { 5, 100'000, 0 };
    vitals_was.recopies = 1;
    vitals_was.points = 100;
    GrabCounts detector_was;
    detector_was.owner = "detector";
    detector_was.grabs = 500;
    detector_was.bitblt = { 500, 2'500'000, 0 };
    before.grabs = { vitals_was, detector_was };

    GrabCounts vitals = vitals_was;
    vitals.grabs = 1400;
    vitals.bitblt = { 1400, 9'800'000, 61'000 };
    vitals.print_window = { 15, 480'000, 140'000 };
    vitals.recopies = 5;
    vitals.points = 1300;
    GrabCounts detector = detector_was;
    detector.grabs = 920;
    detector.bitblt = { 920, 4'600'000, 30'000 };
    GrabCounts shop;
    shop.owner = "shop";
    shop.grabs = 6;
    shop.bitblt = { 6, 60'000, 12'000 };
    now.grabs = { vitals, detector, shop };

    before.vitals_running = now.vitals_running = true;
    before.vitals_reads = 1000;
    now.vitals_reads = 300;                  // the reader started again meanwhile
    now.vitals_period_ms = 150;
    before.anchor_searches = 10;
    now.anchor_searches = 12;
    before.detector_running = now.detector_running = true;
    before.detector_frames = 100;
    now.detector_frames = 520;
    before.detect = { 100, 300'000, 0 };
    now.detect = { 520, 1'560'000, 12'000 };
    before.capture_running = now.capture_running = true;
    before.packets = 1000;
    now.packets = 3460;
    before.log_lines = 100;
    now.log_lines = 134;
    now.log_bytes = 21 * 1024 + 500;

    now.process.working_set = 512ull * 1024 * 1024;
    now.process.private_bytes = 470ull * 1024 * 1024;
    before.process.page_faults = 1000;
    now.process.page_faults = 7000;
    now.process.gdi_objects = 96;
    now.process.user_objects = 40;
    now.process.handles = 610;
    now.front = { 2, 50'000, 30'000 };

    now.jobs = "hunt+macro";
    now.tab = "Đánh quái";
    now.light = true;
}

void test_load_report() {
    section("The Load line from two snapshots (made-up numbers)");

    LoadSnapshot before, now;
    minute_apart(before, now);
    const std::string r = load_report(before, now);
    const std::string shown = "\n" + r;

    check(r.rfind("Load: last 60 s | cpu 25% of 2 CPUs | threads, CPU s/min: gui 6.0, vitals 3.0, detector 4.0, "
                  "other 17.0",
                  0) == 0,
          "the process's CPU over the minute and its CPUs; each named thread's CPU seconds a minute, a name that "
          "did nothing left out, and \"other\" the rest of the process's" + shown);
    check(!has(r, "trash"), "a name none of whose threads ran meanwhile is not shown");
    check(has(r, "\n      gui 5.0 frames/s (build avg 2.0 ms max 18, present avg 1.0 ms max 40), passes 64.0/s "
                 "(longest gap 31 ms, 2 over 32 ms), occluded skips 2"),
          "the window: frames a second, build and present per frame and the longest, passes (ticks) a second with "
          "the longest gap and how many came late, occluded skips" + shown);
    check(has(r, "\n      grabs 13.8/s (vitals 6.7, detector 7.0, shop 0.1); BitBlt avg 6.0 ms max 61; "
                 "PrintWindow 10/min (vitals 10) avg 38.0 ms max 140, recopies 4 (vitals 4), window checks 1200/min"),
          "grabs owner by owner, a new owner counted from nothing; BitBlt and PrintWindow timed, PrintWindow and "
          "recopies by owner, WindowFromPoint looks" + shown);
    check(has(r, "\n      reads: vitals 5.0/s (every 150 ms), anchor searches 2, detector 7.0/s (look avg 3.0 ms max "
                 "12) | packets 41.0/s, dropped 0 | log 34 lines/min, 21 KB"),
          "the readers -- a count that went down taken as started again -- the packets and the log" + shown);
    check(has(r, "\n      mem: working set 512 MB, private 470 MB, page faults 100.0/s, gdi 96, user 40, handles 610 "
                 "| front keeper 2.0 tries/min, GoBot's thread held 50 ms (max 30)"),
          "memory, page faults, GDI, USER and handles; the front keeper's tries and how long they held GoBot" + shown);
    check(has(r, "\n      jobs: hunt+macro, tab Đánh quái, light on"), "what runs, the tab open, light mode" + shown);

    // Nothing running, no window, as GoBot closes.
    LoadSnapshot quiet_before, quiet;
    quiet_before.at_ms = 5000;
    quiet.at_ms = 35'000;
    quiet.process.cpu_100ns = kCpuSecond / 2;
    const std::string q = load_report(quiet_before, quiet, "Load at exit");
    check(q.rfind("Load at exit: last 30 s | cpu 2% of 1 CPU | threads, CPU s/min: other 1.0", 0) == 0 &&
              !has(q, "\n      gui ") && has(q, "\n      grabs 0.0/s; BitBlt none; PrintWindow 0.0/min, recopies 0, ") &&
              has(q, "reads: vitals off, detector off | packets off | log 0.0 lines/min, 0 KB") &&
              has(q, "\n      jobs: none, light off"),
          "with nothing running: the readers and the capture off, no grabs, no window line, jobs none, under its "
          "own title\n" + q);
    check(load_report(quiet, quiet).find("no time") != std::string::npos,
          "two snapshots of the same moment say so rather than divide by nothing");
}

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
#endif

void test_load_wiring() {
    section("The Load line wired in: the workers named, the captures labelled, the line from tick() (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    if (gui.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto npos = std::string::npos;

    // Each worker loop names its thread first of all.
    const struct { const char* file; const char* head; const char* name; } kLoops[] = {
        { "/apps/gobot_gui/gui_app.cpp", "void GuiApp::initialise() {", "gui" },
        { "/src/vitals_reader.cpp", "void VitalsReader::loop() {", "vitals" },
        { "/src/mob_detector.cpp", "void MobDetector::loop() {", "detector" },
        { "/src/hunter.cpp", "void Hunter::loop() {", "hunter" },
        { "/src/macro.cpp", "void MacroEngine::timer_loop() {", "macro timer" },
        { "/src/macro.cpp", "void MacroEngine::sequence_loop_fn() {", "macro sequence" },
        { "/apps/gobot_gui/simple_popups.cpp", "void GuiApp::popup_loop() {", "popups" },
        { "/apps/gobot_gui/gui_app.cpp", "void GuiApp::revive_loop() {", "revive" },
        { "/apps/gobot_gui/gui_app.cpp", "void GuiApp::party_loop() {", "party" },
        { "/apps/gobot_gui/gui_app.cpp", "void GuiApp::trash_loop() {", "trash" },
        { "/apps/gobot_gui/gui_app.cpp", "void GuiApp::chat_loop() {", "chat" },
        { "/apps/gobot_gui/simple_quest.cpp", "void GuiApp::quest_loop() {", "quest" },
        { "/apps/gobot_gui/gui_app.cpp", "void GuiApp::keeper_loop() {", "keeper" },
        { "/src/packet_capture.cpp", "void PacketCapture::capture_loop() {", "capture" },
        { "/src/packet_capture.cpp", "void PacketCapture::watchdog_loop() {", "capture watchdog" },
        { "/src/command_executor.cpp", "void CommandExecutor::execution_loop() {", "executor" },
        { "/src/command_executor.cpp", "void CommandExecutor::pathfinding_loop() {", "executor path" },
        { "/src/alerts.cpp", "void AlertService::run() {", "alerts" },
    };
    std::string unnamed;
    for (const auto& l : kLoops) {
        const std::string text = source(l.file);
        const std::size_t head = text.find(l.head);
        const std::string call = std::string("name_this_thread(\"") + l.name + "\");";
        const std::size_t at = head == npos ? npos : text.find(call, head);
        const std::size_t first_end = head == npos ? npos : text.find(';', head);
        if (at == npos || first_end != at + call.size() - 1) unnamed += std::string(" ") + l.head;
    }
    check(unnamed.empty(), "every worker loop names its thread on its first line" +
                               (unnamed.empty() ? std::string() : " -- not:" + unnamed));

    // The long-lived captures and the jobs' own carry their owner's label.
    const struct { const char* file; const char* text; } kLabels[] = {
        { "/src/vitals_reader.cpp", "capture.set_label(\"vitals\");" },
        { "/src/mob_detector.cpp", "capture.set_label(\"detector\");" },
        { "/apps/gobot_gui/simple_popups.cpp", "capture.set_label(\"popups\");" },
        { "/apps/gobot_gui/gui_app.cpp", "capture.set_label(\"revive\");" },
        { "/apps/gobot_gui/gui_app.cpp", "capture.set_label(\"party\");" },
        { "/apps/gobot_gui/gui_app.cpp", "capture.set_label(\"trash\");" },
        { "/apps/gobot_gui/gui_app.cpp", "shop_capture.set_label(\"shop\");" },
        { "/apps/gobot_gui/gui_app.cpp", "pass_capture.set_label(\"bag\");" },
        { "/apps/gobot_gui/gui_app.cpp", "capture.set_label(\"chat\");" },
        { "/apps/gobot_gui/simple_quest.cpp", "capture.set_label(\"quest\");" },
        { "/apps/gobot_gui/simple_keeper.cpp", "capture.set_label(\"keeper\");" },
        { "/apps/gobot_gui/simple_view.cpp", "capture.set_label(\"login look\");" },
        { "/src/stall_watch.cpp", "capture_.set_label(\"stall look\");" },
        { "/src/auto_login.cpp", "c.set_label(\"auto login\");" },
    };
    std::string unlabelled;
    for (const auto& l : kLabels)
        if (!has(source(l.file), l.text)) unlabelled += std::string(" ") + l.file + ": " + l.text;
    check(unlabelled.empty(), "the captures of the vitals, the detector, the popups, revive, party, trash, the shop, "
                              "the bag, chat, quest, the keeper, the login look, the stall look and auto login are "
                              "labelled" + (unlabelled.empty() ? std::string() : " -- not:" + unlabelled));

    // grab() counts around its pinned lines, which stay as they were.
    const std::string cap = source("/src/screen_capture.cpp");
    const std::size_t asked = cap.find("ok = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);");
    const std::size_t blit = cap.find("const BOOL copied = BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);");
    const std::size_t print_timed = asked == npos ? npos : cap.find("counted.print_window.add_since(print_from);", asked);
    const std::size_t blit_timed = blit == npos ? npos : cap.find("counted.bitblt.add_since(copy_from);", blit);
    check(has(cap, "if (owner_ == nullptr) owner_ = &grab_owners().owner(label_);") && print_timed != npos &&
              print_timed - asked < 120 && blit_timed != npos && blit_timed - blit < 120 &&
              has(cap, "counted.recopies.fetch_add(1, std::memory_order_relaxed);") &&
              has(cap, "counted.grabs.fetch_add(1, std::memory_order_relaxed);") &&
              has(cap, "if (!screen_only_ && !PrintWindowPause::active() && print_rule_.ask()) {"),
          "screen_capture.cpp: each grab counted under its label, PrintWindow and BitBlt timed, recopies counted, "
          "the pinned lines as they were");

    // The line itself: from the end of tick(), no snapshot of the system's
    // threads, and the front keeper timed around its one try.
    const std::size_t tick = gui.find("void GuiApp::tick() {");
    const std::size_t framing = tick == npos ? npos : gui.find("check_framing_health();", tick);
    const std::size_t line = framing == npos ? npos : gui.find("load_tick();", framing);
    const std::size_t end = tick == npos ? npos : gui.find("\n}", tick);
    check(framing != npos && line != npos && end != npos && framing < line && line < end &&
              gui.find_first_not_of(" \t\r\n", line + std::string("load_tick();").size()) == end + 1,
          "gui_app.cpp: tick() ends with load_tick(), after check_framing_health()");
    const std::string meter = source("/src/load_meter.cpp");
    check(!meter.empty() && meter.find("CreateToolhelp32Snapshot") == npos && meter.find("TH32CS_SNAP") == npos &&
              has(meter, "GetThreadTimes(") && has(meter, "GetProcessTimes(") &&
              has(meter, "OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)") &&
              has(meter, "GetProcAddress(kernel, \"SetThreadDescription\")"),
          "load_meter.cpp: each named thread's CPU by its own handle, \"other\" from the process's, no snapshot of "
          "every thread on the PC; SetThreadDescription looked up by name");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::size_t focus = view.find("input_.focus_window(InputManager::kFocusMinMs, &accepted);");
    const std::size_t from = view.rfind("const std::uint64_t focus_from = load_clock_us();", focus);
    const std::size_t timed = focus == npos ? npos : view.find("load_meters().front_focus.add_since(focus_from);", focus);
    check(focus != npos && from != npos && focus - from < 120 && timed != npos && timed - focus < 120,
          "simple_view.cpp: the front keeper's try timed, for its tries a minute and how long it held GoBot");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_load(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_load_counters();
    test_thread_ledger();
    test_load_report();
    test_load_wiring();
}
