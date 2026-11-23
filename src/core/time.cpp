#include "core/time.hpp"

#include <windows.h>

namespace core::time {
namespace {

auto frequency() noexcept -> std::int64_t {
    static const std::int64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

} // namespace

auto now_ns() noexcept -> std::int64_t {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const std::int64_t f = frequency();
    // Split to avoid overflow of ticks * 1e9 for long uptimes.
    return (t.QuadPart / f) * 1'000'000'000 + (t.QuadPart % f) * 1'000'000'000 / f;
}

Stopwatch::Stopwatch() noexcept : start_(now_ns()) {}

auto Stopwatch::elapsed_ns() const noexcept -> std::int64_t {
    return now_ns() - start_;
}

auto Stopwatch::elapsed_ms() const noexcept -> double {
    return static_cast<double>(elapsed_ns()) / 1'000'000.0;
}

void Stopwatch::restart() noexcept {
    start_ = now_ns();
}

} // namespace core::time
