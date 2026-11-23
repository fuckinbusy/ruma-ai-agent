#pragma once

// Queues for passing values between threads.
//
// MpmcQueue — bounded, lock-free, many producers and many consumers (Dmitry Vyukov's bounded
//   MPMC queue). Used where losing the slot is acceptable to report (try_push returns false):
//   the job pool, the logger.
// Mailbox — unbounded, many producers and one consumer, batch drain; the consumer is woken
//   once per batch through a callback (e.g. PostMessageW to the UI thread).

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace core {

#pragma warning(push)
#pragma warning(disable : 4324) // padded due to alignment specifier: intended (no false sharing)
template <class T>
class MpmcQueue {
public:
    explicit MpmcQueue(std::size_t capacity)
        : capacity_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)),
          mask_(capacity_ - 1), cells_(std::make_unique<Cell[]>(capacity_)) {
        for (std::size_t i = 0; i < capacity_; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~MpmcQueue() {
        T item;
        while (try_pop(item)) {
        }
    }

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    [[nodiscard]] auto try_push(T&& value) -> bool {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        std::construct_at(cell->ptr(), std::move(value));
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] auto try_pop(T& out) -> bool {
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = std::move(*cell->ptr());
        std::destroy_at(cell->ptr());
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] auto capacity() const noexcept -> std::size_t { return capacity_; }

private:
    struct alignas(64) Cell {
        std::atomic<std::size_t> sequence;
        alignas(T) unsigned char storage[sizeof(T)];
        auto ptr() noexcept -> T* { return std::launder(reinterpret_cast<T*>(storage)); }
    };

    std::size_t capacity_;
    std::size_t mask_;
    std::unique_ptr<Cell[]> cells_;
    alignas(64) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(64) std::atomic<std::size_t> dequeue_pos_{0};
};
#pragma warning(pop)

template <class T>
class Mailbox {
public:
    // `wake` runs on the posting thread, outside the lock, possibly on several producer threads
    // at once: it must be thread-safe and cheap (PostMessageW, a semaphore release).
    explicit Mailbox(std::move_only_function<void()> wake) : wake_(std::move(wake)) {}

    void post(T value) {
        bool wake = false;
        {
            std::scoped_lock lock{mutex_};
            pending_.push_back(std::move(value));
            if (!wake_requested_) {
                wake_requested_ = true;
                wake = true;
            }
        }
        if (wake && wake_) {
            wake_();
        }
    }

    // Moves everything posted so far into `out` (cleared first). Swapping the vectors keeps
    // both capacities, so steady-state traffic does not allocate.
    auto drain(std::vector<T>& out) -> std::size_t {
        out.clear();
        std::scoped_lock lock{mutex_};
        std::swap(out, pending_);
        wake_requested_ = false;
        return out.size();
    }

private:
    std::mutex mutex_;
    std::vector<T> pending_;
    bool wake_requested_ = false;
    std::move_only_function<void()> wake_;
};

} // namespace core
