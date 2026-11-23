#pragma once

// core::jobs — a fixed thread pool for CPU work (tool execution, directory scans, text shaping).
// Workers sleep on a semaphore when idle: an idle pool costs no CPU.

#include "core/queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <semaphore>
#include <thread>
#include <vector>

namespace core::jobs {

using Job = std::move_only_function<void()>;

// Counts outstanding work; wait() blocks (WaitOnAddress via std::atomic::wait) until zero.
class WaitGroup {
public:
    void add(std::int64_t n = 1) noexcept { count_.fetch_add(n, std::memory_order_relaxed); }
    void done() noexcept {
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            count_.notify_all();
        }
    }
    [[nodiscard]] auto is_done() const noexcept -> bool {
        return count_.load(std::memory_order_acquire) == 0;
    }
    void wait() noexcept {
        for (std::int64_t v = count_.load(std::memory_order_acquire); v != 0;
             v = count_.load(std::memory_order_acquire)) {
            count_.wait(v, std::memory_order_acquire);
        }
    }

private:
    std::atomic<std::int64_t> count_{0};
};

class Pool {
public:
    // 0 threads: hardware_concurrency - 1 (the caller's thread keeps a core), at least 1.
    explicit Pool(unsigned threads = 0);
    // Runs every job already queued (including ones those jobs submit), then joins.
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // False when the queue (4096 jobs) is full; the job is not run.
    [[nodiscard]] auto submit(Job job) -> bool;
    // Runs one queued job on the calling thread, if there is one. Anything that waits for pool
    // work (parallel_for) helps this way instead of blocking, so a job that waits for other
    // jobs cannot deadlock a pool whose workers are all busy.
    auto run_one_pending() -> bool;
    [[nodiscard]] auto thread_count() const noexcept -> unsigned {
        return static_cast<unsigned>(threads_.size());
    }

private:
    void worker_loop();

    MpmcQueue<Job> queue_{4096};
    std::counting_semaphore<> available_{0};
    std::atomic<std::int64_t> pending_{0}; // submitted and not yet taken
    std::atomic<bool> stopping_{false};
    std::vector<std::jthread> threads_;
};

// Calls body(begin, end) over [0, count) in chunks of `grain` indices, on the pool and on the
// calling thread; returns when every chunk is done. Safe to call from inside a pool job: while
// waiting, the caller runs queued jobs itself.
void parallel_for(Pool& pool, std::size_t count, std::size_t grain,
                  const std::function<void(std::size_t begin, std::size_t end)>& body);

} // namespace core::jobs
