#include "gobot/load_meter.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace gobot {

namespace {

const char* const kOther = "other";

/// What a count rose by from `before` to `now`. Lower in `now`: started again
/// meanwhile -- a reader stopped and started, a capture opened afresh -- so
/// all of `now` is new.
std::uint64_t rise(std::uint64_t before, std::uint64_t now) { return now >= before ? now - before : now; }
double rise(double before, double now) { return now >= before ? now - before : now; }

std::string fixed(double v, int decimals) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

/// `n` over `seconds`, per second, one decimal.
std::string per_s(double n, double seconds) { return fixed(seconds > 0.0 ? n / seconds : 0.0, 1); }
/// And per minute: whole above ten, one decimal under.
std::string per_min(double n, double seconds) {
    const double v = seconds > 0.0 ? n * 60.0 / seconds : 0.0;
    return fixed(v, v >= 10.0 ? 0 : 1);
}

/// "avg 7.2 ms max 61" for `n` of them taking `total_us` together, the
/// longest `max_us`.
std::string avg_max(std::uint64_t n, std::uint64_t total_us, std::uint64_t max_us) {
    const double avg = n > 0 ? static_cast<double>(total_us) / 1000.0 / static_cast<double>(n) : 0.0;
    return "avg " + fixed(avg, 1) + " ms max " + fixed(static_cast<double>(max_us) / 1000.0, 0);
}

std::uint64_t filetime(const FILETIME& f) {
    return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

std::string megabytes(std::uint64_t bytes) { return std::to_string(bytes / (1024u * 1024u)); }

ThreadLedger& thread_ledger() {
    // Never destroyed: workers may still be naming themselves while the
    // process ends.
    static ThreadLedger* const ledger = new ThreadLedger(ThreadLedger::Probe{
        [](void* h) -> std::uint64_t {
            FILETIME created{}, ended{}, kernel{}, user{};
            if (h == nullptr || !GetThreadTimes(static_cast<HANDLE>(h), &created, &ended, &kernel, &user)) return 0;
            return filetime(kernel) + filetime(user);
        },
        [](void* h) {
            DWORD code = 0;
            return h != nullptr && GetExitCodeThread(static_cast<HANDLE>(h), &code) != FALSE && code == STILL_ACTIVE;
        },
        [](void* h) {
            if (h != nullptr) CloseHandle(static_cast<HANDLE>(h));
        } });
    return *ledger;
}

ProcessLoad read_process_load() {
    ProcessLoad p;
    const HANDLE self = GetCurrentProcess();
    FILETIME created{}, ended{}, kernel{}, user{};
    if (GetProcessTimes(self, &created, &ended, &kernel, &user)) p.cpu_100ns = filetime(kernel) + filetime(user);
    // K32GetProcessMemoryInfo, as psapi.h names it for Windows 7 and later
    // (PSAPI_VERSION 2): in kernel32 itself, so no psapi.lib.
    PROCESS_MEMORY_COUNTERS_EX mem{};
    mem.cb = sizeof mem;
    if (GetProcessMemoryInfo(self, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof mem)) {
        p.working_set = mem.WorkingSetSize;
        p.private_bytes = mem.PrivateUsage;
        p.page_faults = mem.PageFaultCount;
    }
    p.gdi_objects = GetGuiResources(self, GR_GDIOBJECTS);
    p.user_objects = GetGuiResources(self, GR_USEROBJECTS);
    DWORD handles = 0;
    if (GetProcessHandleCount(self, &handles)) p.handles = handles;
    return p;
}

} // namespace

// ------------------------------------------------------------------ threads

void name_this_thread(const char* name) {
    if (name == nullptr || *name == '\0') return;

    // Windows' own name for it, which Process Explorer and a debugger show.
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const SetDescription set_description = [] {
        // kernel32 exports it from Windows 10 1703 on; 1607, and Server and
        // LTSB 2016, have it only in KernelBase.dll.
        for (const wchar_t* dll : { L"kernel32.dll", L"KernelBase.dll" }) {
            const HMODULE kernel = GetModuleHandleW(dll);
            if (kernel == nullptr) continue;
            if (const auto fn = reinterpret_cast<SetDescription>(GetProcAddress(kernel, "SetThreadDescription")))
                return fn;
        }
        return SetDescription{};
    }();
    if (set_description != nullptr) {
        const std::wstring described = utf8_to_wide(std::string("GoBot ") + name);
        set_description(GetCurrentThread(), described.c_str());
    }

    // Its own handle, to ask for its CPU once a minute -- the least access
    // there is, and one that outlives the thread, so its last figures can be
    // read after it ends.
    const DWORD tid = GetCurrentThreadId();
    const HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (handle == nullptr) return;     // its CPU stays with "other"
    thread_ledger().named(name, static_cast<std::uint32_t>(tid), handle);
}

ThreadLedger::ThreadLedger(Probe probe) : probe_(std::move(probe)) {}

ThreadLedger::~ThreadLedger() {
    for (const Entry& e : live_) probe_.close(e.handle);
}

ThreadCpu& ThreadLedger::retired(const std::string& name) {
    for (ThreadCpu& t : retired_)
        if (t.name == name) return t;
    retired_.push_back(ThreadCpu{ name, 0, 0 });
    return retired_.back();
}

void ThreadLedger::drop_ended() {
    for (auto it = live_.begin(); it != live_.end();) {
        if (probe_.alive(it->handle)) {
            ++it;
            continue;
        }
        // Read after it ended: the handle still has its last figures.
        retired(it->name).cpu_100ns += rise(it->base, probe_.cpu_100ns(it->handle));
        probe_.close(it->handle);
        it = live_.erase(it);
    }
}

void ThreadLedger::named(const std::string& name, std::uint32_t tid, void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The same thread named again, as a loop run twice on one thread would:
    // what it has used so far stays with the name it had. A tid of a thread
    // that has ended can be another thread's now, so only a live one counts.
    for (Entry& e : live_) {
        if (e.tid != tid || !probe_.alive(e.handle)) continue;
        const std::uint64_t cpu = probe_.cpu_100ns(e.handle);
        retired(e.name).cpu_100ns += rise(e.base, cpu);
        e.name = name;
        e.base = cpu;
        (void)retired(name);
        probe_.close(handle);
        return;
    }
    if (live_.size() >= kPruneAt) drop_ended();
    (void)retired(name);    // the name's place in the order
    live_.push_back(Entry{ name, tid, handle, probe_.cpu_100ns(handle) });
}

std::vector<ThreadCpu> ThreadLedger::totals() {
    std::lock_guard<std::mutex> lock(mutex_);
    drop_ended();
    std::vector<ThreadCpu> out = retired_;
    for (const Entry& e : live_) {
        for (ThreadCpu& t : out) {
            if (t.name != e.name) continue;
            t.cpu_100ns += rise(e.base, probe_.cpu_100ns(e.handle));
            ++t.alive;
            break;
        }
    }
    return out;
}

std::size_t ThreadLedger::tracked() {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_.size();
}

// -------------------------------------------------------------------- clocks

std::uint64_t load_clock_us() {
    static const std::uint64_t hz = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? static_cast<std::uint64_t>(f.QuadPart) : 1u;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    const std::uint64_t ticks = static_cast<std::uint64_t>(c.QuadPart);
    // In two parts: ticks times a million overflows after a few weeks up.
    return ticks / hz * 1'000'000u + ticks % hz * 1'000'000u / hz;
}

void LoadTimer::add_us(std::uint64_t us) {
    count_.fetch_add(1, std::memory_order_relaxed);
    total_us_.fetch_add(us, std::memory_order_relaxed);
    std::uint64_t was = max_us_.load(std::memory_order_relaxed);
    while (us > was && !max_us_.compare_exchange_weak(was, us, std::memory_order_relaxed)) {
    }
}

void LoadTimer::add_since(std::uint64_t from_us) {
    const std::uint64_t to = load_clock_us();
    add_us(to >= from_us ? to - from_us : 0);
}

LoadTimer::Sample LoadTimer::take() {
    Sample s;
    s.count = count_.load(std::memory_order_relaxed);
    s.total_us = total_us_.load(std::memory_order_relaxed);
    s.max_us = max_us_.exchange(0, std::memory_order_relaxed);
    return s;
}

// --------------------------------------------------------------------- grabs

GrabOwner& GrabOwners::owner(const char* label) {
    const std::string name = label != nullptr && *label != '\0' ? label : kOther;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < used_; ++i)
        if (names_[i] == name) return owners_[i];
    if (used_ < kMaxOwners - 1) {
        names_[used_] = name;
        return owners_[used_++];
    }
    // Full: the rest go with "other", which has the last place kept for it.
    for (int i = 0; i < used_; ++i)
        if (names_[i] == kOther) return owners_[i];
    names_[used_] = kOther;
    return owners_[used_++];
}

