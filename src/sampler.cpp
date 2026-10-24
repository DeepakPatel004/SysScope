#include "syscope.hpp"

#include <stdexcept>
#include <utility>

namespace syscope {

Sampler::Sampler(ProcReader reader, std::chrono::milliseconds interval)
    : reader_(std::move(reader)), interval_(interval) {
    if (interval_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Sampling interval must be positive");
    }
    worker_ = std::thread(&Sampler::run, this);
}

Sampler::~Sampler() {
    request_stop();
    if (worker_.joinable()) { worker_.join(); }
}

void Sampler::request_stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
}

std::optional<Snapshot> Sampler::next(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, timeout, [this] { return pending_ || error_ || stopping_; });
    if (pending_) {
        auto result = std::move(pending_);
        pending_.reset();
        lock.unlock();
        changed_.notify_all();
        return result;
    }
    if (error_) { std::rethrow_exception(error_); }
    return std::nullopt;
}

void Sampler::run() noexcept {
    try {
        CpuUsage usage;
        auto deadline = std::chrono::steady_clock::now();
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Never overwrite an unconsumed sample. Stop interrupts either wait.
                changed_.wait(lock, [this] { return stopping_ || !pending_; });
                if (changed_.wait_until(lock, deadline, [this] { return stopping_; })) { return; }
            }
            // Collection and calculations do not hold the shared-state mutex.
            const auto cpu = usage.update(reader_.cpu());
            Snapshot sample{utc_timestamp(), cpu, reader_.memory()};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) { return; }
                pending_ = std::move(sample);
            }
            changed_.notify_all();
            // Slow collection/output delays the next interval without catch-up bursts.
            deadline = std::chrono::steady_clock::now() + interval_;
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::current_exception();
        }
        changed_.notify_all();
    }
}

} // namespace syscope
