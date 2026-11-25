// gobot_test -- the "Load:" line (gobot/load_meter.hpp): how hard GoBot works
// the PC, written to the log once a minute. The report from two snapshots of
// made-up numbers, the counters it is made of -- the timers, the grabs owner
// by owner, the named threads' CPU on a pretend set of threads -- and where
// the window, the readers and the workers feed it, read off their source.
// And the template matcher's kept buffers (2026-11-16), which save work and
// page faults on every look: held to the answers of buffers made afresh, to
// the bit, on made-up frames. And the game's pictures loaded once for the
// whole program (2026-11-16): the same pictures under the same names as a
// job's own load, short names that point rather than copy, and the GUI's
// jobs asking for that one set.
//
// Nothing here looks at the desktop, a window or the registry, and no input
// is sent: the real calls are the high-resolution clock, and the pictures
// beside the tests and a name map in the temp folder read from disk.

#include "gobot/config.hpp"
#include "gobot/load_meter.hpp"
#include "gobot/logger.hpp"
#include "gobot/template_matcher.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>
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
    const std::size_t framing = tick == npos ? npos : gui.find("say_traffic_lost();", tick);
    const std::size_t line = framing == npos ? npos : gui.find("load_tick();", framing);
    const std::size_t end = tick == npos ? npos : gui.find("\n}", tick);
    check(framing != npos && line != npos && end != npos && framing < line && line < end &&
              gui.find_first_not_of(" \t\r\n", line + std::string("load_tick();").size()) == end + 1,
          "gui_app.cpp: tick() ends with load_tick(), after say_traffic_lost()");
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

// ------------------------------------------------ the matcher's kept buffers

Frame blank_frame(int w, int h) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    return f;
}

void paint(Frame& f, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    if (x < 0 || y < 0 || x >= f.width || y >= f.height) return;
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(f.width) + static_cast<std::size_t>(x)) * 4;
    f.pixels[i] = b;
    f.pixels[i + 1] = g;
    f.pixels[i + 2] = r;
    f.pixels[i + 3] = a;
}

std::uint8_t alpha_at(const Frame& f, int x, int y) {
    return f.pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(f.width) + static_cast<std::size_t>(x)) * 4 + 3];
}

Frame noise_frame(int w, int h, unsigned seed) {
    Frame f = blank_frame(w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            paint(f, x, y, static_cast<std::uint8_t>(rng() % 90), static_cast<std::uint8_t>(rng() % 90),
                  static_cast<std::uint8_t>(rng() % 90));
    return f;
}

/// A `w` x `h` piece of `f` from (x, y), opaque.
Frame cut(const Frame& f, int x, int y, int w, int h) {
    Frame t = blank_frame(w, h);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            std::uint8_t r, g, b;
            f.get(x + i, y + j, r, g, b);
            paint(t, i, j, r, g, b);
        }
    return t;
}

/// `t`'s opaque pixels drawn onto `f` at (x, y).
void stamp(Frame& f, const Frame& t, int x, int y) {
    for (int j = 0; j < t.height; ++j)
        for (int i = 0; i < t.width; ++i) {
            if (alpha_at(t, i, j) < TemplateMatcher::kOpaque) continue;
            std::uint8_t r, g, b;
            t.get(i, j, r, g, b);
            paint(f, x + i, y + j, r, g, b);
        }
}

/// A close button: a ring with a cross in it, its corners see-through; with
/// `ring_only`, only the ring is drawn, the rest see-through too.
Frame close_button(int size, bool ring_only) {
    Frame t = blank_frame(size, size);
    const int c = size / 2;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int dx = x - c, dy = y - c;
            const int d2 = dx * dx + dy * dy;
            if (d2 >= c * c) continue;
            const bool rim = d2 >= (c - 2) * (c - 2);
            const bool cross = (dx == dy || dx == -dy) && d2 < (c - 3) * (c - 3);
            if (ring_only && !rim) continue;
            const int v = rim ? 200 : cross ? 240 : 40;
            paint(t, x, y, static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v - 20),
                  static_cast<std::uint8_t>(v / 2));
        }
    return t;
}