std::vector<GrabCounts> GrabOwners::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<GrabCounts> out;
    out.reserve(static_cast<std::size_t>(used_));
    for (int i = 0; i < used_; ++i) {
        GrabOwner& o = owners_[i];
        GrabCounts c;
        c.owner = names_[i];
        c.grabs = o.grabs.load(std::memory_order_relaxed);
        c.bitblt = o.bitblt.take();
        c.print_window = o.print_window.take();
        c.recopies = o.recopies.load(std::memory_order_relaxed);
        c.points = o.points.load(std::memory_order_relaxed);
        out.push_back(std::move(c));
    }
    return out;
}

GrabOwners& grab_owners() {
    static GrabOwners* const owners = new GrabOwners;    // never destroyed, as the ledger
    return *owners;
}

LoadMeters& load_meters() {
    static LoadMeters* const meters = new LoadMeters;
    return *meters;
}

// ------------------------------------------------------------------ snapshot

void take_load_snapshot(LoadSnapshot& s) {
    s.at_ms = now_ms();
    const DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    s.logical_cpus = cpus > 0 ? static_cast<int>(cpus) : 1;
    // The threads before the process: the process's CPU then holds all of
    // theirs, and "other" -- the rest -- never comes out below nothing.
    s.threads = thread_ledger().totals();
    s.process = read_process_load();
    s.grabs = grab_owners().take();
    LoadMeters& m = load_meters();
    s.detect = m.detect.take();
    s.front = m.front_focus.take();
    s.anchor_searches = m.anchor_searches.load(std::memory_order_relaxed);
    s.log_lines = Logger::instance().lines_written();
    s.log_bytes = Logger::instance().bytes_written();
}

