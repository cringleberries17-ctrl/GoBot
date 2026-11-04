#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gobot {

/// One named thread that runs the jobs posted to it, one at a time, in the
/// order they came.
///
/// For the work that must never hold up GoBot's own thread, the one that
/// reads F10, F11 and F12: starting and stopping the capture, and the looks
/// at Windows' socket tables, Npcap's service and the adapters that come
/// before a start. A capture stopped on GoBot's thread joined two threads
/// there, and a look at the socket tables in a busy VMware guest took tens of
/// milliseconds.
///
/// At most kMaxJobs wait at once: post() refuses one more, and the caller
/// tries again later -- a job that waits behind four others is out of date by
/// the time it runs. The thread starts with the first job posted. Nothing
/// here knows what a job does, so it is tested with plain lambdas.
class OneWorker {
public:
    static constexpr std::size_t kMaxJobs = 4;

    /// `name` is what the thread is called in the Load line
    /// (name_this_thread) and in Process Explorer.
    explicit OneWorker(std::string name);
    /// stop().
    ~OneWorker();

    OneWorker(const OneWorker&) = delete;
    OneWorker& operator=(const OneWorker&) = delete;

    /// Queue `job`, to run after those queued before it. False -- and
    /// nothing queued -- when kMaxJobs already wait, or after stop().
    bool post(std::function<void()> job);

    /// Whether a job waits or runs now.
    bool busy() const;

    /// Wait until no job waits or runs. At once after stop(). Never from a
    /// job itself, which would wait for its own end.
    void wait_idle();

    /// The job running now is let finish and the thread is joined; the jobs
    /// still waiting are dropped, and nothing posted afterwards runs. For
    /// GoBot's close: no capture is started after it has begun to close.
    void stop();

    /// How many jobs have run to their end, for the tests.
    std::size_t done() const;

private:
    void loop();

    std::string                       name_;
    mutable std::mutex                mutex_;
    std::condition_variable           wake_;      ///< a job posted, or stop()
    std::condition_variable           idle_;      ///< a job ended with none left
    std::deque<std::function<void()>> jobs_;
    bool                              running_job_ = false;
    bool                              stopping_ = false;
    std::size_t                       done_ = 0;
    std::thread                       thread_;
};

} // namespace gobot