/// A square icon of one hue with a dark rim: the same drawing in several
/// hues, as the crest tiers are.
Frame icon(int size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Frame t = blank_frame(size, size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const bool rim = x < 3 || y < 3 || x >= size - 3 || y >= size - 3;
            if (rim) paint(t, x, y, 18, 18, 22);
            else     paint(t, x, y, r, g, b);
        }
    return t;
}

/// Empty when the two answers are the same to the bit; what differs otherwise.
std::string differ(const std::vector<TemplateMatcher::Match>& fresh,
                   const std::vector<TemplateMatcher::Match>& kept) {
    if (fresh.size() != kept.size())
        return std::to_string(fresh.size()) + " found fresh, " + std::to_string(kept.size()) + " kept";
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        const TemplateMatcher::Match& a = fresh[i];
        const TemplateMatcher::Match& b = kept[i];
        if (a.found != b.found || a.at.x != b.at.x || a.at.y != b.at.y || a.at.w != b.at.w || a.at.h != b.at.h ||
            std::memcmp(&a.score, &b.score, sizeof(float)) != 0)
            return "match " + std::to_string(i) + ": fresh " + std::to_string(a.at.x) + "," + std::to_string(a.at.y) +
                   " " + std::to_string(a.score) + ", kept " + std::to_string(b.at.x) + "," + std::to_string(b.at.y) +
                   " " + std::to_string(b.score);
    }
    return {};
}

struct MatchCase {
    const Frame*                        frame = nullptr;
    const Frame*                        tpl = nullptr;
    ScreenRect                          area;
    TemplateMatcher::Tone               tone = TemplateMatcher::Tone::Grey;
    float                               min_score = 0.8f;
    std::size_t                         limit = 64;
    std::string                         name;
    std::vector<TemplateMatcher::Match> fresh;
};

/// Every case searched with the kept buffers, in order or backwards, against
/// its fresh answer; the first few that differ, in words.
std::string search_kept(const std::vector<MatchCase>& cases, bool backwards) {
    std::string bad;
    int shown = 0;
    for (std::size_t n = 0; n < cases.size(); ++n) {
        const MatchCase& c = cases[backwards ? cases.size() - 1 - n : n];
        const std::string d =
            differ(c.fresh, TemplateMatcher::find_all(*c.frame, c.area, *c.tpl, c.min_score, c.limit, c.tone));
        if (!d.empty() && shown++ < 3) bad += "\n  " + c.name + ": " + d;
    }
    return bad;
}