// -------------------------------------------------------------------- report

std::string load_report(const LoadSnapshot& before, const LoadSnapshot& now, const char* title) {
    const double wall_s = now.at_ms > before.at_ms ? static_cast<double>(now.at_ms - before.at_ms) / 1000.0 : 0.0;
    std::string out = std::string(title != nullptr && *title != '\0' ? title : "Load") + ": ";
    if (wall_s <= 0.0) return out + "no time between the two looks";
    const std::string indent = "\n      ";

    // ---- CPU: the process, and thread by thread ----
    const int cpus = std::max(1, now.logical_cpus);
    const double process_s = static_cast<double>(rise(before.process.cpu_100ns, now.process.cpu_100ns)) / 1e7;
    out += "last " + fixed(wall_s, 0) + " s | cpu " + fixed(process_s * 100.0 / wall_s / cpus, 0) + "% of " +
           std::to_string(cpus) + (cpus == 1 ? " CPU" : " CPUs") + " | threads, CPU s/min:";
    double named_s = 0.0;
    std::string threads;
    for (const ThreadCpu& t : now.threads) {
        std::uint64_t was = 0;
        for (const ThreadCpu& b : before.threads)
            if (b.name == t.name) was = b.cpu_100ns;
        const double used_s = static_cast<double>(rise(was, t.cpu_100ns)) / 1e7;
        named_s += used_s;
        // A name none of whose threads ran meanwhile says nothing.
        if (t.alive == 0 && used_s <= 0.0) continue;
        threads += (threads.empty() ? " " : ", ") + t.name + " " + fixed(used_s * 60.0 / wall_s, 1);
    }
    // Everything not named: Direct3D's and the drivers' threads, OpenCV's and
    // the thread pool's, std::async looks, and the loops that have no name.
    const double other_s = std::max(0.0, process_s - named_s);
    out += threads + (threads.empty() ? " " : ", ") + "other " + fixed(other_s * 60.0 / wall_s, 1);

    // ---- GoBot's window ----
    if (now.gui) {
        const std::uint64_t frames = rise(before.frames, now.frames);
        out += indent + "gui " + per_s(static_cast<double>(frames), wall_s) + " frames/s";
        if (frames > 0) {
            const double build = rise(before.build_ms, now.build_ms) / static_cast<double>(frames);
            const double present = rise(before.present_ms, now.present_ms) / static_cast<double>(frames);
            out += " (build avg " + fixed(build, 1) + " ms max " + fixed(now.build_max_ms, 0) + ", present avg " +
                   fixed(present, 1) + " ms max " + fixed(now.present_max_ms, 0) + ")";
        }
        out += ", passes " + per_s(static_cast<double>(rise(before.passes, now.passes)), wall_s) +
               "/s (longest gap " + fixed(now.pass_gap_max_ms, 0) + " ms, " +
               std::to_string(rise(before.passes_late, now.passes_late)) + " over " + fixed(kLatePassMs, 0) +
               " ms), occluded skips " + std::to_string(rise(before.occluded_skips, now.occluded_skips));
    }

    // ---- screen grabs, owner by owner ----
    {
        std::uint64_t grabs = 0, recopies = 0, points = 0;
        std::uint64_t blits = 0, blit_us = 0, blit_max = 0;
        std::uint64_t prints = 0, print_us = 0, print_max = 0;
        std::string by_owner, prints_by, recopies_by;
        for (const GrabCounts& g : now.grabs) {
            const GrabCounts* b = nullptr;
            for (const GrabCounts& x : before.grabs)
                if (x.owner == g.owner) b = &x;
            const GrabCounts zero;
            const GrabCounts& was = b != nullptr ? *b : zero;
            const std::uint64_t n = rise(was.grabs, g.grabs);
            const std::uint64_t p = rise(was.print_window.count, g.print_window.count);
            const std::uint64_t r = rise(was.recopies, g.recopies);
            grabs += n;
            recopies += r;
            points += rise(was.points, g.points);
            blits += rise(was.bitblt.count, g.bitblt.count);
            blit_us += rise(was.bitblt.total_us, g.bitblt.total_us);
            blit_max = std::max(blit_max, g.bitblt.max_us);
            prints += p;
            print_us += rise(was.print_window.total_us, g.print_window.total_us);
            print_max = std::max(print_max, g.print_window.max_us);
            if (n > 0) by_owner += (by_owner.empty() ? "" : ", ") + g.owner + " " + per_s(static_cast<double>(n), wall_s);
            if (p > 0) prints_by += (prints_by.empty() ? "" : ", ") + g.owner + " " + std::to_string(p);
            if (r > 0) recopies_by += (recopies_by.empty() ? "" : ", ") + g.owner + " " + std::to_string(r);
        }
        out += indent + "grabs " + per_s(static_cast<double>(grabs), wall_s) + "/s";
        if (!by_owner.empty()) out += " (" + by_owner + ")";
        out += "; BitBlt " + (blits > 0 ? avg_max(blits, blit_us, blit_max) : std::string("none"));
        out += "; PrintWindow " + per_min(static_cast<double>(prints), wall_s) + "/min";
        if (prints > 0) out += " (" + prints_by + ") " + avg_max(prints, print_us, print_max);
        out += ", recopies " + std::to_string(recopies);
        if (recopies > 0) out += " (" + recopies_by + ")";
        out += ", window checks " + per_min(static_cast<double>(points), wall_s) + "/min";
    }

    // ---- the readers, the packets, the log ----
    out += indent + "reads: vitals ";
    if (now.vitals_running) {
        out += per_s(static_cast<double>(rise(before.vitals_reads, now.vitals_reads)), wall_s) + "/s";
        if (now.vitals_period_ms > 0) out += " (every " + std::to_string(now.vitals_period_ms) + " ms)";
        out += ", anchor searches " + std::to_string(rise(before.anchor_searches, now.anchor_searches));
    } else {
        out += "off";
    }
    out += ", detector ";
    if (now.detector_running) {
        out += per_s(static_cast<double>(rise(before.detector_frames, now.detector_frames)), wall_s) + "/s";
        const std::uint64_t looks = rise(before.detect.count, now.detect.count);
        if (looks > 0)
            out += " (look " + avg_max(looks, rise(before.detect.total_us, now.detect.total_us), now.detect.max_us) +
                   ")";
    } else {
        out += "off";
    }
    out += " | packets ";
    if (now.capture_running)
        out += per_s(static_cast<double>(rise(before.packets, now.packets)), wall_s) + "/s, dropped " +
               std::to_string(rise(before.pcap_dropped, now.pcap_dropped));
    else
        out += "off";
    out += " | log " + per_min(static_cast<double>(rise(before.log_lines, now.log_lines)), wall_s) + " lines/min, " +
           std::to_string(rise(before.log_bytes, now.log_bytes) / 1024u) + " KB";

    // ---- memory and handles; the front keeper ----
    const ProcessLoad& p = now.process;
    out += indent + "mem: working set " + megabytes(p.working_set) + " MB, private " + megabytes(p.private_bytes) +
           " MB, page faults " + per_s(static_cast<double>(rise(before.process.page_faults, p.page_faults)), wall_s) +
           "/s, gdi " + std::to_string(p.gdi_objects) + ", user " + std::to_string(p.user_objects) + ", handles " +
           std::to_string(p.handles);
    {
        const std::uint64_t tries = rise(before.front.count, now.front.count);
        out += " | front keeper " + per_min(static_cast<double>(tries), wall_s) + " tries/min";
        if (tries > 0)
            out += ", GoBot's thread held " +
                   fixed(static_cast<double>(rise(before.front.total_us, now.front.total_us)) / 1000.0, 0) +
                   " ms (max " + fixed(static_cast<double>(now.front.max_us) / 1000.0, 0) + ")";
    }

    // ---- what runs ----
    out += indent + "jobs: " + (now.jobs.empty() ? std::string("none") : now.jobs);
    if (!now.tab.empty()) out += ", tab " + now.tab;
    out += std::string(", light ") + (now.light ? "on" : "off");
    return out;
}

} // namespace gobot
