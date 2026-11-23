#pragma once

#include <cstdint>

namespace core::time {

// Monotonic nanoseconds (QueryPerformanceCounter); only differences are meaningful.
auto now_ns() noexcept -> std::int64_t;

class Stopwatch {
public:
    Stopwatch() noexcept;
    [[nodiscard]] auto elapsed_ns() const noexcept -> std::int64_t;
    [[nodiscard]] auto elapsed_ms() const noexcept -> double;
    void restart() noexcept;

private:
    std::int64_t start_;
};

} // namespace core::time