void test_matcher_kept_buffers() {
    section("Template matching with kept buffers and pictures made ready once: the answers of fresh ones, to the bit");

    if (!TemplateMatcher::uses_opencv()) {
        skip("built without OpenCV, which keeps nothing between searches\n");
        return;
    }

    // Two made-up screens. The first has what the searches below exercise:
    // three copies of one close button (equal scores, so the order among
    // ties counts), two icons of one drawing in two hues, a flat patch where
    // the masked correlation blows up, and stripes that repeat.
    const Frame button = close_button(18, false);
    const Frame ring = close_button(22, true);
    const Frame red = icon(28, 210, 40, 40);
    const Frame blue = icon(28, 50, 70, 220);

    Frame a = noise_frame(400, 260, 11);
    for (int y = 10; y < 90; ++y)
        for (int x = 250; x < 390; ++x) paint(a, x, y, 36, 44, 72);
    for (int y = 160; y < 190; ++y)
        for (int x = 10; x < 140; ++x) {
            const auto v = static_cast<std::uint8_t>(40 + (x % 7) * 25);
            paint(a, x, y, v, v, static_cast<std::uint8_t>(255 - v));
        }
    stamp(a, button, 40, 30);
    stamp(a, button, 190, 30);
    stamp(a, button, 41, 200);
    stamp(a, ring, 300, 40);
    stamp(a, red, 110, 120);
    stamp(a, blue, 160, 120);

    Frame b = noise_frame(400, 260, 22);
    stamp(b, button, 300, 150);
    stamp(b, red, 20, 20);

    const Frame noise_piece = cut(a, 61, 97, 24, 20);
    const Frame stripe_piece = cut(a, 14, 165, 16, 16);
    const Frame big_piece = cut(a, 220, 140, 70, 45);

    const struct { const Frame* tpl; const char* name; } kPictures[] = {
        { &button, "close button" }, { &ring, "ring" },          { &red, "red icon" },     { &blue, "blue icon" },
        { &noise_piece, "piece" },   { &stripe_piece, "stripes" }, { &big_piece, "big piece" },
    };
    const struct { ScreenRect area; const char* name; } kAreas[] = {
        { ScreenRect{ 0, 0, 0, 0 }, "whole" },
        { ScreenRect{ 13, 7, 331, 233 }, "odd part" },
        { ScreenRect{ -5, -9, 190, 150 }, "over the top left" },
        { ScreenRect{ 250, 120, 400, 400 }, "past the bottom right" },
    };
    const float kScores[] = { 0.5f, 0.8f, 0.95f };
    const std::size_t kLimits[] = { 64, 1, 4 };

    std::vector<MatchCase> cases;
    std::size_t found = 0;
    int i = 0;
    for (const Frame* frame : { &a, &b })
        for (const auto& p : kPictures)
            for (const auto& area : kAreas)
                for (const TemplateMatcher::Tone tone : { TemplateMatcher::Tone::Grey, TemplateMatcher::Tone::Colour }) {
                    MatchCase c;
                    c.frame = frame;
                    c.tpl = p.tpl;
                    c.area = area.area;
                    c.tone = tone;
                    c.min_score = kScores[i % 3];
                    c.limit = kLimits[(i / 3) % 3];
                    c.name = std::string(frame == &a ? "screen A, " : "screen B, ") + p.name + ", " + area.name +
                             (tone == TemplateMatcher::Tone::Colour ? ", colour" : ", grey");
                    c.fresh = TemplateMatcher::find_all_fresh(*c.frame, c.area, *c.tpl, c.min_score, c.limit, c.tone);
                    found += c.fresh.size();
                    cases.push_back(std::move(c));
                    ++i;
                }

    // Not an empty comparison: the fresh searches do find things, the three
    // buttons among them.
    const std::vector<TemplateMatcher::Match> buttons =
        TemplateMatcher::find_all_fresh(a, ScreenRect{}, button, 0.9f, 64, TemplateMatcher::Tone::Grey);
    check(found >= 20 && buttons.size() == 3,
          "the fresh searches find things to compare: " + std::to_string(found) + " in " +
              std::to_string(cases.size()) + " searches, the three buttons " + std::to_string(buttons.size()));

    std::string bad = search_kept(cases, false);
    check(bad.empty(), "every search with the kept buffers gives the fresh answer, to the bit" + bad);
    bad = search_kept(cases, true);
    check(bad.empty(), "and again backwards, each search on buffers left by searches of other sizes" + bad);

    // Under one OneFrame per screen, one inside the other: each search cuts
    // its part from the whole screen converted once.
    {
        TemplateMatcher::OneFrame one_a(a);
        TemplateMatcher::OneFrame one_b(b);
        bad = search_kept(cases, false);
    }
    check(bad.empty(), "under a OneFrame for each screen, one inside the other, the same answers" + bad);

    // Two threads at once, which share the pool of buffers and the pictures
    // made ready.
    {
        std::string first, second;
        std::thread one([&] { first = search_kept(cases, false); });
        std::thread two([&] { second = search_kept(cases, true); });
        one.join();
        two.join();
        check(first.empty() && second.empty(),
              "two threads searching at once get the same answers" + first + second);
    }

    // A picture written over in place, where its pixels were: made ready
    // again, not matched with what was there before.
    {
        Frame picture = cut(a, 61, 97, 24, 20);
        const std::uint8_t* place = picture.pixels.data();
        const TemplateMatcher::Match before = TemplateMatcher::find(a, ScreenRect{}, picture, 0.9f);
        const Frame other = cut(a, 300, 200, 24, 20);
        std::memcpy(picture.pixels.data(), other.pixels.data(), other.pixels.size());
        const TemplateMatcher::Match after = TemplateMatcher::find(a, ScreenRect{}, picture, 0.9f);
        // And see-through corners on it now, which give it a mask.
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                paint(picture, x, y, 0, 0, 0, 0);
                paint(picture, 23 - x, 19 - y, 0, 0, 0, 0);
            }
        const std::vector<TemplateMatcher::Match> masked =
            TemplateMatcher::find_all(a, ScreenRect{}, picture, 0.8f, 8, TemplateMatcher::Tone::Colour);
        const std::vector<TemplateMatcher::Match> masked_fresh =
            TemplateMatcher::find_all_fresh(a, ScreenRect{}, picture, 0.8f, 8, TemplateMatcher::Tone::Colour);
        check(picture.pixels.data() == place && before.found && before.at.x == 61 && before.at.y == 97 &&
                  after.found && after.at.x == 300 && after.at.y == 200 && differ(masked_fresh, masked).empty() &&
                  !masked.empty() && masked.front().at.x == 300 && masked.front().at.y == 200,
              "a picture written over where it lies is made ready again: found at its new place, its new mask "
              "used -- " + differ(masked_fresh, masked));
    }
}

