#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace gobot {

// ---- the "Load:" line ----
//
// Every [Performance] load_log_s seconds (a minute as it ships) gobot_gui
// writes one entry saying how hard GoBot works the PC: its CPU, thread by
// thread; its window's frames; its screen grabs, owner by owner, and what
// BitBlt and PrintWindow cost them; the readers; the packets; the log; memory
// and handles; the front keeper; and what runs. A player in a VMware guest who
// says GoBot makes the VM "nặng và giật" sends the log, and the line says where
// the time goes -- measured, not guessed.
//
// Everything here is counted as it happens, from any thread, with atomics, and
// read once a minute: two clock reads per grab and per frame, nothing more.
// load_report(), which turns two snapshots into the entry, makes no Windows
// call, so the tests give it numbers of their own; so does ThreadLedger.

/// Names the calling thread, for the Load line and for Process Explorer: the
/// thread is registered, so that the CPU it uses from now on is counted under
/// `name`, and Windows is told its name ("GoBot <name>", SetThreadDescription,
/// looked up by name in kernel32.dll, and in KernelBase.dll, the only one that
/// has it on 10 1607 and Server/LTSB 2016: a Windows older than 1607 has none,
/// and GoBot still runs there). Called on the first line of each worker loop.
/// Naming the same thread again renames it, its CPU so far kept under the old
/// name; a thread that has ended is dropped at the next snapshot, its CPU kept
/// under its name. `name` is copied.
void name_this_thread(const char* name);

/// Microseconds on the high-resolution clock (QueryPerformanceCounter): for
/// timing a piece of work, never for the time of day.
std::uint64_t load_clock_us();

/// A count of something, and how long it took, added to from any thread.
class LoadTimer {
public:
    struct Sample {
        std::uint64_t count = 0;      ///< since the start
        std::uint64_t total_us = 0;   ///< since the start
        std::uint64_t max_us = 0;     ///< the longest since the last take()
    };
    void add_us(std::uint64_t us);
    /// One more, timed from `from_us` (load_clock_us()) to now.
    void add_since(std::uint64_t from_us);
    /// The counts, and the longest since the last take(), which starts the
    /// longest again.
    Sample take();

private:
    std::atomic<std::uint64_t> count_{ 0 };
    std::atomic<std::uint64_t> total_us_{ 0 };
    std::atomic<std::uint64_t> max_us_{ 0 };
};

/// One owner's screen grabs: a long-lived reader (the vitals, the detector)
/// or a job's own capture (the shop's, the bag's), by the label its
/// ScreenCapture was given (set_label). Counted into by ScreenCapture::grab.
struct GrabOwner {
    std::atomic<std::uint64_t> grabs{ 0 };      ///< frames handed out
    LoadTimer                  bitblt;          ///< copies off the screen
    LoadTimer                  print_window;    ///< PrintWindow asked
    std::atomic<std::uint64_t> recopies{ 0 };   ///< the screen copied again after a blank PrintWindow
    std::atomic<std::uint64_t> points{ 0 };     ///< WindowFromPoint asked, judging such a copy
};

/// A GrabOwner's counts as read at one moment, with its name.
struct GrabCounts {
    std::string       owner;
    std::uint64_t     grabs = 0;
    LoadTimer::Sample bitblt;
    LoadTimer::Sample print_window;
    std::uint64_t     recopies = 0;
    std::uint64_t     points = 0;
};

/// Every owner of grabs, by its label: a table that never moves, so that a
/// capture keeps a reference to its own and counts with no lock. Past
/// kMaxOwners - 1 labels, the rest share "other", which keeps the last place.
class GrabOwners {
public:
    static constexpr int kMaxOwners = 24;
    /// The owner called `label`, found or made: null or empty is "other".
    GrabOwner& owner(const char* label);
    /// Each owner's counts now, in the order they were first asked for. The
    /// longest times start again.
    std::vector<GrabCounts> take();

private:
    std::mutex  mutex_;
    std::string names_[kMaxOwners];
    GrabOwner   owners_[kMaxOwners];
    int         used_ = 0;
};

/// The process's own, which every ScreenCapture counts into.
GrabOwners& grab_owners();

/// What else GoBot counts for the Load line, process-wide.
struct LoadMeters {
    /// The detector's work on a frame: the plates, its own character's name,
    /// the minimap and the camera -- timed, since its Cost has counts only.
    LoadTimer detect;
    /// The front keeper's tries to bring the game forward, and how long each
    /// held GoBot's own thread, the one that reads F12.
    LoadTimer front_focus;
    /// The vitals reader looking for its HUD landmark (a template search over
    /// the top-left 560x280), which it does on every read while it is lost.
    std::atomic<std::uint64_t> anchor_searches{ 0 };
};
LoadMeters& load_meters();

/// The CPU (kernel and user) used under a name since its threads were named:
/// every thread that went by it, ended ones included.
struct ThreadCpu {
    std::string   name;
    std::uint64_t cpu_100ns = 0;
    int           alive = 0;    ///< of those threads, how many still run
};

/// The bookkeeping behind name_this_thread, on its own so that the tests can
/// drive it with threads of their own making: what a thread has used is asked
/// of the probe's `cpu_100ns`, whether it still runs of `alive`, and a handle
/// done with is given to `close`. Each takes the handle named() was given.
/// Thread-safe.
class ThreadLedger {
public:
    struct Probe {
        std::function<std::uint64_t(void*)> cpu_100ns;
        std::function<bool(void*)>          alive;
        std::function<void(void*)>          close;
    };
    /// Threads ended are dropped when this many are tracked, should no
    /// snapshot come to drop them: a GoBot with the Load line off, or a test
    /// run that starts and stops workers by the hundred.
    static constexpr std::size_t kPruneAt = 64;

