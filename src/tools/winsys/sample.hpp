#pragma once

// tools::winsys::sample_processes — what each process used, second by second, over a window
// (phase 13a § 1, § 3): processor from its times, the graphics card from PDH's GPU Engine counters.

#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace tools::winsys {

struct Sample {
    std::uint32_t pid = 0;
    std::string name;
    std::wstring path;          // its image; empty when not readable
    std::vector<float> cpu;     // percent of all processors, a point a second
    std::vector<float> gpu;     // percent of the busiest engine, a point a second
    std::uint64_t memory = 0;   // working set at the end
    bool windowless = true;     // no visible top-level window
};

// The top 50 by processor, then by memory; `window` 1..60 s; a stop ends it early with what it has.
// `tick`: after each second, how many of the window's have passed.
[[nodiscard]] auto sample_processes(std::chrono::seconds window, std::stop_token stop,
                                    const std::function<void(int second, int of)>& tick = {}) -> std::vector<Sample>;

} // namespace tools::winsys