// ------------------------------------------------- the one set of pictures

bool same_picture(const Frame* a, const Frame* b) {
    return a != nullptr && b != nullptr && a->width == b->width && a->height == b->height && a->pixels == b->pixels;
}

/// A file in the PC's temp folder, written whole; its path, or empty.
std::string temp_ini(const char* name, const std::string& text) {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    const std::string path = (dir / name).string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return out ? path : std::string();
}

void test_store_short_names() {
    section("The pictures' short names point at the picture, no copy, and act as the copies did (2026-11-16)");
    Frame a = blank_frame(6, 4), b = blank_frame(5, 5), shadowed = blank_frame(3, 3);
    paint(a, 1, 1, 200, 10, 10);
    paint(b, 2, 2, 10, 200, 10);
    paint(shadowed, 0, 0, 10, 10, 200);
    TemplateStore store;
    store.put("long/a", a);
    store.put("long/b", b);
    store.put("short_b", shadowed);     // a picture of the same name as a short name
    // The keys are applied in name order (IniFile keeps them sorted), as the
    // copies were: "tail" comes after the "short_a" it names.
    const std::string ini = temp_ini("gobot_test_short_names.ini",
                                     "[Templates]\nshort_a = long/a\nshort_b = long/b\ntail = short_a\n"
                                     "nothing = long/missing\n");
    if (ini.empty()) {
        skip("no temp folder to write a name map in\n");
        return;
    }
    const int named = store.apply_names(ini);
    std::error_code ec;
    std::filesystem::remove(ini, ec);
    check(named == 3,
          "three short names given, the one for a picture not loaded left out (" + std::to_string(named) + ")");
    check(store.get("short_a") == store.get("long/a") && store.get("tail") == store.get("long/a"),
          "a short name, and a short name for a short name, is the picture itself: no copy");
    check(same_picture(store.get("short_b"), &b) && store.get("nothing") == nullptr && !store.has("nothing") &&
              store.has("short_a") && store.has("tail"),
          "a short name wins over a picture of the same name, as the copy put over it did");
    const std::vector<std::string> names = store.names();
    check(store.size() == 5 && names == std::vector<std::string>{ "long/a", "long/b", "short_a", "short_b", "tail" },
          "size and names count each name once, short names included");

    // A put after the names: what each name shows is what it showed when the
    // names were copies.
    TemplateStore over = store;
    Frame other = blank_frame(6, 4);
    paint(other, 4, 2, 90, 90, 90);
    over.put("long/a", other);
    check(same_picture(over.get("long/a"), &other) && same_picture(over.get("short_a"), &a) &&
              same_picture(over.get("tail"), &a) && same_picture(over.get("short_b"), &b) && over.size() == 5,
          "a picture put over one a short name shows leaves the short name showing the old one");
    TemplateStore renamed = store;
    renamed.put("short_a", other);
    check(same_picture(renamed.get("short_a"), &other) && same_picture(renamed.get("long/a"), &a) &&
              same_picture(renamed.get("tail"), &a),
          "a picture put under a short name replaces only that name");
    check(store.get("short_a") == store.get("long/a") && same_picture(store.get("short_a"), &a),
          "the store copied from is untouched by the copies' puts");
}

