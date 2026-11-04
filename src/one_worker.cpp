#include "gobot/one_worker.hpp"
#include "gobot/load_meter.hpp"
#include "gobot/logger.hpp"

#include <exception>
#include <utility>

namespace gobot {

OneWorker::OneWorker(std::string name) : name_(std::move(name)) {}

OneWorker::~OneWorker() { stop(); }

bool OneWorker::post(std::function<void()> job) {
    if (!job) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || jobs_.size() >= kMaxJobs) return false;
    jobs_.push_back(std::move(job));
    // Started with the first job: a GoBot that never captures has no thread
    // standing idle for it.
    if (!thread_.joinable()) thread_ = std::thread(&OneWorker::loop, this);
    wake_.notify_one();
    return true;
}

bool OneWorker::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_job_ || !jobs_.empty();
}

void OneWorker::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return !running_job_ && jobs_.empty(); });
}

void OneWorker::stop() {
    std::thread ending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        // What has not begun never will: a capture started as GoBot closes
        // would be left running under it.
        jobs_.clear();
        ending = std::move(thread_);
    }
    wake_.notify_all();
    if (ending.joinable()) ending.join();
    idle_.notify_all();
}

std::size_t OneWorker::done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_;
}

void OneWorker::loop() {
    name_this_thread(name_.c_str());
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) return;      // stopping, and nothing left that may run
            job = std::move(jobs_.front());
            jobs_.pop_front();
            running_job_ = true;
        }
        // A job that throws is said, and the next one still runs: the thread
        // is all there is to start the capture with.
        try {
            job();
        } catch (const std::exception& e) {
            GB_ERROR("%s: a job failed: %s", name_.c_str(), e.what());
        } catch (...) {
            GB_ERROR("%s: a job failed", name_.c_str());
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_job_ = false;
            ++done_;
        }
        idle_.notify_all();
    }
}

} // namespace gobot
