#pragma once

// app::Workers and dropped_picture — the pictures pasted or dropped on the window, decoded off its
// thread (phase 9).

#include "app/details.hpp"
#include "core/error.hpp"
#include "tools/image.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace app {

// Threads for the window's short jobs: a finished one is joined (at once) when the next starts, a
// running one is never waited for — the window's thread goes on. The end joins them all.
class Workers {
public:
    template <class F>
    void start(F&& job) {
        std::erase_if(threads_, [](const Worker& w) { return w.done->load(std::memory_order_acquire); });
        auto done = std::make_shared<std::atomic<bool>>(false);
        threads_.push_back({done, std::jthread{[done, job = std::forward<F>(job)]() mutable {
                                job();
                                done->store(true, std::memory_order_release); // the last thing it does
                            }}});
    }
    [[nodiscard]] auto running() const -> std::size_t {
        return static_cast<std::size_t>(std::ranges::count_if(threads_, [](const Worker& w) { return !w.done->load(std::memory_order_acquire); }));
    }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return threads_.size(); }

private:
    struct Worker {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;
    };
    std::vector<Worker> threads_;
};

inline constexpr std::size_t kMaxDroppedPicture = 50u << 20;

// A picture file dropped on the window, for the model (its path kept): its size looked at before a
// byte is read; past kMaxDroppedPicture, unreadable or not a picture, an error.
auto dropped_picture(const std::string& path) -> core::Result<tools::ImageOut>;

// An attached file's details, read in the background (phase 10): its size, a PDF's pages, a
// workbook's sheets — never a whole document; a picture's pixels come with its decoding. Empty
// when it cannot be read.
auto file_details(const std::string& path) -> std::string;
// The extension of a path, lower case, without its dot ("" when none).
auto extension_of(std::string_view path) -> std::string;

} // namespace app
