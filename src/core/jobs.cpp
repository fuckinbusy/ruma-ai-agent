#include "core/jobs.hpp"

#include <algorithm>

namespace core::jobs {

Pool::Pool(unsigned threads) {
    if (threads == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        threads = hw > 1 ? hw - 1 : 1;
    }
    threads_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        threads_.emplace_back([this] { worker_loop(); });
    }
}

Pool::~Pool() {
    stopping_.store(true, std::memory_order_release);
    // One extra permit per worker: a worker exits when it holds a permit, finds no job and
    // nothing is pending.
    available_.release(static_cast<std::ptrdiff_t>(threads_.size()));
    threads_.clear(); // joins
}

auto Pool::submit(Job job) -> bool {
    pending_.fetch_add(1, std::memory_order_acq_rel);
    if (!queue_.try_push(std::move(job))) {
        pending_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    available_.release();
    return true;
}

auto Pool::run_one_pending() -> bool {
    if (!available_.try_acquire()) { // take the job's permit too, or a worker would spin on it
        return false;
    }
    Job job;
    if (queue_.try_pop(job)) {
        pending_.fetch_sub(1, std::memory_order_acq_rel);
        job();
        return true;
    }
    available_.release();
    return false;
}

void Pool::worker_loop() {
    for (;;) {
        available_.acquire();
        Job job;
        if (queue_.try_pop(job)) {
            pending_.fetch_sub(1, std::memory_order_acq_rel);
            job();
            continue;
        }
        if (stopping_.load(std::memory_order_acquire) &&
            pending_.load(std::memory_order_acquire) == 0) {
            return;
        }
        // A job is pending but its producer has not finished publishing the slot yet (or the
        // permit belongs to a job another worker took first): hand the permit back and retry.
        available_.release();
        std::this_thread::yield();
    }
}

void parallel_for(Pool& pool, std::size_t count, std::size_t grain,
                  const std::function<void(std::size_t begin, std::size_t end)>& body) {
    if (count == 0) {
        return;
    }
    grain = std::max<std::size_t>(grain, 1);
    const std::size_t chunks = (count + grain - 1) / grain;
    std::atomic<std::size_t> next{0};
    const auto drain = [&] {
        for (std::size_t c = next.fetch_add(1, std::memory_order_relaxed); c < chunks;
             c = next.fetch_add(1, std::memory_order_relaxed)) {
            const std::size_t begin = c * grain;
            body(begin, std::min(begin + grain, count));
        }
    };

    WaitGroup helpers;
    const std::size_t wanted = std::min<std::size_t>(pool.thread_count(), chunks - 1);
    for (std::size_t i = 0; i < wanted; ++i) {
        helpers.add();
        if (!pool.submit([&] {
                drain();
                helpers.done();
            })) {
            helpers.done(); // queue full: the caller does the work instead
        }
    }
    drain();
    // Every chunk is claimed now; helpers still running are on their last chunk, and helpers
    // not started yet will find nothing. Run queued jobs (possibly our own helpers) instead of
    // blocking: if every worker is inside a parallel_for, nobody else would run them.
    while (!helpers.is_done()) {
        if (!pool.run_one_pending()) {
            std::this_thread::yield();
        }
    }
}

} // namespace core::jobs