    explicit ThreadLedger(Probe probe);
    ~ThreadLedger();
    ThreadLedger(const ThreadLedger&) = delete;
    ThreadLedger& operator=(const ThreadLedger&) = delete;

    /// Thread `tid`, whose `handle` is the ledger's to close from now on, is
    /// called `name`, and its CPU from here on is counted under it.
    void named(const std::string& name, std::uint32_t tid, void* handle);
    /// Every name's CPU so far, in the order the names were first given;
    /// threads that have ended are dropped, their CPU kept.
    std::vector<ThreadCpu> totals();
    /// Threads tracked now.
    std::size_t tracked();

private:
    struct Entry {
        std::string   name;
        std::uint32_t tid = 0;
        void*         handle = nullptr;
        std::uint64_t base = 0;     ///< its CPU when it was named
    };
    ThreadCpu& retired(const std::string& name);    ///< under mutex_
    void       drop_ended();                         ///< under mutex_

    Probe                  probe_;
    std::mutex             mutex_;
    std::vector<Entry>     live_;
    std::vector<ThreadCpu> retired_;    ///< per name, the CPU of its threads ended or renamed
};

/// The process as Windows counts it.
struct ProcessLoad {
    std::uint64_t cpu_100ns = 0;        ///< kernel and user, every thread there has been
    std::uint64_t working_set = 0;      ///< bytes
    std::uint64_t private_bytes = 0;    ///< bytes
    std::uint64_t page_faults = 0;      ///< since the start
    unsigned      gdi_objects = 0;
    unsigned      user_objects = 0;
    unsigned      handles = 0;
};

/// Passes of gobot_gui's loop further apart than this are counted as late:
/// two of Windows' 15.6 ms clock ticks. tick() reads F12 and the other keys
/// once a pass, so a key tapped between two late passes can be missed.
constexpr double kLatePassMs = 32.0;

/// Everything the Load line is made from, at one moment: counts since the
/// start, which two snapshots a while apart turn into rates, gauges as they
/// are now, and the longest times since the snapshot before.
struct LoadSnapshot {
    std::uint64_t          at_ms = 0;          ///< now_ms()
    int                    logical_cpus = 1;
    ProcessLoad            process;
    std::vector<ThreadCpu> threads;

    // GoBot's window, its loop (GuiApp::LoopCounters).
    bool          gui = false;                 ///< these are filled in
    std::uint64_t passes = 0;                  ///< tick() each
    double        pass_gap_max_ms = 0.0;       ///< the longest between two, since the snapshot before
    std::uint64_t passes_late = 0;             ///< more than kLatePassMs after the one before
    std::uint64_t frames = 0;
    std::uint64_t occluded_skips = 0;
    double        build_ms = 0.0;              ///< all frames together
    double        build_max_ms = 0.0;          ///< since the snapshot before
    double        present_ms = 0.0;
    double        present_max_ms = 0.0;

    std::vector<GrabCounts> grabs;

    bool              vitals_running = false;
    std::uint64_t     vitals_reads = 0;
    int               vitals_period_ms = 0;    ///< the gap asked between reads; 0 unknown
    std::uint64_t     anchor_searches = 0;
    bool              detector_running = false;
    std::uint64_t     detector_frames = 0;
    LoadTimer::Sample detect;
    bool              capture_running = false;
    std::uint64_t     packets = 0;
    std::uint64_t     pcap_dropped = 0;
    std::uint64_t     log_lines = 0;
    std::uint64_t     log_bytes = 0;
    LoadTimer::Sample front;

    /// The frames the capture handed over, by kind (FrameTally::take), for
    /// a "frames:" line under "reads:"; empty for none in this entry -- the
    /// window fills it every ten minutes, at a hunt's start and end, and when
    /// what the game sends changes. `frames_filtered`: Apply's opcode filter
    /// was on, so only the listed opcodes were counted.
    std::string frame_kinds;
    bool        frames_filtered = false;

    std::string jobs;                          ///< "hunt+macro+popups"; empty is none
    std::string tab;                           ///< the tab open in GoBot's window
    bool        light = false;                 ///< light mode ("Nhẹ máy")
};

/// The process-wide part of a snapshot: the clock, the CPUs, every named
/// thread and the process, the grabs, the detector, the vitals' anchor
/// searches, the front keeper and the log. The longest times start again.
/// What only the window knows -- its loop, the readers' and the capture's own
/// counts, the jobs -- is the caller's to fill in. A lock and a GetThreadTimes
/// per named thread, a few cheap queries of the process: no snapshot of the
/// system's threads, which takes every thread on the PC and can take tens of
/// milliseconds in a busy guest, on the thread that reads F12.
void take_load_snapshot(LoadSnapshot& s);

/// The Load entry from two snapshots a while apart, `before` first: rates
/// over the time between them, averages, and the longest times in `now`. A
/// count lower in `now` than in `before` was started again meanwhile (a reader
/// stopped and started) and is taken from zero. Several lines, the first
/// starting "<title>: ". No Windows call.
std::string load_report(const LoadSnapshot& before, const LoadSnapshot& now, const char* title = "Load");

} // namespace gobot