void test_shared_pictures() {
    section("The game's pictures loaded once for the program, handed to every job (2026-11-16)");
    const std::string map = exe_relative("configs/templates.ini");
    std::shared_ptr<const TemplateStore> one = TemplateStore::shared_best(map);
    if (!one || one->size() == 0) {
        skip("no game pictures beside the tests\n");
        return;
    }
    const std::shared_ptr<const TemplateStore> again = TemplateStore::shared_best(map);
    check(again.get() == one.get(), "a second job asking while the first holds the set gets the same one");
    {
        const TemplateStore* got[2] = { nullptr, nullptr };
        std::thread t1([&] { got[0] = TemplateStore::shared_best(map).get(); });
        std::thread t2([&] { got[1] = TemplateStore::shared_best(map).get(); });
        t1.join();
        t2.join();
        check(got[0] == one.get() && got[1] == one.get(), "and so do two threads asking at once");
    }

    // The same pictures, under the same names, as a job's own load gave.
    TemplateStore own;
    const bool loaded = own.load_best(map);
    bool same = loaded && own.size() == one->size() && own.names() == one->names() && own.source() == one->source();
    std::string first_odd;
    if (same)
        for (const std::string& name : own.names())
            if (!same_picture(own.get(name), one->get(name))) {
                same = false;
                first_odd = name;
                break;
            }
    check(same, "the shared set holds what load_best loads, picture for picture" +
                    (first_odd.empty() ? std::string() : " (not " + first_odd + ")"));

    // The name map written again: a job starting then gets the names as they
    // are, and one that holds the set keeps its own.
    std::string text;
    {
        std::ifstream in(map, std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    if (text.find("[Templates]") == std::string::npos) text += "\n[Templates]\n";
    const std::string real = one->names().front();
    const std::string ini = temp_ini("gobot_test_shared_names.ini", text + "\ns1_shared_test = " + real + "\n");
    if (ini.empty()) {
        skip("no temp folder to write a name map in\n");
        return;
    }
    const std::shared_ptr<const TemplateStore> named = TemplateStore::shared_best(ini);
    const std::shared_ptr<const TemplateStore> named_again = TemplateStore::shared_best(ini);
    check(named.get() != one.get() && named_again.get() == named.get() && named->has("s1_shared_test") &&
              !one->has("s1_shared_test") && named->get("s1_shared_test") == named->get(real),
          "another name map, another set, shared in its turn");
    temp_ini("gobot_test_shared_names.ini", text + "\ns1_shared_test = " + real + "\ns1_shared_again = " + real + "\n");
    const std::shared_ptr<const TemplateStore> changed = TemplateStore::shared_best(ini);
    check(changed.get() != named.get() && changed->has("s1_shared_again") && !named->has("s1_shared_again") &&
              named->has("s1_shared_test"),
          "the name map written again: loaded again for the job asking, the held one left as it was");
    std::error_code ec;
    std::filesystem::remove(ini, ec);
}

void test_shared_wiring() {
    section("The GUI's jobs use the one set of pictures (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string popups = source("/apps/gobot_gui/simple_popups.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string closer = source("/src/popup_closer.cpp");
    if (gui.empty() || popups.empty() || view.empty() || closer.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto count = [](const std::string& text, const std::string& part) {
        std::size_t n = 0;
        for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++n;
        return n;
    };
    const std::string own_load = ".load_best(exe_relative(\"configs/templates.ini\"))";
    check(count(gui, own_load) == 0 && count(popups, own_load) == 0 && count(view, own_load) == 0 &&
              count(gui, "AutoLogin::load_pictures(") == 0 && count(view, "AutoLogin::load_pictures(") == 0,
          "no job loads a set of its own any more");
    // The keeper, Tổ đội, Hồi sinh, Dọn rác, Tự chat, BẮT ĐẦU's look, and the
    // two looks after a stop; Tự đóng thông báo and its Thử xem; the trash
    // tab's bag picture and the login tab's look.
    check(count(gui, "TemplateStore::shared_best(exe_relative(\"configs/templates.ini\"))") == 8 &&
              count(popups, "TemplateStore::shared_best(exe_relative(\"configs/templates.ini\"))") == 2 &&
              count(view, "TemplateStore::shared_best(exe_relative(\"configs/templates.ini\"))") == 2,
          "each of them asks for the shared one");
    check(count(popups, "eyes.load(std::move(store), exe_relative(\"configs\"));") == 2 &&
              has(closer, "held_ = std::move(store);"),
          "the popups' eyes hold the shared set they look with");
#else
    skip("the source tree is not known to this build\n");
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
    test_matcher_kept_buffers();
    test_store_short_names();
    test_shared_pictures();
    test_shared_wiring();
}
